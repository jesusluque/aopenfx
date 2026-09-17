// Copyright (c) 2026 aopenfx contributors.
//
// A picture pinned to four corners.
//
// The node that puts the advertisement on the ground. Everything upstream of it
// in a virtual-advertising graph exists to work out where those four corners
// are, and everything downstream exists to make what lands there look like it
// was painted rather than pasted.
//
// The map is computed here and the pixels are moved in the kernel, and the
// split is the usual one: a homography is eight numbers and a 3x3 inverse,
// which is nothing on a CPU and would be the same arithmetic repeated two
// million times on a GPU.
//
// BACKWARDS, ALWAYS
//
// The kernel maps each *output* pixel back to where it came from. Forwards --
// pushing each source pixel to where it lands -- leaves holes wherever the quad
// is larger than the source and writes the same pixel many times wherever it is
// smaller, and no amount of filtering fixes a hole. That is not a preference,
// it is why every resampler in this repository is written this way.
//
// THE CROWN
//
// A road is not a plane. It is domed towards the centre so that water runs off,
// and over the twelve metres an advertisement occupies that dome is several
// pixels of disagreement between where a homography says a point is and where
// it is. Painted onto asphalt, those pixels read as the advertisement sliding
// under the wheels -- which is the single thing that gives a virtual insert
// away.
//
// `crown` is that one degree of freedom, as a 3x3 mesh of offsets built here
// and interpolated in the kernel. The kernel takes a mesh of any size because
// the general answer is a mesh fitted to the ground rather than a scalar
// somebody dialled in; when there is an edge in this graph that can carry a
// field of numbers, that is where it plugs in, and nothing in the kernel
// changes.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "aofx/Effect.h"
#include "aofx/Entry.h"

#include "aofx_kernels_cornerpin.h"

namespace {

/// Matched byte for byte by `PinParams` in cornerpin.slang.
struct PinUniforms {
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
    uint32_t srcStride = 0;
    int32_t  srcOriginX = 0;
    int32_t  srcOriginY = 0;
    uint32_t dstWidth = 0;
    uint32_t dstHeight = 0;
    uint32_t dstStride = 0;
    int32_t  dstOriginX = 0;
    int32_t  dstOriginY = 0;
    float    h0 = 1.0F, h1 = 0.0F, h2 = 0.0F;
    float    h3 = 0.0F, h4 = 1.0F, h5 = 0.0F;
    float    h6 = 0.0F, h7 = 0.0F;
    uint32_t meshX = 1;
    uint32_t meshY = 1;
    float    softness = 1.0F;
    uint32_t filter = 1;
    uint32_t blackOutside = 1;
    float    m[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float    shutter = 0.5F;
    uint32_t blurOn = 0;
};
static_assert(sizeof(PinUniforms) == 132, "must match PinParams exactly");

using Point = std::array<double, 2>;
/// Row major, nine numbers, the bottom right one carried like the rest so the
/// multiply and the inverse do not have to special-case it.
using Matrix = std::array<double, 9>;

double numberOf(const aofx::RenderRequest& request, const char* name,
                double fallback, size_t component = 0) {
    for (const aofx::ParamValue& param : request.params) {
        if (param.name == name && param.numbers.size() > component) {
            return param.numbers[component];
        }
    }
    return fallback;
}

/// The map from the unit square's corners to four points, in the order
/// (0,0), (1,0), (1,1), (0,1).
///
/// Heckbert's construction. The degenerate branch is not an optimisation: when
/// the quad is a parallelogram the denominator below is exactly zero, and a
/// perspective map is not what a parallelogram wants anyway.
Matrix squareToQuad(const std::array<Point, 4>& q) {
    const double sx = q[0][0] - q[1][0] + q[2][0] - q[3][0];
    const double sy = q[0][1] - q[1][1] + q[2][1] - q[3][1];
    if (std::abs(sx) < 1e-12 && std::abs(sy) < 1e-12) {
        return Matrix{q[1][0] - q[0][0], q[3][0] - q[0][0], q[0][0],
                      q[1][1] - q[0][1], q[3][1] - q[0][1], q[0][1],
                      0.0,               0.0,               1.0};
    }
    const double dx1 = q[1][0] - q[2][0];
    const double dx2 = q[3][0] - q[2][0];
    const double dy1 = q[1][1] - q[2][1];
    const double dy2 = q[3][1] - q[2][1];
    const double den = dx1 * dy2 - dx2 * dy1;
    if (std::abs(den) < 1e-12) {
        return Matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
    }
    const double g = (sx * dy2 - dx2 * sy) / den;
    const double h = (dx1 * sy - sx * dy1) / den;
    return Matrix{q[1][0] - q[0][0] + g * q[1][0],
                  q[3][0] - q[0][0] + h * q[3][0],
                  q[0][0],
                  q[1][1] - q[0][1] + g * q[1][1],
                  q[3][1] - q[0][1] + h * q[3][1],
                  q[0][1],
                  g,
                  h,
                  1.0};
}

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out[size_t(row) * 3 + size_t(col)] =
                a[size_t(row) * 3 + 0] * b[0 + size_t(col)] +
                a[size_t(row) * 3 + 1] * b[3 + size_t(col)] +
                a[size_t(row) * 3 + 2] * b[6 + size_t(col)];
        }
    }
    return out;
}

Matrix inverse(const Matrix& m) {
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                       m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::abs(det) < 1e-12) {
        return Matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
    }
    const double s = 1.0 / det;
    return Matrix{(m[4] * m[8] - m[5] * m[7]) * s,
                  (m[2] * m[7] - m[1] * m[8]) * s,
                  (m[1] * m[5] - m[2] * m[4]) * s,
                  (m[5] * m[6] - m[3] * m[8]) * s,
                  (m[0] * m[8] - m[2] * m[6]) * s,
                  (m[2] * m[3] - m[0] * m[5]) * s,
                  (m[3] * m[7] - m[4] * m[6]) * s,
                  (m[1] * m[6] - m[0] * m[7]) * s,
                  (m[0] * m[4] - m[1] * m[3]) * s};
}

/// Normalised so the bottom right is one, which is what the kernel assumes so
/// that eight floats are enough.
Matrix normalised(const Matrix& m) {
    if (std::abs(m[8]) < 1e-12) {
        return m;
    }
    Matrix out = m;
    for (double& value : out) {
        value /= m[8];
    }
    return out;
}

std::array<Point, 4> cornersFrom(const aofx::RenderRequest& request,
                                 const aofx::Rect& fallback) {
    const auto def = [&](int index, int which) {
        const double x1 = fallback.x1;
        const double y1 = fallback.y1;
        const double x2 = fallback.x2;
        const double y2 = fallback.y2;
        const double xs[4] = {x1, x2, x2, x1};
        const double ys[4] = {y1, y1, y2, y2};
        return which == 0 ? xs[index] : ys[index];
    };
    std::array<Point, 4> quad{};
    for (int index = 0; index < 4; ++index) {
        const std::string name = "corner" + std::to_string(index + 1);
        quad[size_t(index)] = {
            numberOf(request, name.c_str(), def(index, 0), 0),
            numberOf(request, name.c_str(), def(index, 1), 1)};
    }
    return quad;
}

class CornerPin : public aofx::Effect {
public:
    /// The other node of this file: the same pin with the supersampled
    /// filter, under its own name. A separate node rather than a fourth
    /// choice on this one, so a graph that reads "CornerPinSS" says what
    /// it does, and a document with a Corner Pin in it renders as it did.
    explicit CornerPin(bool supersampled = false) : supersampled_(supersampled) {}

    void describe(aofx::EffectDesc& into) override {
        into.identifier = supersampled_ ? "org.aopenfx.cornerpinss"
                                        : "org.aopenfx.cornerpin";
        into.label = supersampled_ ? "CornerPinSS" : "Corner Pin";
        into.grouping = "Track";
        into.description =
            std::string(supersampled_
                            ? "Corner Pin with a supersampled filter: for a surface seen "
                              "at a grazing angle, where the far edge of the picture is "
                              "a few pixels tall and a single tap shimmers. Everything "
                              "else is the Corner Pin.\n\n"
                            : "") +
            "Pins the picture to four corners, with perspective. The node that "
            "puts a flat thing onto a surface in a shot -- an advertisement on "
            "the asphalt, a screen in a window, a poster on a wall.\n\n"
            "`Crown` bends it. A road is domed so that water runs off, and over "
            "the length of an advertisement that dome is several pixels the "
            "perspective cannot account for; left in, they read as the "
            "advertisement sliding under the wheels.";

        aofx::ClipDesc source;
        source.name = "Source";
        source.label = "Source";
        source.passThrough = true;
        into.inputs.push_back(source);

        // Where a track comes in, and it is a second input rather than the
        // first for a reason worth stating: the numbers ride on a picture, and
        // the picture a corner pin *warps* is the artwork. The picture that
        // knows where the surface went is the plate, which is a different
        // branch of the graph entirely.
        //
        // Nothing is read from this clip's pixels. It is here to carry what is
        // attached to them, which is the price of hanging data on images and
        // is cheap: the plate is already rendered for the merge below.
        aofx::ClipDesc track;
        track.name = "Track";
        track.label = "Track";
        track.optional = true;
        // The planes too, for the one a GeometricTrack writes its mesh in.
        track.wantsPlanes = true;
        into.inputs.push_back(track);

        into.outputs.push_back(
            aofx::PlaneDesc{"Color", "Colour", {"R", "G", "B", "A"}});

        // Anticlockwise from the bottom left, which is the order every corner
        // pin has used since Cineon and the order the unit square is written in
        // wherever this arithmetic appears in a textbook. Getting it wrong is
        // not subtle -- the picture arrives mirrored -- but it is worth saying
        // rather than leaving four numbered controls to be discovered.
        static const char* labels[4] = {"Bottom left", "Bottom right",
                                        "Top right", "Top left"};
        static const double defaults[4][2] = {
            {0.0, 0.0}, {1920.0, 0.0}, {1920.0, 1080.0}, {0.0, 1080.0}};
        for (int index = 0; index < 4; ++index) {
            aofx::ParamDesc corner;
            corner.name = "corner" + std::to_string(index + 1);
            corner.label = labels[index];
            corner.type = aofx::ParamType::Double;
            corner.dimension = 2;
            corner.defaults = {defaults[index][0], defaults[index][1]};
            corner.hint =
                "Where this corner of the picture ends up, in the project's "
                "own pixels.";
            into.params.push_back(corner);
        }

        aofx::ParamDesc crown;
        crown.name = "crown";
        crown.label = "Crown";
        crown.type = aofx::ParamType::Double;
        crown.defaults = {0.0};
        crown.displayMin = {-40.0};
        crown.displayMax = {40.0};
        crown.hardMin = {-400.0};
        crown.hardMax = {400.0};
        crown.hint =
            "Bow the middle of the pin, in source pixels. Positive lifts it, "
            "which is what a road does between its gutters. Zero is a true "
            "plane and costs nothing.";
        into.params.push_back(crown);

        aofx::ParamDesc softness;
        softness.name = "softness";
        softness.label = "Edge softness";
        softness.type = aofx::ParamType::Double;
        softness.defaults = {1.0};
        softness.displayMin = {0.0};
        softness.displayMax = {20.0};
        softness.hardMin = {0.0};
        softness.hardMax = {200.0};
        softness.hint =
            "How wide the ramp at the edge of the pinned picture is, in source "
            "pixels. A hard edge on something that moves a fraction of a pixel "
            "a frame crawls, and the crawl is what makes a composite look stuck "
            "on rather than part of the shot.";
        into.params.push_back(softness);

        aofx::ParamDesc filter;
        filter.name = "filter";
        filter.label = "Filter";
        filter.type = aofx::ParamType::Choice;
        if (supersampled_) {
            filter.choices = {{"supersampled", "Supersampled"},
                              {"bilinear", "Bilinear"},
                              {"nearest", "Nearest"}};
            filter.defaults = {0.0};
            filter.hint =
                "How the source is read between its pixels. Supersampled reads "
                "as many taps as the source is squeezed by, each way, and "
                "averages them: a floor advertisement seen from a low camera "
                "is a few pixels tall at its far edge, and one bilinear tap "
                "there shimmers as the camera moves. Up to eight by eight "
                "taps, sized by the map itself. Bilinear and Nearest are here "
                "to compare against.";
        } else {
            filter.choices = {{"nearest", "Nearest"}, {"bilinear", "Bilinear"}};
            filter.defaults = {1.0};
            filter.hint =
                "How the source is read between its pixels. Nearest is for "
                "looking at what the map is doing, not for delivering. For a "
                "surface seen at a grazing angle, CornerPinSS.";
        }
        into.params.push_back(filter);

        aofx::ParamDesc black;
        black.name = "black_outside";
        black.label = "Black outside";
        black.type = aofx::ParamType::Boolean;
        black.defaults = {1.0};
        black.hint =
            "Outside the source is transparent. Off, the edge pixels repeat "
            "outwards, which is what you want when the pin is being used to "
            "unwarp something rather than to place it.";
        into.params.push_back(black);

        aofx::ParamDesc motionBlur;
        motionBlur.name = "motion_blur";
        motionBlur.label = "Motion blur";
        motionBlur.type = aofx::ParamType::Boolean;
        motionBlur.defaults = {1.0};
        motionBlur.hint =
            "Blur the pinned picture along the motion the tracker measured "
            "between this frame and the last. A camera smears paint on the "
            "road by the shutter's share of the movement; a pin that stays "
            "crisp while the road blurs is what gives it away. Needs a "
            "tracker on Track that hangs `motion` on its picture.";
        into.params.push_back(motionBlur);
        aofx::ParamDesc shutter;
        shutter.name = "shutter";
        shutter.label = "Shutter";
        shutter.type = aofx::ParamType::Double;
        shutter.defaults = {0.5};
        shutter.hardMin = {0.0};
        shutter.displayMax = {1.0};
        shutter.hint = "The fraction of a frame the exposure lasts: 0.5 is a 180-degree shutter, the film default.";
        into.params.push_back(shutter);

        aofx::ParamDesc invert;
        invert.name = "invert";
        invert.label = "Invert";
        invert.type = aofx::ParamType::Boolean;
        invert.defaults = {0.0};
        invert.hint =
            "Map the other way: take what is inside those four corners and "
            "straighten it into the frame. For measuring a surface, or for "
            "painting on one and putting the paint back.";
        into.params.push_back(invert);
    }

    std::vector<aofx::KernelDesc> kernels() const override {
        return {aofx::KernelDesc{"pinBlur", "pinBlur", k_cornerpin, k_cornerpinBytes},
                aofx::KernelDesc{"pinWarp", "pinWarp", k_cornerpin,
                                 k_cornerpinBytes}};
    }

    /// Where the picture ends up: the bounding box of the four corners, and one
    /// pixel more all round for the filter's tail.
    ///
    /// Inverted, it is the input's own rectangle -- what comes back is the
    /// straightened quad, and that is the frame.
    ///
    /// The scaled question, because the typed corners are canonical pixels and
    /// the rectangles arrive at the render's scale: a half-size render used to
    /// get a region twice as far out as its picture.
    [[nodiscard]] aofx::Rect regionOfDefinition(
        double time, double scaleX, double scaleY,
        const std::vector<aofx::Rect>& inputRods,
        const std::vector<aofx::ParamValue>& params) const override {
        const double sx = scaleX > 0.0 ? scaleX : 1.0;
        const double sy = scaleY > 0.0 ? scaleY : 1.0;
        // A connected Track makes the corners dynamic: they arrive on the
        // picture at render time, and the authored parameters below are only
        // the seed. A region computed from the seed is a box around where
        // the quad USED to be -- and everything downstream intersects
        // against it, which is what was eating the artwork as the tracked
        // quad moved away from its authored home. With a tracker driving,
        // the honest region is the tracked frame itself.
        if (inputRods.size() >= 2 && !inputRods[1].isEmpty()) {
            return inputRods[1];
        }
        const aofx::Rect in =
            aofx::Effect::regionOfDefinition(time, inputRods, params);
        if (in.isEmpty()) {
            return in;
        }
        double invert = 0.0;
        for (const aofx::ParamValue& value : params) {
            if (value.name == "invert" && !value.numbers.empty()) {
                invert = value.numbers.front();
            }
        }
        if (invert != 0.0) {
            return in;
        }

        double minX = 0, minY = 0, maxX = 0, maxY = 0;
        bool   first = true;
        for (int index = 0; index < 4; ++index) {
            const std::string name = "corner" + std::to_string(index + 1);
            const double xs[4] = {double(in.x1), double(in.x2), double(in.x2),
                                  double(in.x1)};
            const double ys[4] = {double(in.y1), double(in.y1), double(in.y2),
                                  double(in.y2)};
            double x = xs[index];
            double y = ys[index];
            for (const aofx::ParamValue& value : params) {
                if (value.name == name && value.numbers.size() >= 2) {
                    // Typed corners are canonical; the fallback, the input's
                    // own rectangle, is already at this render's scale.
                    x = value.numbers[0] * sx;
                    y = value.numbers[1] * sy;
                }
            }
            if (first) {
                minX = maxX = x;
                minY = maxY = y;
                first = false;
            } else {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
        return aofx::Rect{int(std::floor(minX)) - 1, int(std::floor(minY)) - 1,
                          int(std::ceil(maxX)) + 1, int(std::ceil(maxY)) + 1};
    }

    /// All of it, always.
    ///
    /// A corner pin maps its source somewhere else in the frame, so there is no
    /// relationship between the rectangle it is asked to fill and the part of
    /// the source it will read. Asking for the output's rectangle -- which is
    /// what an effect gets if it does not answer -- gave this node the part of
    /// the artwork that happened to lie underneath the quad, and for artwork
    /// that lives at the origin and a quad in the middle of the frame that is
    /// most of it missing.
    ///
    /// The exact answer is the quad back-projected through the inverse map,
    /// which is a rectangle no smaller than this one in the cases that matter
    /// and is more arithmetic to reach. The whole input is correct, cheap to
    /// say, and for a piece of artwork -- which is what gets pinned -- is not
    /// large.
    [[nodiscard]] std::vector<aofx::Rect> regionOfInterest(
        double /*time*/, const aofx::Rect& output,
        const std::vector<aofx::Rect>& inputRods,
        const std::vector<aofx::ParamValue>& /*params*/) const override {
        std::vector<aofx::Rect> wanted;
        wanted.reserve(inputRods.size());
        for (const aofx::Rect& rod : inputRods) {
            wanted.push_back(rod.isEmpty() ? output : rod);
        }
        return wanted;
    }

    bool process(const aofx::RenderRequest& request) override {
        const aofx::InputPlane*  source = request.input("Source");
        const aofx::OutputPlane* target = request.output("Color");
        if (source == nullptr || target == nullptr ||
            !source->buffer.isValid() || !target->buffer.isValid()) {
            return false;
        }

        PinUniforms uniforms;
        uniforms.srcWidth = uint32_t(source->buffer.width);
        uniforms.srcHeight = uint32_t(source->buffer.height);
        uniforms.srcStride = uint32_t(source->buffer.stride);
        uniforms.dstWidth = uint32_t(target->buffer.width);
        uniforms.dstHeight = uint32_t(target->buffer.height);
        uniforms.dstStride = uint32_t(target->buffer.stride);
        uniforms.dstOriginX = target->buffer.rect.x1;
        uniforms.dstOriginY = target->buffer.rect.y1;
        uniforms.srcOriginX = source->buffer.rect.x1;
        uniforms.srcOriginY = source->buffer.rect.y1;
        uniforms.softness =
            float(std::max(numberOf(request, "softness", 1.0), 0.0));
        // The kernel's numbering is nearest 0, bilinear 1, supersampled 2;
        // CornerPinSS lists its choices the other way round so the one it
        // is for comes first.
        {
            const int chosen = std::clamp(
                static_cast<int>(numberOf(request, "filter", supersampled_ ? 0.0 : 1.0)), 0, 2);
            uniforms.filter = static_cast<uint32_t>(supersampled_ ? 2 - chosen : chosen);
        }
        uniforms.blackOutside =
            numberOf(request, "black_outside", 1.0) != 0.0 ? 1u : 0u;

        // The source quad is the input's own rectangle. A `from` set of corners
        // would be the general case and this is the case that gets used: put
        // *this picture* into *those four corners*. Inverting covers the other
        // direction, which is what a `from` set is really for.
        const aofx::Rect in = source->buffer.rect;
        const std::array<Point, 4> from{
            {{double(in.x1), double(in.y1)},
             {double(in.x2), double(in.y1)},
             {double(in.x2), double(in.y2)},
             {double(in.x1), double(in.y2)}}};
        // Where the corners come from, in order of authority.
        //
        // A tracker upstream hangs eight numbers on the picture, and if they
        // are there they win: that is what makes this the same node whether it
        // is driven by a track or by hand, rather than two nodes that have to
        // be kept in step. Absent -- which is the ordinary case -- the
        // parameters answer, and nothing about the node changes.
        //
        // Eight exactly. A shorter row is somebody's mistake and the honest
        // response is to ignore it rather than to read past its end; a longer
        // one is a convention this node does not know and is not entitled to
        // guess at.
        std::array<Point, 4> to = cornersFrom(request, in);
        // Typed corners are in the document's pixels; the fallback -- the
        // input's own rectangle -- is already at this render's scale.
        const bool typed = request.param("corner1") != nullptr;
        // The Track input first, then the Source, then the parameters. The
        // middle one is not redundant: a pin placed directly below a tracker,
        // warping the plate itself, has the numbers on its Source and nothing
        // on Track, and asking somebody to wire the same picture into two
        // inputs to make that work would be a worse node.
        const aofx::InputPlane* carrier = request.input("Track");
        // A Track wired and saying nothing is a tracker that has lost its
        // target -- the region is out of the picture, or never found --
        // and the picture goes nowhere: transparent, rather than pinned to
        // the typed corners as though nothing were wired.
        const bool trackSilent = carrier != nullptr && carrier->buffer.isValid() &&
                                 carrier->value("corners") == nullptr;
        if (carrier == nullptr || carrier->value("corners") == nullptr) {
            carrier = source;
        }
        if (const std::vector<float>* tracked = carrier->value("corners");
            tracked != nullptr && tracked->size() == 8) {
            for (int index = 0; index < 4; ++index) {
                to[size_t(index)] = {double((*tracked)[size_t(index) * 2]),
                                     double((*tracked)[size_t(index) * 2 + 1])};
            }
        }
        // The corners -- typed or tracked -- are in the document's pixels,
        // and this render may be at a fraction of them: a viewer at half
        // size. Without this the pin landed twice as far out in the window
        // as it did from the command line, and the two have to agree.
        if (typed || (carrier->value("corners") != nullptr && carrier->value("corners")->size() == 8)) {
            const double sx = request.scaleX > 0.0 ? request.scaleX : 1.0;
            const double sy = request.scaleY > 0.0 ? request.scaleY : 1.0;
            for (Point& corner : to) {
                corner[0] *= sx;
                corner[1] *= sy;
            }
        }

        const Matrix square = squareToQuad(from);
        const Matrix quad = squareToQuad(to);
        // The *backward* map: output to input. Forwards would be
        // quad * inverse(square), and this is its inverse written directly
        // rather than as a second matrix inversion of the product.
        Matrix back = multiply(square, inverse(quad));
        if (numberOf(request, "invert", 0.0) != 0.0) {
            back = multiply(quad, inverse(square));
        }
        back = normalised(back);
        if (trackSilent) {
            // Every pixel fetches from far outside the source, and outside
            // is transparent: a picture of nothing, made by the same kernel.
            back = {0.0, 0.0, -1.0e5, 0.0, 0.0, -1.0e5, 0.0, 0.0, 1.0};
            uniforms.blackOutside = 1u;
        }
        uniforms.h0 = float(back[0]);
        uniforms.h1 = float(back[1]);
        uniforms.h2 = float(back[2]);
        uniforms.h3 = float(back[3]);
        uniforms.h4 = float(back[4]);
        uniforms.h5 = float(back[5]);
        uniforms.h6 = float(back[6]);
        uniforms.h7 = float(back[7]);

        // A tracked mesh first: a GeometricTrack upstream writes, in the
        // first pixels of its `mesh` plane, where every vertex of its grid
        // went -- x, y, weight, valid, in the document's pixels -- and says
        // how many columns and rows in `mesh_dims`. Vertex (i, j) is where
        // the picture's own point (i/(cols-1), j/(rows-1)) must land, so the
        // knot's offset, in source pixels as the kernel wants it, is that
        // point minus where the plain homography would have fetched from.
        // Only the mesh's pixels are read back off the card, not the plane.
        const double crown = numberOf(request, "crown", 0.0);
        std::vector<float> knots;
        aofx::Buffer mesh;
        const std::vector<float>* dims =
            carrier != nullptr ? carrier->value("mesh_dims") : nullptr;
        if (dims != nullptr && dims->size() == 2 && (*dims)[0] >= 2.0F && (*dims)[1] >= 2.0F) {
            const uint32_t cols = uint32_t((*dims)[0]);
            const uint32_t rows = uint32_t((*dims)[1]);
            const aofx::InputPlane* tracked = nullptr;
            for (const aofx::InputPlane& candidate : request.inputs) {
                if (candidate.clip == "Track" && candidate.plane == "mesh" && candidate.buffer.isValid()) {
                    tracked = &candidate;
                    break;
                }
            }
            std::vector<float> verts(size_t(cols) * rows * 4, 0.0F);
            // The vertices sit in the plane's first pixels, from its
            // origin: a plane handed over cropped away from that origin
            // has them elsewhere, and the homography alone is the answer.
            const bool whole = tracked != nullptr && tracked->buffer.rect.x1 == 0 &&
                               tracked->buffer.rect.y1 == 0;
            if (whole && cols * rows <= 4096 &&
                request.gpu->read(tracked->buffer, verts.data(), verts.size() * sizeof(float))) {
                const double sx = request.scaleX > 0.0 ? request.scaleX : 1.0;
                const double sy = request.scaleY > 0.0 ? request.scaleY : 1.0;
                knots.assign(size_t(cols) * rows * 2, 0.0F);
                size_t placed = 0;
                for (uint32_t j = 0; j < rows; ++j) {
                    for (uint32_t i = 0; i < cols; ++i) {
                        const size_t v = size_t(j) * cols + i;
                        if (verts[v * 4 + 3] < 0.5F) {
                            continue;   // nobody knows: the homography's own point
                        }
                        const double px = verts[v * 4] * sx;
                        const double py = verts[v * 4 + 1] * sy;
                        const double w = back[6] * px + back[7] * py + 1.0;
                        if (std::abs(w) < 1e-9) {
                            continue;
                        }
                        const double bx = (back[0] * px + back[1] * py + back[2]) / w;
                        const double by = (back[3] * px + back[4] * py + back[5]) / w;
                        const double wantX = in.x1 + double(i) / (cols - 1) * (in.x2 - in.x1);
                        const double wantY = in.y1 + double(j) / (rows - 1) * (in.y2 - in.y1);
                        knots[v * 2] = float(wantX - bx);
                        knots[v * 2 + 1] = float(wantY - by);
                        ++placed;
                    }
                }
                uniforms.meshX = cols;
                uniforms.meshY = rows;
                knots.resize(((knots.size() + 3) / 4) * 4, 0.0F);
                const std::string key = "cornerpin.tracked." + request.instance;
                request.gpu->drop(key);
                mesh = request.gpu->keep(key, knots.data(), knots.size() * sizeof(float));
                request.gpu->publish(request.instance, "mesh_knots", double(placed));
            }
        }
        if (mesh.isValid()) {
            // The tracked mesh took the place of the crown.
        } else if (std::abs(crown) > 1e-6) {
            uniforms.meshX = 3;
            uniforms.meshY = 3;
            knots.assign(9 * 2, 0.0F);
            knots[(4 * 2) + 1] = float(-crown);
        } else {
            uniforms.meshX = 1;
            uniforms.meshY = 1;
            knots.assign(2, 0.0F);
        }
        if (!mesh.isValid()) {
            // Rounded up to a whole float4, which is what a buffer is made of.
            knots.resize(((knots.size() + 3) / 4) * 4, 0.0F);
            mesh = request.gpu->keep(crownKey(crown), knots.data(),
                                     knots.size() * sizeof(float));
        }
        if (!mesh.isValid()) {
            return false;
        }

        const aofx::KernelId warp = request.gpu->load("pinWarp");
        if (warp == aofx::kInvalidKernel) {
            return false;
        }
        // Motion blur, from the track's own measurement of how far the
        // corners moved since the last frame. A camera would have smeared
        // paint on the road by the shutter's share of that; a pin that
        // stays crisp while the road blurs is the one thing that says it
        // was never there. Off when nothing moved, and off by the knob.
        const double shutter = numberOf(request, "shutter", 0.5);
        const std::vector<float>* motion =
            numberOf(request, "motion_blur", 1.0) >= 0.5 && shutter > 0.0 && carrier != nullptr
                ? carrier->value("motion")
                : nullptr;
        if (motion != nullptr && motion->size() == 8) {
            const double sx = request.scaleX > 0.0 ? request.scaleX : 1.0;
            const double sy = request.scaleY > 0.0 ? request.scaleY : 1.0;
            double travel = 0.0;
            for (size_t i = 0; i < 4; ++i) {
                uniforms.m[i * 2] = static_cast<float>((*motion)[i * 2] * sx);
                uniforms.m[i * 2 + 1] = static_cast<float>((*motion)[i * 2 + 1] * sy);
                travel += std::hypot(uniforms.m[i * 2], uniforms.m[i * 2 + 1]);
            }
            uniforms.shutter = static_cast<float>(shutter);
            uniforms.blurOn = travel * shutter / 4.0 >= 0.75 ? 1u : 0u;
            request.gpu->publish(request.instance, "blur_px", travel * shutter / 4.0);
        }
        if (uniforms.blurOn == 0u) {
            return request.gpu->run(
                warp, aofx::Grid{uniforms.dstWidth, uniforms.dstHeight, 1},
                {source->buffer, mesh, target->buffer}, &uniforms, sizeof(uniforms));
        }
        const aofx::KernelId blur = request.gpu->load("pinBlur");
        const aofx::Buffer warped = request.gpu->scratch(target->buffer.stride, target->buffer.height);
        if (blur == aofx::kInvalidKernel || !warped.isValid()) {
            return false;
        }
        if (!request.gpu->run(warp, aofx::Grid{uniforms.dstWidth, uniforms.dstHeight, 1},
                              {source->buffer, mesh, warped}, &uniforms, sizeof(uniforms))) {
            return false;
        }
        return request.gpu->run(blur, aofx::Grid{uniforms.dstWidth, uniforms.dstHeight, 1},
                                {warped, mesh, target->buffer}, &uniforms, sizeof(uniforms));
    }

private:
    /// One kept buffer per crown value, rather than per node.
    ///
    /// A mesh is not per-node state: two pins with the same crown want the same
    /// nine numbers, and a value somebody is dragging wants a new buffer rather
    /// than a rewrite of one that a render in flight may still be reading. The
    /// key is the value, quantised to a hundredth of a pixel -- finer than
    /// anybody can see and coarse enough that a drag does not mint a thousand
    /// buffers.
    static std::string crownKey(double crown) {
        const long long ticks = std::llround(crown * 100.0);
        return "cornerpin.mesh." + std::to_string(ticks);
    }

protected:
    bool supersampled_ = false;
};

}   // namespace

class CornerPinSS final : public CornerPin {
public:
    CornerPinSS() : CornerPin(true) {}
};

AOFX_EXPORT_EFFECTS(CornerPin, CornerPinSS)
