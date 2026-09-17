// Copyright (c) 2026 aopenfx contributors.
//
// A grade, against net.sf.openfx.GradePlugin.
//
// The parameters are that plugin's, name for name and default for default, so
// that a script can name either and mean the same thing. Where it differs is
// worth listing, because both differences are the host doing a job the plugin
// had to do itself:
//
//   * No process-R/G/B/A switches. Every AOFX node gets those from the host,
//     which puts the unselected channels back afterwards -- so an effect that
//     declared its own would have two controls fighting over one question.
//     Note the defaults differ: Grade leaves alpha *off*, the host's leave it
//     on, so a graded alpha is the one visible change on swapping them.
//
//   * No premultiply switch. That belongs to the host's image model, not to
//     an effect: everything here is premultiplied throughout.
//
// On speed: level with the plugin it replaces, and that is more than the colour
// conversions managed. Measured on a 1920x1080 plate in an optimised build,
// 5.3 ms against 5.6. The difference from Colorspace is the gamma -- a `pow`
// per channel is enough arithmetic for the GPU to earn its trip, where four
// multiplies were not. Worth knowing which side of that line an operation falls
// on before writing a kernel for it.
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "aofx/Entry.h"

#include "aofx_kernels_grade.h"

namespace {

constexpr const char* kKernel = "org.aopenfx.grade";
constexpr const char* kEntry = "gradeMain";

/// Matched byte for byte by `GradeParams` in grade.slang.
///
/// The float4s are sixteen-byte members among four-byte ones, which is the one
/// place this file departs from the "everything four bytes wide" rule the other
/// kernels follow. It holds because a float4 is naturally sixteen-byte aligned
/// and every one of them sits at a multiple of sixteen -- checked by the
/// static_assert below rather than by reading, since that is exactly the kind
/// of thing two compilers can disagree about.
struct GradeUniforms {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t srcStride = 0;
    uint32_t dstStride = 0;
    float    blackPoint[4] = {0, 0, 0, 0};
    float    whitePoint[4] = {1, 1, 1, 1};
    float    black[4] = {0, 0, 0, 0};
    float    white[4] = {1, 1, 1, 1};
    float    multiply[4] = {1, 1, 1, 1};
    float    offset[4] = {0, 0, 0, 0};
    float    gamma[4] = {1, 1, 1, 1};
    uint32_t clampBlack = 1;
    uint32_t clampWhite = 0;
    uint32_t reverse = 0;
    uint32_t wantClip = 0;
    // Where the input sits relative to the output, and how big it is. The
    // kernel relates the two rather than assuming they line up -- see
    // fetchSource in the .slang, and the crop defect that made it necessary.
    int32_t  srcOffsetX = 0;
    int32_t  srcOffsetY = 0;
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
};
static_assert(sizeof(GradeUniforms) == 160, "no padding, on any compiler");

struct Knob {
    const char* name;
    const char* label;
    const char* hint;
    double      value;   ///< the default, the same in all four components
};

/// Grade's own names and defaults, so a script can say either.
const std::array<Knob, 7>& knobs() {
    static const std::array<Knob, 7> kAll{{
        {"blackPoint", "Black Point",
         "The colour of the darkest pixels in the image.", 0.0},
        {"whitePoint", "White Point",
         "The colour of the brightest pixels in the image.", 1.0},
        {"black", "Lift",
         "Colours at the black point are set to this.", 0.0},
        {"white", "Gain",
         "Colours at the white point are set to this.", 1.0},
        {"multiply", "Multiply", "Multiplies the result by this.", 1.0},
        {"offset", "Offset",
         "Added to the result, after the black and white points.", 0.0},
        {"gamma", "Gamma",
         "Final gamma. Negative values are left alone -- there is no real "
         "answer for a negative number raised to a fractional power, and a "
         "scene-linear picture carries negatives routinely.",
         1.0},
    }};
    return kAll;
}

class Grade final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.grade";
        into.label = "Grade";
        into.grouping = "Colour";
        into.description =
            "Two points in, two points out, then multiply, offset and gamma. "
            "The same arithmetic as the Grade plugin, on the GPU.";

        into.inputs.push_back(
            aofx::ClipDesc{"Source", "Source", false, false, false});

        // The picture, and a matte of what this grade pushed out of range.
        //
        // The first effect in the tree to declare an arbitrary output variable,
        // which until now was transport with nothing travelling on it. It is a
        // fair example of what the idea is for: the information exists only
        // while the kernel holds the value, since a clamp destroys it and an
        // unclamped value downstream no longer says which node put it there.
        //
        // One cost, said out loud because the SDK cannot yet avoid it: a
        // declared plane is allocated on every render whether or not anything
        // downstream reads it. For a 1920x1080 float4 that is 33 MiB per grade
        // node, per frame. Making it conditional needs the host to ask the
        // effect at render time which planes are wanted -- describe() is
        // answered once and cannot know. Until that exists, an effect should
        // declare a plane only where it earns its keep.
        into.outputs.push_back(
            aofx::PlaneDesc{"Color", "Colour", {"R", "G", "B", "A"}});
        into.outputs.push_back(aofx::PlaneDesc{
            "Clip", "Clipping matte", {"R", "G", "B", "A"}});

        for (const Knob& knob : knobs()) {
            aofx::ParamDesc param;
            param.name = knob.name;
            param.label = knob.label;
            param.hint = knob.hint;
            // A colour, not four numbers that happen to sit together.
            //
            // Every one of Nuke's seven Grade controls is an AColor_Knob, and
            // that is not decoration: lift and gain are colours, people reach
            // for them to take a cast out of a shot, and doing that by typing
            // four figures is doing it with the wrong instrument. The panel
            // gives a colour a swatch, a picker and a wheel; it gives four
            // doubles four spin boxes.
            //
            // Nothing about the values changes -- the kernel receives exactly
            // what it did before. This is a statement about what they mean.
            param.type = aofx::ParamType::Colour;
            // Four components, alpha included. A grade that could not touch
            // alpha would be a grade that cannot pull a matte back.
            param.dimension = 4;
            param.defaults = {knob.value, knob.value, knob.value, knob.value};
            into.params.push_back(param);
        }

        for (const auto& [name, label, hint, on] :
             std::vector<std::tuple<const char*, const char*, const char*, bool>>{
                 {"clampBlack", "Clamp Black",
                  "Everything below zero on output becomes zero.", true},
                 {"clampWhite", "Clamp White",
                  "Everything above one on output becomes one.", false},
                 {"reverse", "Reverse",
                  "Apply the inverse correction -- the exact inverse, gamma "
                  "first and then the line, not a second grade with reciprocal "
                  "numbers. Useful for undoing a grade downstream of itself.",
                  false}}) {
            aofx::ParamDesc param;
            param.name = name;
            param.label = label;
            param.hint = hint;
            param.type = aofx::ParamType::Boolean;
            param.defaults = {on ? 1.0 : 0.0};
            into.params.push_back(param);
        }
    }

    [[nodiscard]] std::vector<aofx::KernelDesc> kernels() const override {
        return {aofx::KernelDesc{kKernel, kEntry, k_grade, k_gradeBytes}};
    }

    /// A grade that does nothing.
    ///
    /// Not a rare case: it is what a node holds the moment it is made, and a
    /// graph full of freshly added nodes is what scrubbing a timeline goes
    /// through. Answering it means the host never allocates an output.
    ///
    /// Clamping is checked too, because a clamp is not nothing -- with
    /// clampBlack on, a picture with negatives is changed by a grade whose
    /// every other control is at its default.
    [[nodiscard]] bool isIdentity(const aofx::RenderRequest& request) const override {
        if (request.number("clampBlack", 1.0) != 0.0 ||
            request.number("clampWhite", 0.0) != 0.0) {
            return false;
        }
        for (const Knob& knob : knobs()) {
            for (size_t c = 0; c < 4; ++c) {
                if (request.number(knob.name, knob.value, c) != knob.value) {
                    return false;
                }
            }
        }
        return true;
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

        GradeUniforms uniforms;
        uniforms.width = static_cast<uint32_t>(target->buffer.width);
        uniforms.height = static_cast<uint32_t>(target->buffer.height);
        uniforms.srcStride = static_cast<uint32_t>(source->buffer.stride);
        uniforms.dstStride = static_cast<uint32_t>(target->buffer.stride);
        uniforms.srcOffsetX = target->buffer.rect.x1 - source->buffer.rect.x1;
        uniforms.srcOffsetY = target->buffer.rect.y1 - source->buffer.rect.y1;
        uniforms.srcWidth = static_cast<uint32_t>(source->buffer.width);
        uniforms.srcHeight = static_cast<uint32_t>(source->buffer.height);

        float* const targets[7] = {uniforms.blackPoint, uniforms.whitePoint,
                                   uniforms.black,      uniforms.white,
                                   uniforms.multiply,   uniforms.offset,
                                   uniforms.gamma};
        for (size_t i = 0; i < knobs().size(); ++i) {
            for (size_t c = 0; c < 4; ++c) {
                targets[i][c] = static_cast<float>(
                    request.number(knobs()[i].name, knobs()[i].value, c));
            }
        }
        uniforms.clampBlack = request.number("clampBlack", 1.0) != 0.0 ? 1u : 0u;
        uniforms.clampWhite = request.number("clampWhite", 0.0) != 0.0 ? 1u : 0u;
        uniforms.reverse = request.number("reverse", 0.0) != 0.0 ? 1u : 0u;

        // The matte is produced only if somebody asked the host for it.
        //
        // When nobody did there is no buffer, and a kernel argument still has
        // to be bound -- so the picture's own buffer goes in its place and
        // `wantClip` stops the write. Binding a buffer that is never written is
        // free; the alternative, allocating a full frame per node per render
        // for a matte nobody reads, is 33 MiB at 1920x1080.
        const aofx::OutputPlane* clip = request.output("Clip");
        const bool haveClip = clip != nullptr && clip->buffer.isValid();
        uniforms.wantClip = haveClip ? 1u : 0u;

        return request.gpu->run(
            kernel,
            aofx::Grid{static_cast<uint32_t>(target->buffer.width),
                       static_cast<uint32_t>(target->buffer.height), 1},
            {source->buffer, target->buffer,
             haveClip ? clip->buffer : target->buffer},
            &uniforms, sizeof(uniforms));
    }
};

}   // namespace

AOFX_EXPORT_EFFECTS(Grade)
