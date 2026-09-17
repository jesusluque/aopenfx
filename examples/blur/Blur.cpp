// Copyright (c) 2026 aopenfx contributors.
//
// A gaussian blur, as an AOFX effect.
//
// The first plugin written against this SDK, and written partly to find out
// what the SDK is missing. Two things it needed and got: somewhere to put the
// first pass, and a region of definition that grows.
//
// WHY THIS RATHER THAN CImgBlur
//
// CImgBlur is CPU code and measures at about 220 ms a frame on a 1920x1080
// plate in an optimised build, against the 40 ms a 25 fps timeline allows.
// Nothing is wrong with it -- it is a recursive filter and costs the same at
// any radius, which is a good design for a CPU -- but it puts a blur in the
// graph and real-time playback out of reach.
//
// The sigma convention is CImgBlur's, deliberately: `sigma = size / 2.4`, so
// that swapping one for the other keeps the look rather than requiring the shot
// to be judged again.
//
// Measured, not asserted, by an equivalence test in the host this was written
// for (not part of this repository), rendering the same graph twice with only
// this node swapped. Over the plate the two agree to
// seven code values at size 8 and two at size 24 -- interchangeable, in the
// sense the word is worth having.
//
// They still differ past the input's edge, and that is not a defect in either:
// they cut their kernels at different distances, so the outer ring of the
// grown region is picture one of them made and the other did not reach. The
// figures are printed by that test rather than hidden behind its bound.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "aofx/Entry.h"

#include "BlurMath.h"
#include "aofx_kernels_blur.h"

namespace {

using namespace aofx_examples::blur;

/// Unique across every plugin loaded in one process, which is why it is a
/// reverse-DNS name and not "blur".
constexpr const char* kKernel = "org.aopenfx.blur";
/// And the function inside the blob, which is a different thing entirely.
constexpr const char* kEntry = "blurMain";

/// Matched byte for byte by `BlurParams` in blur.slang.
struct BlurUniforms {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
    int32_t  srcOffsetX = 0;
    int32_t  srcOffsetY = 0;
    uint32_t srcStride = 0;
    uint32_t dstStride = 0;
    uint32_t radius = 0;
    int32_t  stepX = 0;
    int32_t  stepY = 0;
    float    sigma = 0.0F;
};
static_assert(sizeof(BlurUniforms) == 48, "no padding, on any compiler");

/// Fills in where `from` sits relative to `into`, and what the pass walks.
///
/// The whole reason a Buffer carries a rectangle. Destination pixel (0,0) is at
/// source coordinate `into.rect.x1 - from.rect.x1`, which is negative whenever
/// the destination starts left of or below the source -- and for a blur's
/// output it usually does.
void relate(BlurUniforms& uniforms, const aofx::Buffer& from,
            const aofx::Buffer& into) {
    uniforms.width = static_cast<uint32_t>(into.width);
    uniforms.height = static_cast<uint32_t>(into.height);
    uniforms.srcWidth = static_cast<uint32_t>(from.width);
    uniforms.srcHeight = static_cast<uint32_t>(from.height);
    uniforms.srcOffsetX = into.rect.x1 - from.rect.x1;
    uniforms.srcOffsetY = into.rect.y1 - from.rect.y1;
    uniforms.srcStride = static_cast<uint32_t>(from.stride);
    uniforms.dstStride = static_cast<uint32_t>(into.stride);
}

class Blur final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.blur";
        into.label = "Blur";
        into.grouping = "Filter";
        into.description =
            "A gaussian blur on the GPU. Separable, so its cost is linear in "
            "the radius rather than quadratic.";

        into.inputs.push_back(
            aofx::ClipDesc{"Source", "Source", false, false, false});

        aofx::ParamDesc size;
        size.name = kParamSize;
        size.label = "Size";
        size.hint =
            "Blur diameter in pixels, horizontally and vertically. The "
            "gaussian's standard deviation is size/2.4, the same convention "
            "CImgBlur uses, so swapping one for the other keeps the look.";
        size.type = aofx::ParamType::Double;
        size.dimension = 2;
        size.defaults = {0.0, 0.0};
        size.hardMin = {0.0, 0.0};
        size.displayMax = {100.0, 100.0};
        into.params.push_back(size);
    }

    [[nodiscard]] std::vector<aofx::KernelDesc> kernels() const override {
        return {aofx::KernelDesc{kKernel, kEntry, k_blur, k_blurBytes}};
    }

    /// Bigger than the input, by exactly what the filter reaches at this scale.
    ///
    /// A blur genuinely creates picture outside its input -- that is what the
    /// soft edge is -- and a host that believes otherwise crops it off. The
    /// number here has to be the same one the kernel uses, or energy is lost
    /// over the edge and it reads as a grade error rather than a filter one.
    /// The scaled question, because the size knob is canonical pixels and the
    /// rectangles arrive at the render's scale: answering the unscaled one grew
    /// a half-size render by the full-size reach.
    [[nodiscard]] aofx::Rect regionOfDefinition(
        double time, double scaleX, double scaleY,
        const std::vector<aofx::Rect>& inputRods,
        const std::vector<aofx::ParamValue>& params) const override {
        double sizeX = 0.0;
        double sizeY = 0.0;
        sizeFrom(params, sizeX, sizeY);
        return grown(aofx::Effect::regionOfDefinition(time, inputRods, params),
                     reachAt(sizeX, sizeY, scaleX, scaleY));
    }

    /// The output's rectangle, grown by what the filter reaches.
    ///
    /// A host does not always ask for the whole region of definition: a viewer
    /// asks for the window it shows, a tiled render for one tile. A pixel at the
    /// edge of that rectangle is a weighted sum of neighbours outside it, and
    /// asking only for the rectangle itself hands the kernel transparent black
    /// where those neighbours should be -- a dark border around every tile.
    [[nodiscard]] std::vector<aofx::Rect> regionOfInterest(
        const RegionQuery& query, const aofx::Rect& output) const override {
        static const std::vector<aofx::ParamValue> noParams;
        double sizeX = 0.0;
        double sizeY = 0.0;
        sizeFrom(query.params != nullptr ? *query.params : noParams, sizeX, sizeY);
        const std::size_t inputs =
            query.inputRods != nullptr ? query.inputRods->size() : 1;
        return std::vector<aofx::Rect>(
            std::max<std::size_t>(inputs, 1),
            grown(output, reachAt(sizeX, sizeY, query.scaleX, query.scaleY)));
    }

    /// A blur of nothing is the picture.
    ///
    /// Free to answer and worth answering: the host then passes the input
    /// straight through and never allocates an output, which is most of what
    /// makes scrubbing a graph of defaults cheap. A size of zero is also the
    /// default, so this is the common case rather than a corner of one.
    [[nodiscard]] bool isIdentity(const aofx::RenderRequest& request) const override {
        const double sizeX = request.number(kParamSize, 0.0, 0);
        const double sizeY = request.number(kParamSize, sizeX, 1);
        // At this render's scale, like the regions: a blur too small to reach a
        // neighbour at a proxy scale is a copy there, and says so.
        return reachAt(sizeX, sizeY, request.scaleX, request.scaleY).none();
    }

    [[nodiscard]] bool process(const aofx::RenderRequest& request) override {
        const aofx::InputPlane*  source = request.input("Source");
        const aofx::OutputPlane* target = request.output();
        if (source == nullptr || target == nullptr || request.gpu == nullptr) {
            return false;
        }
        if (!source->buffer.isValid() || !target->buffer.isValid()) {
            return false;
        }

        const aofx::KernelId kernel = request.gpu->load(kKernel);
        if (kernel == aofx::kInvalidKernel) {
            return false;
        }

        const double sizeX = request.number(kParamSize, 0.0, 0);
        const double sizeY = request.number(kParamSize, sizeX, 1);
        const Reach reach = reachAt(sizeX, sizeY, request.scaleX, request.scaleY);

        // Somewhere to put the first pass. Asked of the host rather than
        // allocated, so it comes from the same pool as everything else, counts
        // against the same budget, and is recycled instead of being handed back
        // to the driver -- which at video rates is the difference between a
        // temporary and a stall.
        //
        // Taller than the output by the vertical reach, above and below. The
        // second pass reads `radiusY` rows either side of every output row, and
        // a scratch the output's own shape had no rows there: its top and bottom
        // rows were blurred against transparent black instead of against the
        // picture above and below the window.
        const int scratchHeight = target->buffer.height + 2 * reach.radiusY;
        aofx::Buffer scratch = request.gpu->scratch(target->buffer.width, scratchHeight);
        if (!scratch.isValid()) {
            // No room. Giving up on the frame beats rendering half of it.
            return false;
        }
        // The host hands a scratch at the origin; this one stands for the
        // output's columns and the rows around them, so it takes that place in
        // the picture. Without it the first pass relates the source to a
        // rectangle at zero, and a source that does not start at zero lands
        // outside it.
        scratch.rect = aofx::Rect{target->buffer.rect.x1, target->buffer.rect.y1 - reach.radiusY,
                                  target->buffer.rect.x2, target->buffer.rect.y2 + reach.radiusY};

        // Along x into the scratch, then along y out of it. One kernel per
        // axis, never averaged into one: a horizontal-only blur is a real thing
        // to ask for, and an effect that quietly averages the two refuses it.
        BlurUniforms uniforms;
        relate(uniforms, source->buffer, scratch);
        uniforms.stepX = 1;
        uniforms.stepY = 0;
        uniforms.sigma = static_cast<float>(reach.sigmaX);
        uniforms.radius = static_cast<uint32_t>(reach.radiusX);
        if (!request.gpu->run(kernel,
                              aofx::Grid{uniforms.width, uniforms.height, 1},
                              {source->buffer, scratch}, &uniforms, sizeof(uniforms))) {
            return false;
        }

        // Out of the scratch: the output's row 0 is the scratch's row
        // `radiusY`, so every tap of this pass lands on a row the first pass
        // wrote.
        relate(uniforms, scratch, target->buffer);
        uniforms.stepX = 0;
        uniforms.stepY = 1;
        uniforms.sigma = static_cast<float>(reach.sigmaY);
        uniforms.radius = static_cast<uint32_t>(reach.radiusY);
        return request.gpu->run(kernel,
                                aofx::Grid{uniforms.width, uniforms.height, 1},
                                {scratch, target->buffer}, &uniforms, sizeof(uniforms));
    }
};

}   // namespace

AOFX_EXPORT_EFFECTS(Blur)
