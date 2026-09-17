// Copyright (c) 2026 aopenfx contributors.
//
// A merge, against net.sf.openfx.MergePlugin.
//
// The first AOFX effect with two inputs, and it is the one that made the SDK
// answer a question it had been avoiding. With a single input the host could
// use "the first connected one" for three separate decisions -- what pixel
// format the output takes, what an identity hands back, and which input the
// unselected channels are restored from -- and never be wrong. With two, the
// answer is the background for all three, and nothing in the SDK said so.
//
// ClipDesc::passThrough says it now. Getting it wrong would have been silent:
// restoring the unselected channels from the *foreground* would put the wrong
// picture into the channels nobody asked to change, and it would read as a
// compositing mistake rather than a host one.
//
// The other thing two inputs bring is that they need not be the same size or in
// the same place. A merge is the ordinary case of that -- a small element over
// a full frame -- so the kernel relates all three rectangles rather than
// assuming any of them line up.
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "aofx/Entry.h"

#include "aofx_kernels_merge.h"

namespace {

constexpr const char* kKernel = "org.aopenfx.merge";
constexpr const char* kEntry = "mergeMain";

/// The plugin's own list, in the plugin's own order. The index is what a script
/// records, so this is appended to and never reordered.
const std::array<const char*, 39>& operations() {
    static const std::array<const char*, 39> kAll{
        {"atop", "average", "color", "color-burn", "color-dodge",
         "conjoint-over", "copy", "difference", "disjoint-over", "divide",
         "exclusion", "freeze", "from", "geometric", "grain-extract",
         "grain-merge", "hard-light", "hue", "hypot", "in", "luminosity",
         "mask", "matte", "max", "min", "minus", "multiply", "out", "over",
         "overlay", "pinlight", "plus", "reflect", "saturation", "screen",
         "soft-light", "stencil", "under", "xor"}};
    return kAll;
}

constexpr int kOver = 28;   // the plugin's default, and everyone's

/// Matched byte for byte by `MergeParams` in merge.slang.
struct MergeUniforms {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t aStride = 0;
    uint32_t bStride = 0;
    uint32_t dstStride = 0;
    int32_t  aOffsetX = 0;
    int32_t  aOffsetY = 0;
    int32_t  bOffsetX = 0;
    int32_t  bOffsetY = 0;
    uint32_t aWidth = 0;
    uint32_t aHeight = 0;
    uint32_t bWidth = 0;
    uint32_t bHeight = 0;
    uint32_t operation = kOver;
    uint32_t screenAlpha = 0;
    uint32_t pad0 = 0;
};
static_assert(sizeof(MergeUniforms) == 64, "no padding, on any compiler");

class Merge final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.merge";
        into.label = "Merge";
        into.grouping = "Merge";
        into.description =
            "Composites A onto B. Thirty-nine operations, at the same indices "
            "the Merge plugin uses.";

        // B first, and it is the pass-through one. That order is the
        // convention everywhere -- the background is input 0 -- and it is what
        // makes dropping a node onto a wire do the expected thing.
        aofx::ClipDesc b;
        b.name = "B";
        b.label = "B (background)";
        b.optional = true;
        b.passThrough = true;
        into.inputs.push_back(b);

        aofx::ClipDesc a;
        a.name = "A";
        a.label = "A (foreground)";
        a.optional = true;
        into.inputs.push_back(a);

        aofx::ParamDesc operation;
        operation.name = "operation";
        operation.label = "Operation";
        operation.hint =
            "How A is combined with B. `over` is the ordinary one: A on top of "
            "B, weighted by A's alpha.";
        operation.type = aofx::ParamType::Choice;
        for (const char* name : operations()) {
            // The four that are not per-component are labelled as differing.
            //
            // Fifteen operations were measured against the plugin across the
            // whole range of both inputs and agree to zero code values -- every
            // one that was tried. These four do not, and the disagreement is
            // now pinned down rather than merely admitted:
            //
            //   * The colour arithmetic is right. A flat coloured A over a grey
            //     B returns lum(A) everywhere, and the plugin returns the same
            //     0.595 we do -- so the coefficients and setLum agree.
            //   * Where A has any coverage at all they agree exactly. Both
            //     probed pixels inside the element match to four decimals.
            //   * The whole error is where A partly covers, and a probe that
            //     ramps A's alpha over a flat B says exactly how. At alpha 0
            //     and alpha 1 the two agree. Between them the blue channel
            //     agrees at every step and red and green do not, and the
            //     plugin's answer is not monotonic in alpha: 0.200 at alpha 0,
            //     0.966 at 0.14, then straight down to 0.416 at 1.
            //
            //     Fitting that line, the plugin is lerp(P, blended, alpha) with
            //     P = (1.058, 0.972, 0.800) -- whose blue is exactly B's blue --
            //     and a genuine discontinuity at alpha exactly 0, where it
            //     returns B. Ours is lerp(B, blended, alpha), which is why we
            //     meet at both ends and nowhere in between.
            //
            // So this is one wrong decision about what an HSL blend means over
            // a partly covered pixel, not four broken formulas, and the shape
            // of the right answer is now written down rather than guessed at.
            // A surface probe in the host's equivalence tests (not part of this
            // repository) settled it in one run.
            //
            // Said in the menu rather than in a comment nobody reads, because
            // the alternative is a mode that quietly gives a different picture
            // from the plugin of the same name -- which is the one thing a
            // replacement must not do.
            const bool hsl = std::string(name) == "hue" ||
                             std::string(name) == "saturation" ||
                             std::string(name) == "color" ||
                             std::string(name) == "luminosity";
            operation.choices.push_back(aofx::ChoiceOption{
                name, hsl ? std::string(name) + " (differs from the Merge plugin)"
                          : std::string(name)});
        }
        operation.defaults = {static_cast<double>(kOver)};
        into.params.push_back(operation);

        aofx::ParamDesc screenAlpha;
        screenAlpha.name = "screenAlpha";
        screenAlpha.label = "Alpha masking";
        screenAlpha.hint =
            "Compute the output alpha as a + b - a*b rather than by applying "
            "the operation to alpha. Always on for the four HSL operations, "
            "which have no opinion about alpha at all.";
        screenAlpha.type = aofx::ParamType::Boolean;
        screenAlpha.defaults = {0.0};
        into.params.push_back(screenAlpha);
    }

    [[nodiscard]] std::vector<aofx::KernelDesc> kernels() const override {
        return {aofx::KernelDesc{kKernel, kEntry, k_merge, k_mergeBytes}};
    }

    /// The union, which is what a merge produces.
    ///
    /// The default -- the union of the inputs -- is already right, and saying
    /// so is worth a line: a merge is the one effect where the default region
    /// is correct for a reason rather than by accident. A over B covers
    /// wherever either of them has picture.
    ///
    /// Not every operation needs that much. `in`, `mask` and `stencil` cannot
    /// produce anything outside one of the two, so their region could be
    /// smaller -- but a region that changes when somebody switches operation
    /// is a picture that jumps, and the saving is a rectangle nobody sees.

    [[nodiscard]] bool process(const aofx::RenderRequest& request) override {
        const aofx::InputPlane*  a = request.input("A");
        const aofx::InputPlane*  b = request.input("B");
        const aofx::OutputPlane* target = request.output();
        if (target == nullptr || request.gpu == nullptr) {
            return false;
        }
        // Either input may be missing: a Merge with only a background is a
        // pass-through, and with only a foreground it is that foreground over
        // nothing. Both are ordinary states while a graph is being built.
        if (a == nullptr && b == nullptr) {
            return false;
        }
        const aofx::KernelId kernel = request.gpu->load(kKernel);
        if (kernel == aofx::kInvalidKernel) {
            return false;
        }

        // An unconnected input is bound to the other one and read as nothing:
        // a kernel with an unbound buffer reads a null pointer, and giving it
        // a zero-sized window is what makes every sample fall outside and come
        // back transparent black.
        const aofx::Buffer& present = a != nullptr ? a->buffer : b->buffer;
        const aofx::Buffer aBuffer = a != nullptr ? a->buffer : present;
        const aofx::Buffer bBuffer = b != nullptr ? b->buffer : present;

        MergeUniforms uniforms;
        uniforms.width = static_cast<uint32_t>(target->buffer.width);
        uniforms.height = static_cast<uint32_t>(target->buffer.height);
        uniforms.dstStride = static_cast<uint32_t>(target->buffer.stride);
        uniforms.aStride = static_cast<uint32_t>(aBuffer.stride);
        uniforms.bStride = static_cast<uint32_t>(bBuffer.stride);
        uniforms.aOffsetX = target->buffer.rect.x1 - aBuffer.rect.x1;
        uniforms.aOffsetY = target->buffer.rect.y1 - aBuffer.rect.y1;
        uniforms.bOffsetX = target->buffer.rect.x1 - bBuffer.rect.x1;
        uniforms.bOffsetY = target->buffer.rect.y1 - bBuffer.rect.y1;
        // A missing input is a window of nothing, so every sample of it falls
        // outside and reads as transparent black.
        uniforms.aWidth = a != nullptr ? static_cast<uint32_t>(aBuffer.width) : 0u;
        uniforms.aHeight = a != nullptr ? static_cast<uint32_t>(aBuffer.height) : 0u;
        uniforms.bWidth = b != nullptr ? static_cast<uint32_t>(bBuffer.width) : 0u;
        uniforms.bHeight = b != nullptr ? static_cast<uint32_t>(bBuffer.height) : 0u;

        const auto operation = static_cast<int>(request.number("operation", kOver));
        uniforms.operation = static_cast<uint32_t>(
            std::clamp(operation, 0, static_cast<int>(operations().size()) - 1));
        uniforms.screenAlpha =
            request.number("screenAlpha", 0.0) != 0.0 ? 1u : 0u;

        return request.gpu->run(
            kernel,
            aofx::Grid{static_cast<uint32_t>(target->buffer.width),
                       static_cast<uint32_t>(target->buffer.height), 1},
            {aBuffer, bBuffer, target->buffer}, &uniforms, sizeof(uniforms));
    }
};

}   // namespace

AOFX_EXPORT_EFFECTS(Merge)
