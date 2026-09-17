// Copyright (c) 2026 aopenfx contributors.
//
// A transform, against net.sf.openfx.TransformPlugin.
//
// The parameters are that plugin's, name for name and default for default --
// read off the installed plugin's own description rather than
// remembered, because a default that is nearly right is worse than one that is
// obviously wrong.
//
// This is the first AOFX effect whose region of definition *moves*. Blur's
// grows, which is a rectangle made bigger in place; a transform can put the
// picture somewhere else entirely, and every part of the host that relates two
// rectangles is exercised by that for the first time.
//
// What is not here, said plainly rather than left to be discovered:
//
//   * No motion blur. The plugin has motionBlur, shutter, shutterOffset and
//     shutterCustomOffset, and doing them means rendering the transform several
//     times per frame at different points in the shutter and accumulating. That
//     is a real feature and it is a different piece of work; a version that
//     ignored the parameters while showing them would be worse than one that
//     does not show them.
//
//   * No overlay interact. The plugin draws handles in the viewer. The host
//     already has its own gizmos and hooking those up is a host job.
//
// On speed: 5.5 ms against the plugin's 8.2 at 1920x1080 with the default cubic
// filter, best of five in an optimised build. Faster, and not dramatically --
// a transform is four taps per axis and some arithmetic, which is more work per
// pixel than a grade and far less than a blur. Measured by an equivalence test
// in the host this was written for, which is not part of this repository, with
// the image reader's own 147.6 ms subtracted: at that size reading the EXR
// costs twenty times either node, and a figure including it would say the same
// thing about every plugin ever measured.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "aofx/Entry.h"

#include "aofx_kernels_transform.h"

namespace {

constexpr const char* kKernel = "org.aopenfx.transform";
constexpr const char* kEntry = "transformMain";

/// A 3x3 affine, row major, bottom row implied.
struct Matrix {
    double a = 1.0, b = 0.0, c = 0.0;   ///< x' = a*x + b*y + c
    double d = 0.0, e = 1.0, f = 0.0;   ///< y' = d*x + e*y + f
};

Matrix multiply(const Matrix& l, const Matrix& r) {
    return Matrix{l.a * r.a + l.b * r.d, l.a * r.b + l.b * r.e,
                  l.a * r.c + l.b * r.f + l.c,
                  l.d * r.a + l.e * r.d, l.d * r.b + l.e * r.e,
                  l.d * r.c + l.e * r.f + l.f};
}

/// The inverse, or the identity where there is none.
///
/// A scale of zero is a legal thing to type and collapses the picture to a
/// line, which has no inverse. Returning the identity there means the node
/// shows the input rather than a frame of infinities -- a wrong picture is
/// still a picture somebody can see and undo.
Matrix inverse(const Matrix& m) {
    const double determinant = m.a * m.e - m.b * m.d;
    if (std::abs(determinant) < 1e-12) {
        return Matrix{};
    }
    const double inv = 1.0 / determinant;
    Matrix out;
    out.a = m.e * inv;
    out.b = -m.b * inv;
    out.d = -m.d * inv;
    out.e = m.a * inv;
    out.c = -(out.a * m.c + out.b * m.f);
    out.f = -(out.d * m.c + out.e * m.f);
    return out;
}

struct Settings {
    double translateX = 0.0, translateY = 0.0;
    double rotate = 0.0;
    double scaleX = 1.0, scaleY = 1.0;
    double skewX = 0.0, skewY = 0.0;
    bool   skewOrderYX = false;
    double centerX = 0.0, centerY = 0.0;
    bool   invert = false;
    int    filter = 3;
    bool   blackOutside = true;
    bool   doClamp = false;
};

/// Translate, rotate, skew and scale about a centre.
///
/// The order is the plugin's: move the centre to the origin, scale, skew,
/// rotate, then put it back and translate. Composing them in any other order
/// gives a different picture for the same numbers, which is why this is written
/// as five named matrices rather than one expression.
Matrix forwardOf(const Settings& s) {
    const double radians = s.rotate * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);

    const Matrix toOrigin{1, 0, -s.centerX, 0, 1, -s.centerY};
    const Matrix scale{s.scaleX, 0, 0, 0, s.scaleY, 0};
    const Matrix skew =
        s.skewOrderYX ? Matrix{s.skewX * s.skewY + 1.0, s.skewX, 0, s.skewY, 1, 0}
                      : Matrix{1, s.skewX, 0, s.skewY, s.skewX * s.skewY + 1.0, 0};
    // The sign is the plugin's, established by measurement rather than by
    // reasoning about which way is up. I had it the other way round first, and
    // the bench caught it in one run: translate, scale and skew all agreed to
    // zero code values while every rotation was 170 out -- at every filter,
    // impulse included, which is what said the filter was innocent and the
    // geometry guilty.
    //
    // Note what made that legible. The test's centre of rotation is off the
    // middle of the plate on purpose; about the middle of a square plate a
    // rotation and its mirror image cover the same rectangle and much of the
    // same picture, and the run would have looked like a filtering problem.
    const Matrix rotation{cosine, -sine, 0, sine, cosine, 0};
    const Matrix back{1, 0, s.centerX + s.translateX,
                      0, 1, s.centerY + s.translateY};

    return multiply(back,
                    multiply(rotation, multiply(skew, multiply(scale, toOrigin))));
}

Matrix matrixOf(const Settings& s) {
    const Matrix forward = forwardOf(s);
    return s.invert ? inverse(forward) : forward;
}

/// Matched byte for byte by `TransformParams` in transform.slang.
struct TransformUniforms {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t srcStride = 0;
    uint32_t dstStride = 0;
    int32_t  srcX1 = 0;
    int32_t  srcY1 = 0;
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
    int32_t  dstX1 = 0;
    int32_t  dstY1 = 0;
    float    m0 = 1, m1 = 0, m2 = 0;
    float    m3 = 0, m4 = 1, m5 = 0;
    uint32_t filter = 3;
    uint32_t blackOutside = 1;
    uint32_t doClamp = 0;
    uint32_t pad0 = 0;
};
static_assert(sizeof(TransformUniforms) == 80, "no padding, on any compiler");

const std::array<const char*, 9>& filters() {
    static const std::array<const char*, 9> kAll{
        {"impulse", "box", "bilinear", "cubic", "keys", "simon", "rifman",
         "mitchell", "parzen"}};
    return kAll;
}

class Transform final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.transform";
        into.label = "Transform";
        into.grouping = "Transform";
        into.description =
            "Translate, rotate, scale and skew about a centre, with a choice of "
            "resampling filter. No motion blur.";

        into.inputs.push_back(
            aofx::ClipDesc{"Source", "Source", false, false, false});

        aofx::ParamDesc translate;
        translate.name = "translate";
        translate.label = "Translate";
        translate.hint = "Translation along the x and y axes, in pixels.";
        translate.type = aofx::ParamType::Double;
        // A place in the picture, which is what gets it a handle in the viewer.
        // The host draws it; this file draws nothing.
        translate.role = aofx::ParamRole::Position;
        translate.dimension = 2;
        translate.defaults = {0.0, 0.0};
        into.params.push_back(translate);

        aofx::ParamDesc rotate;
        rotate.name = "rotate";
        rotate.label = "Rotate";
        rotate.hint = "Rotation in degrees about the centre.";
        rotate.type = aofx::ParamType::Double;
        rotate.role = aofx::ParamRole::Angle;
        rotate.defaults = {0.0};
        into.params.push_back(rotate);

        aofx::ParamDesc scale;
        scale.name = "scale";
        scale.label = "Scale";
        scale.hint = "Scale factor along the x and y axes.";
        scale.type = aofx::ParamType::Double;
        scale.role = aofx::ParamRole::Scale;
        scale.dimension = 2;
        scale.defaults = {1.0, 1.0};
        into.params.push_back(scale);

        aofx::ParamDesc uniform;
        uniform.name = "uniform";
        uniform.label = "Uniform";
        uniform.hint = "Use the X scale for both directions.";
        uniform.type = aofx::ParamType::Boolean;
        uniform.defaults = {0.0};
        into.params.push_back(uniform);

        for (const auto& [name, label] :
             std::vector<std::pair<const char*, const char*>>{
                 {"skewX", "Skew X"}, {"skewY", "Skew Y"}}) {
            aofx::ParamDesc skew;
            skew.name = name;
            skew.label = label;
            skew.hint = "Skew along the axis, about the centre.";
            skew.type = aofx::ParamType::Double;
            skew.defaults = {0.0};
            into.params.push_back(skew);
        }

        aofx::ParamDesc skewOrder;
        skewOrder.name = "skewOrder";
        skewOrder.label = "Skew Order";
        skewOrder.hint = "Which skew is applied first.";
        skewOrder.type = aofx::ParamType::Choice;
        skewOrder.choices = {aofx::ChoiceOption{"xy", "XY"},
                             aofx::ChoiceOption{"yx", "YX"}};
        skewOrder.defaults = {0.0};
        into.params.push_back(skewOrder);

        aofx::ParamDesc center;
        center.name = "center";
        center.label = "Center";
        center.hint =
            "The point rotation and scale turn about, in pixels. Starts in the "
            "middle of the frame at any format; drag the ring around the "
            "centre cross in the viewer to move it.";
        center.type = aofx::ParamType::Double;
        center.role = aofx::ParamRole::Position;
        center.dimension = 2;
        // Half the frame, at any format.
        //
        // It was `{0, 0}` -- the bottom-left corner -- which is nobody's idea
        // of where a transform turns. The gizmo, having no way to know, drew
        // its ring in the middle of the frame instead, so the handle said one
        // thing and the render did another: rotating turned the picture about
        // a corner while the ring sat still in the middle.
        //
        // A fraction rather than a number because a plugin cannot know the
        // project size when it describes itself. This is the same thing
        // OpenFX's `kOfxParamPropDefaultCoordinateSystem` says, which this
        // host has expanded for other people's plugins for a long time and
        // had no way to say for its own.
        center.defaultsNormalised = true;
        center.defaults = {0.5, 0.5};
        into.params.push_back(center);

        aofx::ParamDesc invert;
        invert.name = "invert";
        invert.label = "Invert";
        invert.hint = "Apply the inverse transform.";
        invert.type = aofx::ParamType::Boolean;
        invert.defaults = {0.0};
        into.params.push_back(invert);

        aofx::ParamDesc filter;
        filter.name = "filter";
        filter.label = "Filter";
        filter.hint =
            "How the source is resampled. The sharpening ones -- Keys, Simon "
            "and Rifman -- overshoot on purpose and can produce values outside "
            "the original range; Clamp is the switch that says whether that is "
            "wanted.";
        filter.type = aofx::ParamType::Choice;
        for (const char* name : filters()) {
            filter.choices.push_back(aofx::ChoiceOption{name, name});
        }
        // Cubic, which is the plugin's default too.
        filter.defaults = {3.0};
        into.params.push_back(filter);

        aofx::ParamDesc clamp;
        clamp.name = "clamp";
        clamp.label = "Clamp";
        clamp.hint = "Clamp the result to [0, 1].";
        clamp.type = aofx::ParamType::Boolean;
        clamp.defaults = {0.0};
        into.params.push_back(clamp);

        aofx::ParamDesc blackOutside;
        blackOutside.name = "black_outside";
        blackOutside.label = "Black outside";
        blackOutside.hint =
            "Outside the source is transparent black. Off, the edge pixels are "
            "repeated outwards instead.";
        blackOutside.type = aofx::ParamType::Boolean;
        blackOutside.defaults = {1.0};
        into.params.push_back(blackOutside);
    }

    [[nodiscard]] std::vector<aofx::KernelDesc> kernels() const override {
        return {aofx::KernelDesc{kKernel, kEntry, k_transform, k_transformBytes}};
    }

    /// Where the picture ends up.
    ///
    /// The first region in this SDK that moves rather than grows. All four
    /// corners are carried through the transform and the bounding box of the
    /// four is taken -- not the two opposite corners, which is right only while
    /// the transform is axis-aligned and silently wrong the moment anybody
    /// rotates anything.
    /// The part of the input this output is made from: the output's corners
    /// carried back through the inverse of the transform, and the box round
    /// them, grown by the filter's reach.
    ///
    /// The default -- the same rectangle as the output -- is wrong for a node
    /// that moves pixels, and it was hidden for a year by sources that
    /// declared the whole frame: whatever a Transform asked for, the input
    /// had it. With a source declaring its own box, a logo scaled down asked
    /// its input for the small rectangle it was producing, got the part of
    /// the logo that happened to lie under it, and came out cut.
    [[nodiscard]] std::vector<aofx::Rect> regionOfInterest(
        const RegionQuery& query, const aofx::Rect& output) const override {
        const std::vector<aofx::Rect>& inputRods =
            query.inputRods != nullptr ? *query.inputRods : std::vector<aofx::Rect>{};
        const std::vector<aofx::ParamValue>& params =
            query.params != nullptr ? *query.params : std::vector<aofx::ParamValue>{};
        std::vector<aofx::Rect> wanted(inputRods.size(), output);
        if (inputRods.empty() || output.isEmpty()) {
            return wanted;
        }
        const aofx::Rect& in = inputRods.front();
        // The rectangles are at the render's scale; the knobs are canonical.
        Settings settings = settingsFrom(params);
        bringToScale(settings, query.scaleX, query.scaleY);
        if (!hasParam(params, "center")) {
            settings.centerX = (in.x1 + in.x2) * 0.5;
            settings.centerY = (in.y1 + in.y2) * 0.5;
        }
        const Matrix back = inverse(matrixOf(settings));
        const std::array<std::pair<double, double>, 4> corners{
            {{static_cast<double>(output.x1), static_cast<double>(output.y1)},
             {static_cast<double>(output.x2), static_cast<double>(output.y1)},
             {static_cast<double>(output.x1), static_cast<double>(output.y2)},
             {static_cast<double>(output.x2), static_cast<double>(output.y2)}}};
        double minX = 0, minY = 0, maxX = 0, maxY = 0;
        for (size_t i = 0; i < corners.size(); ++i) {
            const double x = back.a * corners[i].first + back.b * corners[i].second + back.c;
            const double y = back.d * corners[i].first + back.e * corners[i].second + back.f;
            if (i == 0) {
                minX = maxX = x;
                minY = maxY = y;
            } else {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
        // The filter's reach: a cubic reads two pixels either side.
        const int margin = 3;
        wanted.front() = aofx::Rect{static_cast<int>(std::floor(minX)) - margin,
                                    static_cast<int>(std::floor(minY)) - margin,
                                    static_cast<int>(std::ceil(maxX)) + margin,
                                    static_cast<int>(std::ceil(maxY)) + margin};
        return wanted;
    }

    [[nodiscard]] aofx::Rect regionOfDefinition(
        double time, double scaleX, double scaleY,
        const std::vector<aofx::Rect>& inputRods,
        const std::vector<aofx::ParamValue>& params) const override {
        const aofx::Rect in = aofx::Effect::regionOfDefinition(time, inputRods, params);
        if (in.isEmpty()) {
            return in;
        }
        // The input's rectangle is already at this render's scale, so the
        // transform put on it has to be as well or the region does not match
        // the pixels the render will write into it.
        Settings settings = settingsFrom(params);
        bringToScale(settings, scaleX, scaleY);
        if (!hasParam(params, "center")) {
            // The input's middle, as the render uses; `in` is already at
            // this scale, so it is set after the scaling above.
            settings.centerX = (in.x1 + in.x2) * 0.5;
            settings.centerY = (in.y1 + in.y2) * 0.5;
        }
        const Matrix m = matrixOf(settings);

        const std::array<std::pair<double, double>, 4> corners{
            {{static_cast<double>(in.x1), static_cast<double>(in.y1)},
             {static_cast<double>(in.x2), static_cast<double>(in.y1)},
             {static_cast<double>(in.x1), static_cast<double>(in.y2)},
             {static_cast<double>(in.x2), static_cast<double>(in.y2)}}};

        double minX = 0, minY = 0, maxX = 0, maxY = 0;
        for (size_t i = 0; i < corners.size(); ++i) {
            const double x = m.a * corners[i].first + m.b * corners[i].second + m.c;
            const double y = m.d * corners[i].first + m.e * corners[i].second + m.f;
            if (i == 0) {
                minX = maxX = x;
                minY = maxY = y;
            } else {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
        // Outwards on both sides, and then one pixel more all round.
        //
        // Rounding inwards would crop a row of pixels the transform genuinely
        // produced. The extra ring is the filter's tail: a resampled edge pixel
        // draws on neighbours that lie outside the geometric image, so the
        // picture really does reach one pixel further than the corners say.
        //
        // One pixel is also exactly what the plugin adds, which is not a
        // coincidence worth assuming -- it was measured. With the ring, our
        // region matches the plugin's to the pixel on translation, scale and
        // skew; without it we were two narrower and one off in each axis every
        // time. Note that a cubic filter's support is two, so both this and the
        // plugin clip the last of that tail; matching the plugin was worth more
        // than being right alone.
        aofx::Rect out;
        out.x1 = static_cast<int>(std::floor(minX)) - 1;
        out.y1 = static_cast<int>(std::floor(minY)) - 1;
        out.x2 = static_cast<int>(std::ceil(maxX)) + 1;
        out.y2 = static_cast<int>(std::ceil(maxY)) + 1;
        return out;
    }

    /// A transform that moves nothing.
    ///
    /// The state a node is in when it is made, and the state it returns to
    /// whenever somebody animates back through zero -- so it is worth the few
    /// comparisons to hand the picture straight back rather than resample it.
    /// Resampling an identity is not free and it is not lossless: every filter
    /// but Impulse and Bilinear changes the picture even when nothing moves,
    /// which is what the plugin's (+) marks mean.
    [[nodiscard]] bool isIdentity(const aofx::RenderRequest& request) const override {
        return request.number("translate", 0.0, 0) == 0.0 &&
               request.number("translate", 0.0, 1) == 0.0 &&
               request.number("rotate", 0.0) == 0.0 &&
               request.number("scale", 1.0, 0) == 1.0 &&
               request.number("scale", 1.0, 1) == 1.0 &&
               request.number("skewX", 0.0) == 0.0 &&
               request.number("skewY", 0.0) == 0.0;
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

        Settings settings;
        settings.translateX = request.number("translate", 0.0, 0);
        settings.translateY = request.number("translate", 0.0, 1);
        settings.rotate = request.number("rotate", 0.0);
        settings.scaleX = request.number("scale", 1.0, 0);
        settings.scaleY = request.number("uniform", 0.0) != 0.0
                              ? settings.scaleX
                              : request.number("scale", 1.0, 1);
        settings.skewX = request.number("skewX", 0.0);
        settings.skewY = request.number("skewY", 0.0);
        settings.skewOrderYX = request.number("skewOrder", 0.0) != 0.0;
        // The *input's* middle when nothing has been stored: the centre of
        // the picture's own bounding box, which is where a person expects a
        // transform to turn and scale a logo, a lottie or any material that
        // arrives with a box of its own. It used to be three different
        // points -- the origin in the region, the output's middle in the
        // render, the format's middle in the gizmo -- and with the box now
        // reported honestly by every source the three had to be one.
        //
        // A node keeps only what was set, so a `center` nobody has touched
        // never reaches here at all -- and a fallback written as `0.0` is the
        // bottom-left corner, which is what this turned about for as long as
        // the parameter went untouched. The declared default cannot be used
        // as the fallback because it is a fraction of a project the effect
        // cannot see; the region it has been asked to fill is the same
        // rectangle, and it can -- and since that rectangle is already at this
        // render's scale, the middle is divided back out so it lands in the
        // canonical numbers `bringToScale` below is expecting.
        // The input's whole picture, not the piece delivered: the piece
        // follows what this node asked for, and a pivot that followed it
        // moved every time the translate did.
        const aofx::Rect& frame = source != nullptr && !source->rod.isEmpty() ? source->rod
                                  : source != nullptr && source->buffer.isValid() ? source->buffer.rect
                                                                                  : request.outputRod;
        const double middleX = (frame.x1 + frame.x2) * 0.5;
        const double middleY = (frame.y1 + frame.y2) * 0.5;
        const double sx = request.scaleX > 0.0 ? request.scaleX : 1.0;
        const double sy = request.scaleY > 0.0 ? request.scaleY : 1.0;
        settings.centerX = request.number("center", middleX / sx, 0);
        settings.centerY = request.number("center", middleY / sy, 1);
        settings.invert = request.number("invert", 0.0) != 0.0;
        settings.filter = static_cast<int>(request.number("filter", 3.0));
        settings.blackOutside = request.number("black_outside", 1.0) != 0.0;
        settings.doClamp = request.number("clamp", 0.0) != 0.0;

        // The lengths, into this render's own pixels. Everything above reads
        // canonical numbers; the kernel walks the buffer it was given.
        bringToScale(settings, sx, sy);

        // The kernel maps output pixels backwards, so it wants the inverse --
        // inverted once here rather than in every thread.
        const Matrix back = inverse(matrixOf(settings));

        TransformUniforms uniforms;
        uniforms.width = static_cast<uint32_t>(target->buffer.width);
        uniforms.height = static_cast<uint32_t>(target->buffer.height);
        uniforms.srcStride = static_cast<uint32_t>(source->buffer.stride);
        uniforms.dstStride = static_cast<uint32_t>(target->buffer.stride);
        uniforms.srcX1 = source->buffer.rect.x1;
        uniforms.srcY1 = source->buffer.rect.y1;
        uniforms.srcWidth = static_cast<uint32_t>(source->buffer.width);
        uniforms.srcHeight = static_cast<uint32_t>(source->buffer.height);
        uniforms.dstX1 = target->buffer.rect.x1;
        uniforms.dstY1 = target->buffer.rect.y1;
        uniforms.m0 = static_cast<float>(back.a);
        uniforms.m1 = static_cast<float>(back.b);
        uniforms.m2 = static_cast<float>(back.c);
        uniforms.m3 = static_cast<float>(back.d);
        uniforms.m4 = static_cast<float>(back.e);
        uniforms.m5 = static_cast<float>(back.f);
        uniforms.filter = static_cast<uint32_t>(
            std::clamp(settings.filter, 0, static_cast<int>(filters().size()) - 1));
        uniforms.blackOutside = settings.blackOutside ? 1u : 0u;
        uniforms.doClamp = settings.doClamp ? 1u : 0u;

        return request.gpu->run(
            kernel,
            aofx::Grid{static_cast<uint32_t>(target->buffer.width),
                       static_cast<uint32_t>(target->buffer.height), 1},
            {source->buffer, target->buffer}, &uniforms, sizeof(uniforms));
    }

private:
    /// The same settings, out of a plain parameter list.
    ///
    /// regionOfDefinition is asked before there is a render request, so it gets
    /// the values this way. Two readers for one set of parameters is a place
    /// for them to drift apart, so both funnel into one Settings.
    /// Brings the lengths in a Settings to the scale a render is being made
    /// at, and leaves everything dimensionless alone.
    ///
    /// `translate` and `center` are pixels somebody typed; a rotation, a skew
    /// and a scale are ratios and mean the same at any size. Unconverted, a
    /// translate of two hundred canonical pixels moved a proxy render by two
    /// hundred of *its* pixels -- twice as far -- and a rotation turned about
    /// a centre twice as far from the origin as it should be. Both for exactly
    /// as long as somebody was dragging, both gone when they let go, which is
    /// what "it shows the wrong thing and then repositions" is.
    static void bringToScale(Settings& s, double scaleX, double scaleY) {
        const double sx = scaleX > 0.0 ? scaleX : 1.0;
        const double sy = scaleY > 0.0 ? scaleY : 1.0;
        s.translateX *= sx;
        s.translateY *= sy;
        s.centerX *= sx;
        s.centerY *= sy;
    }

    [[nodiscard]] static bool hasParam(const std::vector<aofx::ParamValue>& params,
                                       const char* name) {
        for (const aofx::ParamValue& value : params) {
            if (value.name == name && !value.numbers.empty()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static Settings settingsFrom(
        const std::vector<aofx::ParamValue>& params) {
        Settings out;
        for (const aofx::ParamValue& value : params) {
            if (value.name == "translate") {
                out.translateX = value.number(0, 0.0);
                out.translateY = value.number(1, 0.0);
            } else if (value.name == "rotate") {
                out.rotate = value.number(0, 0.0);
            } else if (value.name == "scale") {
                out.scaleX = value.number(0, 1.0);
                out.scaleY = value.number(1, 1.0);
            } else if (value.name == "skewX") {
                out.skewX = value.number(0, 0.0);
            } else if (value.name == "skewY") {
                out.skewY = value.number(0, 0.0);
            } else if (value.name == "skewOrder") {
                out.skewOrderYX = value.number(0, 0.0) != 0.0;
            } else if (value.name == "center") {
                out.centerX = value.number(0, 0.0);
                out.centerY = value.number(1, 0.0);
            } else if (value.name == "invert") {
                out.invert = value.number(0, 0.0) != 0.0;
            }
        }
        for (const aofx::ParamValue& value : params) {
            if (value.name == "uniform" && value.number(0, 0.0) != 0.0) {
                out.scaleY = out.scaleX;
            }
        }
        return out;
    }
};

}   // namespace

AOFX_EXPORT_EFFECTS(Transform)
