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

#include "aofx_kernels_blur.h"

namespace {

/// Unique across every plugin loaded in one process, which is why it is a
/// reverse-DNS name and not "blur".
constexpr const char* kKernel = "org.aopenfx.blur";
/// And the function inside the blob, which is a different thing entirely.
constexpr const char* kEntry = "blurMain";
constexpr const char* kParamSize = "size";

/// Standard deviations covered before the kernel is cut off.
///
/// Three covers 99.7% of the curve, which is below one code value at eight bits
/// and is where every other implementation stops. Named because it is a
/// decision rather than a constant: it trades a wider kernel against a visible
/// step at the edge of the blur.
constexpr double kRadiusInSigmas = 3.0;

/// CImgBlur's, so the two are interchangeable.
constexpr double kSigmaPerSize = 1.0 / 2.4;

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

/// Fills in where `from` sits relative to `into`.
///
/// The whole reason a Buffer carries a rectangle. Destination pixel (0,0) is at
/// source coordinate `into.rect.x1 - from.rect.x1`, which is negative whenever
/// the output is the larger -- and for a blur it always is.
void relate(BlurUniforms& uniforms, const aofx::Buffer& from,
            const aofx::Buffer& into) {
    uniforms.srcWidth = static_cast<uint32_t>(from.width);
    uniforms.srcHeight = static_cast<uint32_t>(from.height);
    uniforms.srcOffsetX = into.rect.x1 - from.rect.x1;
    uniforms.srcOffsetY = into.rect.y1 - from.rect.y1;
    uniforms.srcStride = static_cast<uint32_t>(from.stride);
    uniforms.dstStride = static_cast<uint32_t>(into.stride);
}

[[nodiscard]] double sigmaFrom(double size) {
    return std::max(size, 0.0) * kSigmaPerSize;
}

[[nodiscard]] int radiusFrom(double sigma) {
    return sigma <= 0.0
               ? 0
               : static_cast<int>(std::lround(std::ceil(sigma * kRadiusInSigmas)));
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

    /// Bigger than the input, by exactly what the filter reaches.
    ///
    /// A blur genuinely creates picture outside its input -- that is what the
    /// soft edge is -- and a host that believes otherwise crops it off. The
    /// number here has to be the same one the kernel uses, or energy is lost
    /// over the edge and it reads as a grade error rather than a filter one.
    [[nodiscard]] aofx::Rect regionOfDefinition(
        double time, const std::vector<aofx::Rect>& inputRods,
        const std::vector<aofx::ParamValue>& params) const override {
        aofx::Rect out = aofx::Effect::regionOfDefinition(time, inputRods, params);
        if (out.isEmpty()) {
            return out;
        }
        double sizeX = 0.0;
        double sizeY = 0.0;
        for (const aofx::ParamValue& value : params) {
            if (value.name == kParamSize) {
                sizeX = value.number(0, 0.0);
                sizeY = value.number(1, sizeX);
            }
        }
        const int growX = radiusFrom(sigmaFrom(sizeX));
        const int growY = radiusFrom(sigmaFrom(sizeY));
        out.x1 -= growX;
        out.x2 += growX;
        out.y1 -= growY;
        out.y2 += growY;
        return out;
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
        return radiusFrom(sigmaFrom(sizeX)) == 0 &&
               radiusFrom(sigmaFrom(sizeY)) == 0;
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

        // Somewhere to put the first pass. Asked of the host rather than
        // allocated, so it comes from the same pool as everything else, counts
        // against the same budget, and is recycled instead of being handed back
        // to the driver -- which at video rates is the difference between a
        // temporary and a stall.
        aofx::Buffer scratch =
            request.gpu->scratch(target->buffer.width, target->buffer.height);
        if (!scratch.isValid()) {
            // No room. Giving up on the frame beats rendering half of it.
            return false;
        }
        // The host hands a scratch at the origin; this one is the output's own
        // shape and stands in for it, so it takes the output's place in the
        // picture. Without that the first pass relates the source to a
        // rectangle at zero and a source that does not start at zero -- a
        // card, a crop, anything smaller than the frame -- lands outside it.
        scratch.rect = target->buffer.rect;

        const double sizeX = request.number(kParamSize, 0.0, 0);
        const double sizeY = request.number(kParamSize, sizeX, 1);

        const aofx::Grid grid{static_cast<uint32_t>(target->buffer.width),
                              static_cast<uint32_t>(target->buffer.height), 1};

        // Along x into the scratch, then along y out of it. One kernel per
        // axis, never averaged into one: a horizontal-only blur is a real thing
        // to ask for, and an effect that quietly averages the two refuses it.
        BlurUniforms uniforms;
        uniforms.width = static_cast<uint32_t>(target->buffer.width);
        uniforms.height = static_cast<uint32_t>(target->buffer.height);

        relate(uniforms, source->buffer, scratch);
        uniforms.stepX = 1;
        uniforms.stepY = 0;
        uniforms.sigma = static_cast<float>(sigmaFrom(sizeX));
        uniforms.radius = static_cast<uint32_t>(radiusFrom(sigmaFrom(sizeX)));
        if (!request.gpu->run(kernel, grid, {source->buffer, scratch}, &uniforms,
                              sizeof(uniforms))) {
            return false;
        }

        // The scratch has the output's own rectangle, so the second pass is
        // a straight walk with no offset at all.
        relate(uniforms, scratch, target->buffer);
        uniforms.stepX = 0;
        uniforms.stepY = 1;
        uniforms.sigma = static_cast<float>(sigmaFrom(sizeY));
        uniforms.radius = static_cast<uint32_t>(radiusFrom(sigmaFrom(sizeY)));
        return request.gpu->run(kernel, grid, {scratch, target->buffer},
                                &uniforms, sizeof(uniforms));
    }
};

}   // namespace

AOFX_EXPORT_EFFECTS(Blur)
