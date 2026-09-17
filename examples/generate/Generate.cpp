// Copyright (c) 2026 aopenfx contributors.
//
// The six nodes that make a picture out of nothing, and the one that measures
// one.
//
// They arrive together because they are the same node six times over: no
// input, a rectangle to fill, and a rule for the pixel at (x, y). Six bundles
// would have been six copies of the region-of-definition argument below and
// six CMake files; one bundle exports six effects, which is what
// `AOFX_EXPORT_EFFECTS` takes a list for.
//
// WHY A GENERATOR NEEDS ITS OWN REGION OF DEFINITION
//
// The default asks the inputs how big the picture is. A generator has no
// inputs, so the default answers "empty" and the node renders nothing at all
// -- which looks exactly like a node that is broken. Each of these returns a
// rectangle far larger than any format, and the engine clamps it to the
// project (RenderEngine.cpp, `clampRegion`). Set `size` and the rectangle
// becomes that box at the origin instead, which is how a Constant smaller than
// the format is made.
//
// WHAT REPLACES WHAT
//
// These take over from seven openfx-misc plugins: CheckerBoard, ColorBars,
// ColorWheel, Constant, ImageStatistics, NoOp and Solid. Six, not seven,
// because Solid is a Constant whose colour has no alpha -- a checkbox, not a
// node. The seven are named in `supersededBy` and leave the menu; every one of
// them still resolves, describes and renders, so old scripts open unchanged.

#include <aofx/Effect.h>
#include <aofx/Entry.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "aofx_kernels_generate.h"
#include "aofx_kernels_imagestats.h"

namespace {

/// Must match `GenerateParams` in generate.slang exactly.
struct GenerateUniforms {
    uint32_t dstWidth = 0;
    uint32_t dstHeight = 0;
    uint32_t dstStride = 0;
    int32_t  originX = 0;

    int32_t  originY = 0;
    int32_t  frameX1 = 0;
    int32_t  frameY1 = 0;
    int32_t  frameX2 = 0;

    int32_t  frameY2 = 0;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;

    float colour[4] = {0.0F, 0.0F, 0.0F, 1.0F};

    float checker0[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    float checker1[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    float checker2[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    float checker3[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    float lineColour[4] = {0.0F, 0.0F, 0.0F, 1.0F};

    float boxSize[2] = {64.0F, 64.0F};
    float lineWidth = 0.0F;
    float barLevel = 0.75F;

    float centreSaturation = 0.0F;
    float edgeSaturation = 1.0F;
    float centreValue = 1.0F;
    float edgeValue = 1.0F;

    float wheelGamma = 1.0F;
    float rotate = 0.0F;
    float pad3 = 0.0F;
    float pad4 = 0.0F;
};
static_assert(sizeof(GenerateUniforms) == 192, "must match GenerateParams");

/// Must match `StatsParams` in imagestats.slang exactly.
struct StatsUniforms {
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
    uint32_t srcStride = 0;
    uint32_t rows = 0;

    uint32_t x1 = 0;
    uint32_t y1 = 0;
    uint32_t x2 = 0;
    uint32_t y2 = 0;

    uint32_t dstWidth = 0;
    uint32_t dstHeight = 0;
    uint32_t dstStride = 0;
    int32_t  srcOffsetX = 0;

    int32_t  srcOffsetY = 0;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};
static_assert(sizeof(StatsUniforms) == 64, "must match StatsParams");

double numberOf(const std::vector<aofx::ParamValue>& params, const char* name,
                size_t dim, double fallback) {
    for (const aofx::ParamValue& param : params) {
        if (param.name == name && param.numbers.size() > dim) {
            return param.numbers[dim];
        }
    }
    return fallback;
}

void colourInto(const std::vector<aofx::ParamValue>& params, const char* name,
                float (&into)[4], const float (&fallback)[4]) {
    for (size_t i = 0; i < 4; ++i) {
        into[i] = static_cast<float>(
            numberOf(params, name, i, static_cast<double>(fallback[i])));
    }
}

/// Larger than any format, and the engine cuts it down to the project.
///
/// Not infinite: a rectangle of INT_MAX has a width that overflows the moment
/// anything subtracts its corners, and the arithmetic that clamps it is the
/// first thing to do so.
constexpr aofx::Rect kEverywhere{-1'000'000, -1'000'000, 1'000'000, 1'000'000};

/// What every generator here does with `size`: nothing when it is zero, and a
/// box at the origin when it is not.
///
/// At the render's scale, because `size` is canonical pixels somebody typed
/// and this host renders reduced while anybody is dragging. A generator that
/// handed back its full size during a gesture describes a picture twice the
/// one it has, and everything downstream places it against a rectangle that
/// does not fit.
[[nodiscard]] aofx::Rect sizedRod(const std::vector<aofx::ParamValue>& params,
                                  double scaleX, double scaleY) {
    const double sx = scaleX > 0.0 ? scaleX : 1.0;
    const double sy = scaleY > 0.0 ? scaleY : 1.0;
    const auto w = static_cast<int>(
        std::lround(numberOf(params, "size", 0, 0.0) * sx));
    const auto h = static_cast<int>(
        std::lround(numberOf(params, "size", 1, 0.0) * sy));
    if (w <= 0 || h <= 0) {
        return kEverywhere;
    }
    return aofx::Rect{0, 0, w, h};
}

void describeSize(aofx::EffectDesc& into) {
    aofx::ParamDesc size;
    size.name = "size";
    size.label = "Size";
    size.hint =
        "How big the picture is. Zero means the project format, which is what "
        "you want almost always; anything else makes a box of that many "
        "pixels at the origin.";
    size.type = aofx::ParamType::Integer;
    size.dimension = 2;
    size.defaults = {0.0, 0.0};
    into.params.push_back(size);
}

/// The output every one of these produces: colour with an alpha.
void describeOutput(aofx::EffectDesc& into) {
    into.outputs.push_back(
        aofx::PlaneDesc{"Color", "Colour", {"R", "G", "B", "A"}});
}

/// Fills in the half of the uniforms every pattern shares.
void fillTarget(GenerateUniforms& uniforms, const aofx::RenderRequest& request,
                const aofx::Buffer& target) {
    uniforms.dstWidth = static_cast<uint32_t>(target.width);
    uniforms.dstHeight = static_cast<uint32_t>(target.height);
    uniforms.dstStride = static_cast<uint32_t>(target.stride);
    uniforms.originX = target.rect.x1;
    uniforms.originY = target.rect.y1;
    // The whole picture, not the buffer. `outputRod` is what the host asked
    // this node to cover; the buffer may be one tile of it, or -- when the
    // project crops to format and this node asked for less -- a larger canvas
    // this pattern must not spread into.
    uniforms.frameX1 = request.outputRod.x1;
    uniforms.frameY1 = request.outputRod.y1;
    uniforms.frameX2 = request.outputRod.x2;
    uniforms.frameY2 = request.outputRod.y2;
}

/// One dispatch of one pattern kernel into the output. Every generator's
/// `process` is this line and the uniforms it filled in first.
[[nodiscard]] bool paint(const aofx::RenderRequest& request, const char* name,
                         const GenerateUniforms& uniforms) {
    const aofx::OutputPlane* target = request.output("Color");
    if (target == nullptr) {
        return false;
    }
    const aofx::KernelId kernel = request.gpu->load(name);
    if (kernel == aofx::kInvalidKernel) {
        return false;
    }
    return request.gpu->run(kernel,
                            aofx::Grid{uniforms.dstWidth, uniforms.dstHeight, 1},
                            {target->buffer}, &uniforms, sizeof(uniforms));
}

// ---------------------------------------------------------------------------

class Constant final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.constant";
        into.label = "Constant";
        // "Image/Source" and not "Image": the menu buckets on the part after
        // the slash, and a bare "Image" falls through to Other -- which is
        // where all four generators landed, one shelf away from the readers
        // they are used beside. A generator IS a source.
        into.grouping = "Image/Source";
        into.description =
            "A rectangle of one colour.\n\n"
            "The alpha is a real channel and not an afterthought: a Constant "
            "at alpha zero is a hole, one at alpha one is a card, and the "
            "openfx-misc pair Constant and Solid differed in nothing else.";
        describeOutput(into);

        aofx::ParamDesc colour;
        colour.name = "color";
        colour.label = "Color";
        colour.hint = "The colour, alpha included.";
        colour.type = aofx::ParamType::Colour;
        colour.dimension = 4;
        colour.defaults = {0.0, 0.0, 0.0, 1.0};
        into.params.push_back(colour);

        describeSize(into);
    }

    [[nodiscard]] std::vector<aofx::KernelDesc> kernels() const override {
        return {aofx::KernelDesc{"constant", "constantMain", k_generate,
                                 k_generateBytes}};
    }

    [[nodiscard]] aofx::Rect regionOfDefinition(
        double, double scaleX, double scaleY, const std::vector<aofx::Rect>&,
        const std::vector<aofx::ParamValue>& params) const override {
        return sizedRod(params, scaleX, scaleY);
    }

    [[nodiscard]] bool process(const aofx::RenderRequest& request) override {
        const aofx::OutputPlane* target = request.output("Color");
        if (target == nullptr) {
            return false;
        }
        GenerateUniforms uniforms;
        fillTarget(uniforms, request, target->buffer);
        static constexpr float kBlack[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        colourInto(request.params, "color", uniforms.colour, kBlack);
        return paint(request, "constant", uniforms);
    }
};

// ---------------------------------------------------------------------------

class CheckerBoard final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.checkerboard";
        into.label = "CheckerBoard";
        into.grouping = "Image/Source";
        into.description =
            "A checker, for seeing what a transform did.\n\n"
            "Four colours rather than two, in a 2x2 cell, because a checker "
            "of two cannot show a half-cell shift and one of four can.";
        describeOutput(into);

        struct Slot {
            const char*                name;
            const char*                label;
            std::initializer_list<double> value;
        };
        const Slot slots[] = {
            {"color0", "Color 0", {0.1, 0.1, 0.1, 1.0}},
            {"color1", "Color 1", {0.5, 0.5, 0.5, 1.0}},
            {"color2", "Color 2", {0.5, 0.5, 0.5, 1.0}},
            {"color3", "Color 3", {0.1, 0.1, 0.1, 1.0}},
        };
        for (const Slot& slot : slots) {
            aofx::ParamDesc colour;
            colour.name = slot.name;
            colour.label = slot.label;
            colour.hint =
                "One of the four squares of the repeating cell, read left to "
                "right and then upward.";
            colour.type = aofx::ParamType::Colour;
            colour.dimension = 4;
            colour.defaults = slot.value;
            into.params.push_back(colour);
        }

        aofx::ParamDesc box;
        box.name = "boxsize";
        box.label = "Box size";
        box.hint = "How big one square is, in pixels.";
        box.type = aofx::ParamType::Double;
        box.dimension = 2;
        box.defaults = {64.0, 64.0};
        into.params.push_back(box);

        aofx::ParamDesc lineColour;
        lineColour.name = "linecolor";
        lineColour.label = "Line color";
        lineColour.hint = "The grid drawn over the squares.";
        lineColour.type = aofx::ParamType::Colour;
        lineColour.dimension = 4;
        lineColour.defaults = {1.0, 1.0, 1.0, 1.0};
        into.params.push_back(lineColour);

        aofx::ParamDesc lineWidth;
        lineWidth.name = "linewidth";
        lineWidth.label = "Line width";
        lineWidth.hint =
            "How thick the grid is, in pixels. Zero draws no grid, which is "
            "the default -- the squares are the pattern.";
        lineWidth.type = aofx::ParamType::Double;
        lineWidth.dimension = 1;
        lineWidth.defaults = {0.0};
        into.params.push_back(lineWidth);

        describeSize(into);
    }

    [[nodiscard]] std::vector<aofx::KernelDesc> kernels() const override {
        return {aofx::KernelDesc{"checkerboard", "checkerBoardMain", k_generate,
                                 k_generateBytes}};
    }

    [[nodiscard]] aofx::Rect regionOfDefinition(
        double, double scaleX, double scaleY, const std::vector<aofx::Rect>&,
        const std::vector<aofx::ParamValue>& params) const override {
        return sizedRod(params, scaleX, scaleY);
    }

    [[nodiscard]] bool process(const aofx::RenderRequest& request) override {
        const aofx::OutputPlane* target = request.output("Color");
        if (target == nullptr) {
            return false;
        }
        GenerateUniforms uniforms;
        fillTarget(uniforms, request, target->buffer);
        static constexpr float kDark[4] = {0.1F, 0.1F, 0.1F, 1.0F};
        static constexpr float kLight[4] = {0.5F, 0.5F, 0.5F, 1.0F};
        static constexpr float kWhite[4] = {1.0F, 1.0F, 1.0F, 1.0F};
        colourInto(request.params, "color0", uniforms.checker0, kDark);
        colourInto(request.params, "color1", uniforms.checker1, kLight);
        colourInto(request.params, "color2", uniforms.checker2, kLight);
        colourInto(request.params, "color3", uniforms.checker3, kDark);
        colourInto(request.params, "linecolor", uniforms.lineColour, kWhite);
        // A square is sixty-four *canonical* pixels, so at half scale it is
        // thirty-two of the ones being drawn. Left unscaled the board keeps
        // its cell count in proxy pixels instead, which is the same picture at
        // twice the size -- the texture visibly growing for as long as a
        // gesture lasts and snapping back when it ends.
        const double sx = request.scaleX > 0.0 ? request.scaleX : 1.0;
        const double sy = request.scaleY > 0.0 ? request.scaleY : 1.0;
        uniforms.boxSize[0] = static_cast<float>(
            numberOf(request.params, "boxsize", 0, 64.0) * sx);
        uniforms.boxSize[1] = static_cast<float>(
            numberOf(request.params, "boxsize", 1, 64.0) * sy);
        uniforms.lineWidth = static_cast<float>(
            numberOf(request.params, "linewidth", 0, 0.0) * sx);
        return paint(request, "checkerboard", uniforms);
    }
};

// ---------------------------------------------------------------------------

class ColorBars final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.colorbars";
        into.label = "ColorBars";
        into.grouping = "Image/Source";
        into.description =
            "SMPTE colour bars: seven bars, the reversed blue strip, and the "
            "bottom quarter with -I, +Q and the three-step PLUGE.\n\n"
            "Made from the numbers rather than from a picture, so they are "
            "right at any format and at any bit depth.";
        describeOutput(into);

        aofx::ParamDesc level;
        level.name = "level";
        level.label = "Level";
        level.hint =
            "How bright the bars are. 75% is what a legaliser passes and what "
            "broadcast means by colour bars; 100% clips on the way out and is "
            "for testing that it does.";
        level.type = aofx::ParamType::Choice;
        level.choices = {{"75", "75%"}, {"100", "100%"}};
        level.defaults = {0.0};
        into.params.push_back(level);

        describeSize(into);
    }

    [[nodiscard]] std::vector<aofx::KernelDesc> kernels() const override {
        return {aofx::KernelDesc{"colorbars", "colorBarsMain", k_generate,
                                 k_generateBytes}};
    }

    [[nodiscard]] aofx::Rect regionOfDefinition(
        double, double scaleX, double scaleY, const std::vector<aofx::Rect>&,
        const std::vector<aofx::ParamValue>& params) const override {
        return sizedRod(params, scaleX, scaleY);
    }

    [[nodiscard]] bool process(const aofx::RenderRequest& request) override {
        const aofx::OutputPlane* target = request.output("Color");
        if (target == nullptr) {
            return false;
        }
        GenerateUniforms uniforms;
        fillTarget(uniforms, request, target->buffer);
        uniforms.barLevel =
            numberOf(request.params, "level", 0, 0.0) >= 0.5 ? 1.0F : 0.75F;
        return paint(request, "colorbars", uniforms);
    }
};

// ---------------------------------------------------------------------------

class ColorWheel final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.colorwheel";
        into.label = "ColorWheel";
        into.grouping = "Image/Source";
        into.description =
            "Hue around, saturation outward: the disc for looking at what a "
            "colour operation does to every hue at once.\n\n"
            "Outside the disc is transparent black, not a colour, so it "
            "composites over a plate without a square around it.";
        describeOutput(into);

        struct Knob {
            const char* name;
            const char* label;
            const char* hint;
            double      value;
        };
        const Knob knobs[] = {
            {"centersaturation", "Center saturation",
             "How saturated the middle is. Zero makes the usual wheel, with "
             "white at the centre.",
             0.0},
            {"edgesaturation", "Edge saturation",
             "How saturated the rim is.", 1.0},
            {"centervalue", "Center value", "How bright the middle is.", 1.0},
            {"edgevalue", "Edge value", "How bright the rim is.", 1.0},
            {"gamma", "Gamma",
             "Bends the brightness across the disc, which spreads the darker "
             "end out where it is easier to look at.",
             1.0},
            {"rotate", "Rotate",
             "Turns the wheel, in degrees. Red sits at three o'clock at zero.",
             0.0},
        };
        for (const Knob& knob : knobs) {
            aofx::ParamDesc param;
            param.name = knob.name;
            param.label = knob.label;
            param.hint = knob.hint;
            param.type = aofx::ParamType::Double;
            param.dimension = 1;
            param.defaults = {knob.value};
            into.params.push_back(param);
        }

        describeSize(into);
    }

    [[nodiscard]] std::vector<aofx::KernelDesc> kernels() const override {
        return {aofx::KernelDesc{"colorwheel", "colorWheelMain", k_generate,
                                 k_generateBytes}};
    }

    [[nodiscard]] aofx::Rect regionOfDefinition(
        double, double scaleX, double scaleY, const std::vector<aofx::Rect>&,
        const std::vector<aofx::ParamValue>& params) const override {
        return sizedRod(params, scaleX, scaleY);
    }

    [[nodiscard]] bool process(const aofx::RenderRequest& request) override {
        const aofx::OutputPlane* target = request.output("Color");
        if (target == nullptr) {
            return false;
        }
        GenerateUniforms uniforms;
        fillTarget(uniforms, request, target->buffer);
        uniforms.centreSaturation = static_cast<float>(
            numberOf(request.params, "centersaturation", 0, 0.0));
        uniforms.edgeSaturation = static_cast<float>(
            numberOf(request.params, "edgesaturation", 0, 1.0));
        uniforms.centreValue =
            static_cast<float>(numberOf(request.params, "centervalue", 0, 1.0));
        uniforms.edgeValue =
            static_cast<float>(numberOf(request.params, "edgevalue", 0, 1.0));
        uniforms.wheelGamma =
            static_cast<float>(numberOf(request.params, "gamma", 0, 1.0));
        uniforms.rotate =
            static_cast<float>(numberOf(request.params, "rotate", 0, 0.0));
        return paint(request, "colorwheel", uniforms);
    }
};

// ---------------------------------------------------------------------------

/// A node that does nothing, on purpose.
///
/// Nuke calls it a Dot and uses it to route wires; openfx-misc calls it NoOp
/// and uses it to force a copy. Here it is neither trick and both uses: it is
/// always an identity, so the engine hands the input straight through and the
/// node costs a graph edge and no pixels at all. Which is exactly what a node
/// for labelling a branch should cost.
class NoOp final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.noop";
        into.label = "NoOp";
        into.grouping = "Other";
        into.description =
            "Passes the picture through untouched.\n\n"
            "For routing a wire, for parking a branch, and for hanging a name "
            "on part of a graph. It renders nothing: the engine sees an "
            "identity and skips it, so a graph full of these costs the same "
            "as one with none.";

        aofx::ClipDesc source;
        source.name = "Source";
        source.label = "Source";
        into.inputs.push_back(source);
        describeOutput(into);
    }

    [[nodiscard]] std::vector<aofx::KernelDesc> kernels() const override {
        return {};
    }

    [[nodiscard]] bool isIdentity(const aofx::RenderRequest&) const override {
        return true;
    }

    [[nodiscard]] bool process(const aofx::RenderRequest&) override {
        // Never reached: `isIdentity` is unconditional, so the engine takes
        // the input and this is never asked to render. Returning true rather
        // than false anyway, because a node that says it did nothing and then
        // reports failure is a node that breaks a graph on the day somebody
        // changes the identity rule.
        return true;
    }
};

// ---------------------------------------------------------------------------

/// Min, max, mean and standard deviation, per channel, attached to the output.
///
/// A probe, not a filter: the picture goes through untouched and the numbers
/// travel with it as `stats.<node>`, which the panel reads and a node
/// downstream could too.
///
/// THE ONE SYNCHRONISATION
///
/// Reading numbers back means waiting for the device, once per frame, which is
/// the thing every other node here is written to avoid. It is unavoidable: a
/// statistic nobody can read is not a statistic. It is also why this is its own
/// node rather than a switch on another one -- the cost is opt-in, and a graph
/// without one never pays it.
class ImageStatistics final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.imagestatistics";
        into.label = "ImageStatistics";
        // Analysis, with the other nodes that measure rather than change:
        // it is a probe, and somebody looking for it is looking for a
        // measurement.
        into.grouping = "Measure";
        into.description =
            "Measures the picture and passes it through.\n\n"
            "Minimum, maximum, mean and standard deviation for each of the "
            "four channels, over the whole frame or over a rectangle. The "
            "numbers appear in this node's panel under Measurements, and they "
            "ride along with the picture, so a node below can read them.";

        aofx::ClipDesc source;
        source.name = "Source";
        source.label = "Source";
        into.inputs.push_back(source);
        describeOutput(into);

        aofx::ParamDesc restrict;
        restrict.name = "restrict";
        restrict.label = "Restrict to region";
        restrict.hint =
            "Measures inside the rectangle below instead of the whole frame.";
        restrict.type = aofx::ParamType::Boolean;
        restrict.defaults = {0.0};
        into.params.push_back(restrict);

        aofx::ParamDesc region;
        region.name = "region";
        region.label = "Region";
        region.hint =
            "The rectangle to measure, in pixels: left, bottom, right, top.";
        region.type = aofx::ParamType::Double;
        region.dimension = 4;
        region.defaults = {0.0, 0.0, 0.0, 0.0};
        into.params.push_back(region);
    }

    [[nodiscard]] std::vector<aofx::KernelDesc> kernels() const override {
        return {
            aofx::KernelDesc{"statsRow", "statsRow", k_imagestats,
                             k_imagestatsBytes},
            aofx::KernelDesc{"statsFold", "statsFold", k_imagestats,
                             k_imagestatsBytes},
            aofx::KernelDesc{"statsCopy", "statsCopy", k_imagestats,
                             k_imagestatsBytes},
        };
    }

    [[nodiscard]] bool process(const aofx::RenderRequest& request) override {
        const aofx::InputPlane*  source = request.input("Source", "Color");
        const aofx::OutputPlane* target = request.output("Color");
        if (source == nullptr || target == nullptr) {
            return false;
        }
        const aofx::Buffer& in = source->buffer;

        // The region, brought into the source buffer's own coordinates and
        // then cut to it. A rectangle typed outside the picture measures the
        // part that overlaps and nothing else -- not zero, and not the whole
        // frame either.
        int x1 = 0;
        int y1 = 0;
        int x2 = in.width;
        int y2 = in.height;
        if (numberOf(request.params, "restrict", 0, 0.0) >= 0.5) {
            const auto left = static_cast<int>(
                std::floor(numberOf(request.params, "region", 0, 0.0)));
            const auto bottom = static_cast<int>(
                std::floor(numberOf(request.params, "region", 1, 0.0)));
            const auto right = static_cast<int>(
                std::ceil(numberOf(request.params, "region", 2, 0.0)));
            const auto top = static_cast<int>(
                std::ceil(numberOf(request.params, "region", 3, 0.0)));
            x1 = std::clamp(left - in.rect.x1, 0, in.width);
            y1 = std::clamp(bottom - in.rect.y1, 0, in.height);
            x2 = std::clamp(right - in.rect.x1, 0, in.width);
            y2 = std::clamp(top - in.rect.y1, 0, in.height);
        }

        StatsUniforms uniforms;
        uniforms.srcWidth = static_cast<uint32_t>(in.width);
        uniforms.srcHeight = static_cast<uint32_t>(in.height);
        uniforms.srcStride = static_cast<uint32_t>(in.stride);
        uniforms.x1 = static_cast<uint32_t>(x1);
        uniforms.y1 = static_cast<uint32_t>(y1);
        uniforms.x2 = static_cast<uint32_t>(x2);
        uniforms.y2 = static_cast<uint32_t>(y2);
        uniforms.rows = static_cast<uint32_t>(std::max(0, y2 - y1));
        uniforms.dstWidth = static_cast<uint32_t>(target->buffer.width);
        uniforms.dstHeight = static_cast<uint32_t>(target->buffer.height);
        uniforms.dstStride = static_cast<uint32_t>(target->buffer.stride);
        uniforms.srcOffsetX = target->buffer.rect.x1 - in.rect.x1;
        uniforms.srcOffsetY = target->buffer.rect.y1 - in.rect.y1;

        // Four float4 for the answer plus four per row. Asked for as a 2D
        // scratch because that is the shape `scratch` takes; what matters is
        // that it is at least this many quads.
        const int quads = 4 + static_cast<int>(uniforms.rows) * 4;
        const aofx::Buffer sums = request.gpu->scratch(quads, 1);
        if (!sums.isValid()) {
            return false;
        }
        const std::vector<aofx::Buffer> bound{in, sums, target->buffer};

        const aofx::KernelId row = request.gpu->load("statsRow");
        const aofx::KernelId fold = request.gpu->load("statsFold");
        const aofx::KernelId copy = request.gpu->load("statsCopy");
        if (row == aofx::kInvalidKernel || fold == aofx::kInvalidKernel ||
            copy == aofx::kInvalidKernel) {
            return false;
        }
        if (!request.gpu->run(copy,
                              aofx::Grid{uniforms.dstWidth, uniforms.dstHeight, 1},
                              bound, &uniforms, sizeof(uniforms))) {
            return false;
        }
        if (uniforms.rows == 0 || x2 <= x1) {
            // Nothing to measure. The picture still goes through, and no
            // numbers are attached -- an empty Measurements row says "no
            // region" more honestly than four zeroes would.
            return true;
        }
        if (!request.gpu->run(row, aofx::Grid{uniforms.rows, 1, 1}, bound,
                              &uniforms, sizeof(uniforms)) ||
            !request.gpu->run(fold, aofx::Grid{4, 1, 1}, bound, &uniforms,
                              sizeof(uniforms))) {
            return false;
        }

        float answer[16] = {};
        if (!request.gpu->read(sums, answer, sizeof(answer))) {
            return false;
        }
        const auto count = static_cast<double>(x2 - x1) *
                           static_cast<double>(y2 - y1);
        std::vector<float> values;
        values.reserve(16);
        for (int i = 0; i < 8; ++i) {   // min and max, as measured
            values.push_back(answer[i]);
        }
        for (int c = 0; c < 4; ++c) {   // mean
            values.push_back(static_cast<float>(answer[8 + c] / count));
        }
        for (int c = 0; c < 4; ++c) {
            // The variance from the two sums. Clamped at zero because the
            // subtraction of two nearly equal large numbers can land a hair
            // below it, and a negative variance is a NaN one square root later.
            const double mean = answer[8 + c] / count;
            const double variance =
                std::max(0.0, answer[12 + c] / count - mean * mean);
            values.push_back(static_cast<float>(std::sqrt(variance)));
        }
        request.attach("stats." + request.instance, std::move(values));
        return true;
    }
};

}   // namespace

AOFX_EXPORT_EFFECTS(Constant, CheckerBoard, ColorBars, ColorWheel, NoOp,
                    ImageStatistics)
