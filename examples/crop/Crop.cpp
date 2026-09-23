// Copyright (c) 2026 aopenfx contributors.
//
// A rectangle, dragged in the viewer, and a tick that says whether it cuts.
//
// THE TICK IS THE WHOLE NODE
//
// Blacking outside a box and cutting the picture down to it look identical on
// screen and are completely different downstream. Blacked, the frame is still
// the frame: every node after it computes the corners it is about to throw
// away, and a proxy stream carries them across a network. Cut, the region of
// definition *is* the box, and everything after this works on that many pixels
// and no more.
//
// Which is right depends on what comes next, which is why it is a tick and not
// a policy: a crop feeding a viewer wants black, and a crop feeding a network
// wants the cut.
//
// WHY IT IS ITS OWN NODE
//
// Because a crop is a crop. The alternative -- a Region parameter inside every
// expensive node -- writes the same geometry three times, tests it three times
// and breaks it three times, and cannot be taken out of the chain to see what
// it was doing.

#include <aofx/Effect.h>
#include <aofx/Entry.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "aofx_kernels_crop.h"

namespace {

/// Must match `CropParams` in crop.slang exactly.
struct CropUniforms {
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
    uint32_t srcStride = 0;
    int32_t  srcOffsetX = 0;
    int32_t  srcOffsetY = 0;
    uint32_t dstWidth = 0;
    uint32_t dstHeight = 0;
    uint32_t dstStride = 0;
    int32_t  dstOriginX = 0;
    int32_t  dstOriginY = 0;
    float    x1 = 0.0F;
    float    y1 = 0.0F;
    float    x2 = 0.0F;
    float    y2 = 0.0F;
    float    softness = 0.0F;
    uint32_t keepOutside = 0;
};
static_assert(sizeof(CropUniforms) == 64, "must match CropParams exactly");

/// A parameter's numbers, whatever its dimension.
const std::vector<double>* numbersOf(const std::vector<aofx::ParamValue>& params,
                                     const char* name) {
    for (const aofx::ParamValue& param : params) {
        if (param.name == name && !param.numbers.empty()) {
            return &param.numbers;
        }
    }
    return nullptr;
}

double numberOf(const std::vector<aofx::ParamValue>& params, const char* name,
                double fallback) {
    const std::vector<double>* held = numbersOf(params, name);
    return held == nullptr ? fallback : held->front();
}

/// The box, ordered, in the picture's coordinates.
///
/// Ordered here and nowhere else. Two corners dragged in a viewer arrive in
/// whatever order somebody dragged them, and every consumer that ordered them
/// itself would be a consumer that could forget to.
struct Box {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;

    [[nodiscard]] bool isEmpty() const { return x2 <= x1 || y2 <= y1; }
};

Box boxOf(const std::vector<aofx::ParamValue>& params, const aofx::Rect& source) {
    const std::vector<double>* one = numbersOf(params, "corner1");
    const std::vector<double>* two = numbersOf(params, "corner2");
    // Both left at nothing is a node somebody has just made: the box is the
    // whole picture, so it does nothing until it is dragged. A crop that
    // blacked the frame the moment it was added would be a node people learn
    // to add last.
    if (one == nullptr || two == nullptr || one->size() < 2 || two->size() < 2 ||
        (one->at(0) == 0.0 && one->at(1) == 0.0 && two->at(0) == 0.0 &&
         two->at(1) == 0.0)) {
        return Box{static_cast<double>(source.x1), static_cast<double>(source.y1),
                   static_cast<double>(source.x2), static_cast<double>(source.y2)};
    }
    Box box;
    box.x1 = std::min(one->at(0), two->at(0));
    box.y1 = std::min(one->at(1), two->at(1));
    box.x2 = std::max(one->at(0), two->at(0));
    box.y2 = std::max(one->at(1), two->at(1));
    return box;
}

class Crop final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.crop";
        into.label = "Crop";
        into.grouping = "Transform";
        into.description =
            "A rectangle, dragged in the viewer.\n\n"
            "With Cut off, what is outside the box goes black and the frame "
            "keeps its size. With it on, the picture really is cut down: the "
            "region of definition becomes the box, and everything after this "
            "node works on that many pixels and no more. The two look the same "
            "on screen and are completely different for everything downstream, "
            "which is why it is a tick rather than a decision made for you.";

        aofx::ClipDesc source;
        source.name = "Source";
        source.label = "Source";
        source.passThrough = true;
        into.inputs.push_back(source);

        into.outputs.push_back(
            aofx::PlaneDesc{"Color", "Colour", {"R", "G", "B", "A"}});

        // Two places in the picture, so the viewer draws a handle on each
        // without anybody writing one: `Position` is the plugin saying "this
        // pair of numbers is somewhere", and the gizmo finds it by asking.
        aofx::ParamDesc one;
        one.name = "corner1";
        one.label = "Corner 1";
        one.type = aofx::ParamType::Double;
        one.dimension = 2;
        one.role = aofx::ParamRole::Position;
        one.defaults = {0.0, 0.0};
        one.hint =
            "One corner of the box, in pixels. Which corner does not matter -- "
            "the two are ordered before anything uses them, so dragging one "
            "past the other turns the box inside out and nothing else.";
        into.params.push_back(one);

        aofx::ParamDesc two = one;
        two.name = "corner2";
        two.label = "Corner 2";
        two.hint = "The opposite corner. Both left at zero means the whole "
                   "picture, so a crop does nothing until it is dragged.";
        into.params.push_back(two);

        aofx::ParamDesc cut;
        cut.name = "cut";
        cut.label = "Cut";
        cut.type = aofx::ParamType::Boolean;
        cut.defaults = {0.0};
        cut.hint =
            "Cut the picture down to the box instead of blacking what is "
            "outside it.\n\n"
            "Off, the frame keeps its size: every node after this one computes "
            "corners it is about to throw away, and a proxy stream carries "
            "them across a network. On, the region of definition is the box, "
            "and the saving is real -- which is what makes this the tick to "
            "reach for when the crop is feeding something expensive.\n\n"
            "It is off by default because a node that silently changed the "
            "size of the picture the moment it was added would be a node that "
            "breaks the graph below it.";
        into.params.push_back(cut);

        aofx::ParamDesc soft;
        soft.name = "softness";
        soft.label = "Softness";
        soft.type = aofx::ParamType::Double;
        soft.defaults = {0.0};
        soft.displayMin = {0.0};
        soft.displayMax = {100.0};
        soft.hardMin = {0.0};
        soft.hardMax = {2000.0};
        soft.hint =
            "How many pixels the edge fades over. A ramp on the distance to "
            "the box, not a blur of the picture -- which is what a soft crop "
            "is actually being asked for, and costs nothing.\n\n"
            "It has no effect with Cut on: there is nothing outside a cut to "
            "fade into.";
        into.params.push_back(soft);

        // And the two corners as one box, so the viewer draws the box that is
        // cut rather than two points somebody has to imagine a rectangle
        // between. The corners keep their `Position` role for a host older
        // than gizmos; a host that knows them draws the box instead, never
        // both (see `aofx/Gizmo.h`).
        aofx::GizmoDesc box;
        box.id = "box";
        box.label = "Crop box";
        box.kind = aofx::GizmoKind::Box;
        box.bindings = {aofx::bindParam("corner1", "corner1"),
                        aofx::bindParam("corner2", "corner2")};
        box.style.line = aofx::GizmoStyle::Line::Dashed;
        into.gizmos.push_back(box);
    }

    std::vector<aofx::KernelDesc> kernels() const override {
        return {aofx::KernelDesc{"cropMain", "cropMain", k_crop, k_cropBytes}};
    }

    /// The box, when it cuts. Otherwise the input, unchanged.
    ///
    /// This is where the tick actually happens: everything else about the two
    /// modes is the same arithmetic.
    [[nodiscard]] aofx::Rect regionOfDefinition(
        double /*time*/, const std::vector<aofx::Rect>& inputRods,
        const std::vector<aofx::ParamValue>& params) const override {
        aofx::Rect input;
        if (!inputRods.empty()) {
            input = inputRods.front();
        }
        if (numberOf(params, "cut", 0.0) < 0.5 || input.isEmpty()) {
            return input;
        }
        const Box box = boxOf(params, input);
        if (box.isEmpty()) {
            return input;
        }
        // Intersected with the input, never grown past it. A crop that made the
        // picture bigger would be a pad, and a node that is two things is a
        // node nobody can predict.
        aofx::Rect out;
        out.x1 = std::max(input.x1, static_cast<int>(std::floor(box.x1)));
        out.y1 = std::max(input.y1, static_cast<int>(std::floor(box.y1)));
        out.x2 = std::min(input.x2, static_cast<int>(std::ceil(box.x2)));
        out.y2 = std::min(input.y2, static_cast<int>(std::ceil(box.y2)));
        return out.isEmpty() ? input : out;
    }

    /// Only what the output covers, which with `cut` on is the box.
    ///
    /// Without this the crop would still ask its input for the whole picture
    /// and the saving would stop at this node instead of travelling up the
    /// chain -- which for a crop in front of a Read is the difference between
    /// decoding a frame and decoding part of one.
    [[nodiscard]] std::vector<aofx::Rect> regionOfInterest(
        double /*time*/, const aofx::Rect& output,
        const std::vector<aofx::Rect>& inputRods,
        const std::vector<aofx::ParamValue>& /*params*/) const override {
        return std::vector<aofx::Rect>(inputRods.size(), output);
    }

    bool isIdentity(const aofx::RenderRequest& request) const override {
        const aofx::InputPlane* source = request.input("Source");
        if (source == nullptr) {
            return false;
        }
        // A box that covers everything and does not cut is a node doing
        // nothing, and saying so is what stops the host allocating an output
        // to copy a picture into.
        const Box box = boxOf(request.params, source->buffer.rect);
        return numberOf(request.params, "cut", 0.0) < 0.5 &&
               box.x1 <= static_cast<double>(source->buffer.rect.x1) &&
               box.y1 <= static_cast<double>(source->buffer.rect.y1) &&
               box.x2 >= static_cast<double>(source->buffer.rect.x2) &&
               box.y2 >= static_cast<double>(source->buffer.rect.y2);
    }

    bool process(const aofx::RenderRequest& request) override {
        const aofx::InputPlane*  source = request.input("Source");
        const aofx::OutputPlane* target = request.output("Color");
        if (source == nullptr || target == nullptr ||
            !source->buffer.isValid() || !target->buffer.isValid() ||
            request.gpu == nullptr) {
            return false;
        }
        const aofx::KernelId kernel = request.gpu->load("cropMain");
        if (kernel == aofx::kInvalidKernel) {
            return false;
        }

        const Box box = boxOf(request.params, source->buffer.rect);
        const bool cut = numberOf(request.params, "cut", 0.0) >= 0.5;

        CropUniforms uniforms;
        uniforms.srcWidth = static_cast<uint32_t>(source->buffer.width);
        uniforms.srcHeight = static_cast<uint32_t>(source->buffer.height);
        uniforms.srcStride = static_cast<uint32_t>(source->buffer.stride);
        uniforms.srcOffsetX = target->buffer.rect.x1 - source->buffer.rect.x1;
        uniforms.srcOffsetY = target->buffer.rect.y1 - source->buffer.rect.y1;
        uniforms.dstWidth = static_cast<uint32_t>(target->buffer.width);
        uniforms.dstHeight = static_cast<uint32_t>(target->buffer.height);
        uniforms.dstStride = static_cast<uint32_t>(target->buffer.stride);
        uniforms.dstOriginX = target->buffer.rect.x1;
        uniforms.dstOriginY = target->buffer.rect.y1;
        uniforms.x1 = static_cast<float>(box.x1);
        uniforms.y1 = static_cast<float>(box.y1);
        uniforms.x2 = static_cast<float>(box.x2);
        uniforms.y2 = static_cast<float>(box.y2);
        uniforms.softness = static_cast<float>(
            std::max(numberOf(request.params, "softness", 0.0), 0.0));
        // With the cut made, the output *is* the box and there is nothing left
        // outside to black. Blacking anyway would eat the box's own edge, by
        // exactly the softness.
        uniforms.keepOutside = cut ? 1u : 0u;

        return request.gpu->run(
            kernel, aofx::Grid{uniforms.dstWidth, uniforms.dstHeight, 1},
            {source->buffer, target->buffer}, &uniforms, sizeof(uniforms));
    }
};

}   // namespace

AOFX_EXPORT_EFFECTS(Crop)
