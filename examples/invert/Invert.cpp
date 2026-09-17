// Copyright (c) 2026 aopenfx contributors.
//
// One, minus the picture.
//
// The same node as openfx-misc's InvertOFX, control for control, on the GPU.
// It exists because that plugin is what a script written elsewhere reaches
// for, and because an invert in the middle of a chain should not be the one
// node that brings the picture back to the CPU.
//
// WHERE ITS TEN CONTROLS WENT
//
// InvertOFX declares ten parameters. Four of them -- Process R, G, B, A -- are
// this host's own: every AOFX effect gets `aofx.channel.{r,g,b,a}`, the host
// fills all four channels and puts back the ones nobody asked to change. So
// they are not declared here and they behave identically, which is the point.
//
// Two more are bookkeeping that plugin keeps for its own UI (`premultChanged`,
// and the page). What is left is the four that do something -- (Un)premult, By,
// Invert Mask, Mix -- and they are all here, with the same names, the same
// defaults and the same meanings.

#include <aofx/Effect.h>
#include <aofx/Entry.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "aofx_kernels_invert.h"

namespace {

/// Must match `InvertParams` in invert.slang exactly.
struct InvertUniforms {
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
    uint32_t srcStride = 0;
    int32_t  srcOffsetX = 0;
    int32_t  srcOffsetY = 0;
    uint32_t dstWidth = 0;
    uint32_t dstHeight = 0;
    uint32_t dstStride = 0;
    uint32_t hasMask = 0;
    uint32_t maskWidth = 0;
    uint32_t maskHeight = 0;
    uint32_t maskStride = 0;
    int32_t  maskOffsetX = 0;
    int32_t  maskOffsetY = 0;
    uint32_t maskInvert = 0;
    uint32_t premult = 0;
    uint32_t premultChannel = 3;
    float    mix = 1.0F;
};
static_assert(sizeof(InvertUniforms) == 72, "must match InvertParams exactly");

class Invert final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.invert";
        into.label = "Invert";
        into.grouping = "Colour";
        into.description =
            "One minus the picture, on the GPU.\n\n"
            "The same controls as InvertOFX, so a script that used that one "
            "behaves the same here. Which channels are inverted is this host's "
            "own R/G/B/A above -- all four by default, like InvertOFX's.\n\n"
            "(Un)premult matters on a plate with a matte: inverting a "
            "premultiplied pixel without dividing it out first makes soft "
            "edges invert brighter than the middle of the same shape, which "
            "reads as a halo. It is off by default because that is what "
            "InvertOFX does, and because a depth pass or a normal map does not "
            "want it.";

        aofx::ClipDesc source;
        source.name = "Source";
        source.label = "Source";
        source.passThrough = true;
        into.inputs.push_back(source);

        // Optional, and the node is complete without it: the alpha of what is
        // wired here says how much of the invert each pixel receives.
        aofx::ClipDesc mask;
        mask.name = "Mask";
        mask.label = "Mask";
        mask.optional = true;
        mask.isMask = true;
        into.inputs.push_back(mask);

        into.outputs.push_back(
            aofx::PlaneDesc{"Color", "Colour", {"R", "G", "B", "A"}});

        aofx::ParamDesc premult;
        premult.name = "premult";
        premult.label = "(Un)premult";
        premult.type = aofx::ParamType::Boolean;
        premult.defaults = {0.0};
        premult.hint =
            "Divide the image by a channel before inverting and multiply it "
            "back afterwards. On for a premultiplied plate: without it a soft "
            "edge inverts brighter than the middle of the shape it belongs to.";
        into.params.push_back(premult);

        aofx::ParamDesc by;
        by.name = "premultChannel";
        by.label = "By";
        by.type = aofx::ParamType::Choice;
        by.choices = {{"r", "R"}, {"g", "G"}, {"b", "B"}, {"a", "A"}};
        by.defaults = {3.0};
        // Shown rather than secret. InvertOFX hides this one and reveals it
        // from code; a rule the host can read is the same thing without the
        // code, and it is the rule the panel already understands.
        by.shownWhen = aofx::ShownWhen{"premult", "1"};
        by.hint =
            "Which channel to divide by and multiply back. Alpha is the "
            "answer for a picture; the others are for a pass that carries its "
            "weight somewhere else.";
        into.params.push_back(by);

        aofx::ParamDesc maskInvert;
        maskInvert.name = "maskInvert";
        maskInvert.label = "Invert Mask";
        maskInvert.type = aofx::ParamType::Boolean;
        maskInvert.defaults = {0.0};
        maskInvert.hint =
            "The invert lands where the mask is dark instead of where it is "
            "bright. With nothing wired to Mask this does nothing, rather than "
            "switching the whole effect off.";
        into.params.push_back(maskInvert);

        aofx::ParamDesc mix;
        mix.name = "mix";
        mix.label = "Mix";
        mix.type = aofx::ParamType::Double;
        mix.defaults = {1.0};
        mix.displayMin = {0.0};
        mix.displayMax = {1.0};
        mix.hardMin = {0.0};
        mix.hardMax = {1.0};
        mix.hint =
            "How much of the invert to keep. Zero is the input, untouched -- "
            "and answered without rendering at all.";
        into.params.push_back(mix);
    }

    std::vector<aofx::KernelDesc> kernels() const override {
        return {aofx::KernelDesc{"invertMain", "invertMain", k_invert,
                                 k_invertBytes}};
    }

    /// At a mix of zero this node is the picture that went in, and saying so
    /// here means the frame is never rendered, never cached and never
    /// uploaded.
    [[nodiscard]] bool isIdentity(
        const aofx::RenderRequest& request) const override {
        return request.number("mix", 1.0) <= 0.0;
    }

    bool process(const aofx::RenderRequest& request) override {
        const aofx::InputPlane*  source = request.input("Source");
        const aofx::OutputPlane* target = request.output("Color");
        if (source == nullptr || target == nullptr ||
            !source->buffer.isValid() || !target->buffer.isValid() ||
            request.gpu == nullptr) {
            return false;
        }
        const aofx::KernelId kernel = request.gpu->load("invertMain");
        if (kernel == aofx::kInvalidKernel) {
            return false;
        }

        InvertUniforms uniforms;
        uniforms.srcWidth = static_cast<uint32_t>(source->buffer.width);
        uniforms.srcHeight = static_cast<uint32_t>(source->buffer.height);
        uniforms.srcStride = static_cast<uint32_t>(source->buffer.stride);
        uniforms.srcOffsetX = target->buffer.rect.x1 - source->buffer.rect.x1;
        uniforms.srcOffsetY = target->buffer.rect.y1 - source->buffer.rect.y1;
        uniforms.dstWidth = static_cast<uint32_t>(target->buffer.width);
        uniforms.dstHeight = static_cast<uint32_t>(target->buffer.height);
        uniforms.dstStride = static_cast<uint32_t>(target->buffer.stride);

        // The mask buffer must always be bound -- the kernel declares three --
        // and `hasMask` is what stops it being read.
        const aofx::InputPlane* mask = request.input("Mask");
        const bool hasMask = mask != nullptr && mask->buffer.isValid();
        uniforms.hasMask = hasMask ? 1U : 0U;
        if (hasMask) {
            uniforms.maskWidth = static_cast<uint32_t>(mask->buffer.width);
            uniforms.maskHeight = static_cast<uint32_t>(mask->buffer.height);
            uniforms.maskStride = static_cast<uint32_t>(mask->buffer.stride);
            uniforms.maskOffsetX =
                target->buffer.rect.x1 - mask->buffer.rect.x1;
            uniforms.maskOffsetY =
                target->buffer.rect.y1 - mask->buffer.rect.y1;
        }
        uniforms.maskInvert =
            request.number("maskInvert", 0.0) >= 0.5 ? 1U : 0U;
        uniforms.premult = request.number("premult", 0.0) >= 0.5 ? 1U : 0U;
        uniforms.premultChannel = static_cast<uint32_t>(
            std::clamp(static_cast<int>(request.number("premultChannel", 3.0) +
                                        0.5),
                       0, 3));
        uniforms.mix = static_cast<float>(
            std::clamp(request.number("mix", 1.0), 0.0, 1.0));

        return request.gpu->run(
            kernel, aofx::Grid{uniforms.dstWidth, uniforms.dstHeight, 1},
            {source->buffer, hasMask ? mask->buffer : source->buffer,
             target->buffer},
            &uniforms, sizeof(uniforms));
    }
};

}   // namespace

AOFX_EXPORT_EFFECTS(Invert)
