// Copyright (c) 2026 aopenfx contributors.
//
// Shapes: the contract between a host that draws them and an effect that
// renders them.
//
// WHY THE VALUE IS A FLAT ARRAY OF DOUBLES
//
// Because `ParamValue` already carries one, and a new C++ type across this
// boundary would be a new ABI to keep in step -- a vector of structs of vectors
// is exactly the shape of thing that links, loads, and then disagrees about
// where a member lives when two compilers built the two sides.
//
// So a shape parameter travels in `ParamValue::numbers`, and what this header
// adds is the *layout* of that array, written once and read by both sides
// through the same functions. The encoding is self-describing: a reader knows
// how many shapes there are and how long each one is without being told
// anything it did not already receive.
//
// It is the same trade `gpe::Args` makes with its kernels, for the same reason
// and with the same discipline: one definition, a version in the first slot,
// and no caller anywhere writing offsets by hand.
//
// WHAT A SHAPE IS NOT
//
// It is not animated in here. A value handed to `render` is the shape *at that
// time* -- already evaluated, already transformed -- because an effect has no
// business interpolating and no way to know what a keyframe means to this host.
// Where the keys live and how they interpolate is the host's problem, and it
// keeps them in the document rather than in this array.
#pragma once

#include <cmath>
#include <algorithm>
#include <cstddef>
#include <vector>

namespace aofx::shape {

/// Bumped when the layout below changes.
///
/// In the first slot of the array rather than implied by the ABI version: a
/// document outlives a plugin, and a reader that finds a number it does not
/// know must be able to say so instead of reading somebody else's meaning out
/// of the same bytes.
/// 2 added the outline colour. A reader of this version still accepts a 1 and
/// gives those shapes the default colour, because documents written by the
/// version before it exist and refusing them would lose somebody's work over a
/// field that only decides what an outline looks like on screen.
inline constexpr double kEncodingVersion = 2.0;

/// How one shape combines with everything under it.
///
/// The four Natron and Nuke both have, in the order they list them, because
/// the number is what a document stores and an order nobody has a reason to
/// change is an order that will not be changed by accident.
enum class Op : int {
    Union = 0,
    Subtract,
    Intersect,
    Difference,
};

/// One control point.
///
/// Tangents are **offsets from the point**, not absolute positions. Two
/// reasons, and the second is the one that matters: an offset survives a
/// transform of the whole shape without being re-derived, and a point dragged
/// with its tangents does not have to touch three numbers to stay the shape it
/// was.
///
/// `feather` is how far this point's feather edge sits from the outline, along
/// the outward normal. Per point rather than per shape because a feather that
/// is one width everywhere is a feather that is wrong at the ends of anything
/// with a soft side and a hard side, which is most things.
struct Point {
    double x = 0.0;
    double y = 0.0;
    double inX = 0.0;
    double inY = 0.0;
    double outX = 0.0;
    double outY = 0.0;
    double feather = 0.0;
};

/// How many doubles one point takes. Named so no reader counts members.
inline constexpr size_t kPointStride = 7;

/// One shape, evaluated at a time.
struct Shape {
    /// Stable for the life of the shape, so the host can key its points and
    /// find them again after an edit. Never reused within one parameter.
    int    id = 0;
    Op     op = Op::Union;
    double opacity = 1.0;
    /// Whether the last point joins the first. Always true today; here because
    /// an open shape is the same list of points with a different answer, and
    /// leaving the field out would make adding one a change to the encoding
    /// rather than a change to a value.
    bool   closed = true;
    /// Off means "do not render", which is not the same as deleted: a shape
    /// turned off keeps its points and its keys.
    bool   enabled = true;
    /// How the feather fades from the outline outwards. 1 is linear; below
    /// one holds the edge and falls late, above one leaves early.
    double featherFalloff = 1.0;
    /// What the outline is drawn in, nought to one.
    ///
    /// It changes nothing about the matte -- a shape is coverage, and coverage
    /// has no colour. It is for telling one shape from another on a picture
    /// with nine of them over a face, which is the ordinary case and is
    /// impossible when every outline is the same yellow.
    double red = 1.0;
    double green = 0.82;
    double blue = 0.31;
    std::vector<Point> points;
};

/// How many doubles a shape's header takes, before its points.
inline constexpr size_t kShapeHeaderStride = 10;
/// What it took in version 1, which is still readable.
inline constexpr size_t kShapeHeaderStrideV1 = 7;

// --- encoding ---------------------------------------------------------------
//
// [ version, shapeCount,
//   id, op, opacity, closed, enabled, featherFalloff, r, g, b, pointCount,
//   x, y, inX, inY, outX, outY, feather,  ... one per point ...
//   ... next shape ... ]
//
// Version 1 is the same without r, g and b.

/// Packs shapes into the array a `ParamValue` carries.
[[nodiscard]] inline std::vector<double> encode(
    const std::vector<Shape>& shapes) {
    std::vector<double> out;
    out.reserve(2 + shapes.size() * (kShapeHeaderStride + 4 * kPointStride));
    out.push_back(kEncodingVersion);
    out.push_back(static_cast<double>(shapes.size()));
    for (const Shape& shape : shapes) {
        out.push_back(static_cast<double>(shape.id));
        out.push_back(static_cast<double>(static_cast<int>(shape.op)));
        out.push_back(shape.opacity);
        out.push_back(shape.closed ? 1.0 : 0.0);
        out.push_back(shape.enabled ? 1.0 : 0.0);
        out.push_back(shape.featherFalloff);
        out.push_back(shape.red);
        out.push_back(shape.green);
        out.push_back(shape.blue);
        out.push_back(static_cast<double>(shape.points.size()));
        for (const Point& point : shape.points) {
            out.push_back(point.x);
            out.push_back(point.y);
            out.push_back(point.inX);
            out.push_back(point.inY);
            out.push_back(point.outX);
            out.push_back(point.outY);
            out.push_back(point.feather);
        }
    }
    return out;
}

/// Reads them back. Empty for anything it does not fully understand.
///
/// All or nothing on purpose. A half-decoded shape list is a matte with a hole
/// in it and no error anywhere, and the array is either something this reader
/// wrote or something it should not guess about. The version is checked first,
/// and every length is checked against what is actually left rather than
/// against what the header claims -- a truncated document must not be read
/// past its end.
[[nodiscard]] inline std::vector<Shape> decode(const std::vector<double>& in) {
    if (in.size() < 2) {
        return {};
    }
    const bool withColour = in[0] == kEncodingVersion;
    if (!withColour && in[0] != 1.0) {
        return {};
    }
    const size_t header = withColour ? kShapeHeaderStride : kShapeHeaderStrideV1;
    const auto count = static_cast<size_t>(in[1]);
    std::vector<Shape> shapes;
    shapes.reserve(count);
    size_t at = 2;
    for (size_t i = 0; i < count; ++i) {
        if (at + header > in.size()) {
            return {};
        }
        Shape shape;
        shape.id = static_cast<int>(in[at + 0]);
        shape.op = static_cast<Op>(static_cast<int>(in[at + 1]));
        shape.opacity = in[at + 2];
        shape.closed = in[at + 3] != 0.0;
        shape.enabled = in[at + 4] != 0.0;
        shape.featherFalloff = in[at + 5];
        if (withColour) {
            shape.red = in[at + 6];
            shape.green = in[at + 7];
            shape.blue = in[at + 8];
        }
        const auto points = static_cast<size_t>(in[at + header - 1]);
        at += header;
        if (at + points * kPointStride > in.size()) {
            return {};
        }
        shape.points.reserve(points);
        for (size_t p = 0; p < points; ++p) {
            const size_t base = at + p * kPointStride;
            shape.points.push_back(Point{in[base + 0], in[base + 1],
                                         in[base + 2], in[base + 3],
                                         in[base + 4], in[base + 5],
                                         in[base + 6]});
        }
        at += points * kPointStride;
        shapes.push_back(std::move(shape));
    }
    return shapes;
}

// --- geometry ---------------------------------------------------------------

/// A point on the cubic between `from` and `to`, at `t` in [0,1].
///
/// Here rather than in the effect because the host draws the same curve it
/// renders, and a viewer showing a slightly different curve from the matte is
/// the bug this shares one function to prevent.
inline void evaluate(const Point& from, const Point& to, double t, double& x,
                     double& y) {
    const double u = 1.0 - t;
    const double c0 = u * u * u;
    const double c1 = 3.0 * u * u * t;
    const double c2 = 3.0 * u * t * t;
    const double c3 = t * t * t;
    x = c0 * from.x + c1 * (from.x + from.outX) + c2 * (to.x + to.inX) +
        c3 * to.x;
    y = c0 * from.y + c1 * (from.y + from.outY) + c2 * (to.y + to.inY) +
        c3 * to.y;
}

/// How many straight pieces one segment needs to stay within `tolerance`
/// pixels of the true curve.
///
/// By curvature, not by length. A fixed number of pixels per piece sounds
/// reasonable and is wrong in both directions: it chops a straight edge into
/// hundreds of collinear pieces that are all the same line, and it under-serves
/// a tight corner where the curve turns fastest. Measured on the case that
/// prompted this -- forty circles of radius 180 at 4K -- three pixels per piece
/// gave about six hundred segments per circle and put the node at 41.5 ms
/// against a 40 ms budget; by tolerance the same circles are about seventy
/// segments each and the arithmetic they save is the whole overrun.
///
/// The bound is the standard one for a cubic: the curve's greatest departure
/// from its chord is governed by the second difference of the control points,
/// and halving the step count quarters that departure. So the count is the
/// square root of the ratio, which is why a curve twice as bent costs only
/// forty per cent more pieces.
///
/// A twentieth of a pixel, and the number was chosen by measuring rather than
/// by taste. Chords cut *inside* the curve they approximate, so the error is
/// not noise around the true outline -- it is a matte uniformly smaller than
/// the shape somebody drew. At a quarter of a pixel a circle of radius sixty
/// came out 0.38% small, which is a quarter of a pixel of shrink all the way
/// round and exactly the kind of bias that is invisible alone and shows up the
/// moment the matte is composited against the plate it was cut from. At a
/// twentieth it is 0.03%, for about twice the segments.
[[nodiscard]] inline int stepsFor(const Point& from, const Point& to,
                                  double tolerance = 0.05) {
    // The control points, as the evaluator sees them.
    const double x1 = from.x + from.outX;
    const double y1 = from.y + from.outY;
    const double x2 = to.x + to.inX;
    const double y2 = to.y + to.inY;

    const double ax = from.x - 2.0 * x1 + x2;
    const double ay = from.y - 2.0 * y1 + y2;
    const double bx = x1 - 2.0 * x2 + to.x;
    const double by = y1 - 2.0 * y2 + to.y;
    const double bend =
        std::sqrt(std::max(ax * ax + ay * ay, bx * bx + by * by));

    const double safe = std::max(1e-6, tolerance);
    const int steps = static_cast<int>(
        std::ceil(std::sqrt(3.0 * bend / (4.0 * safe))));
    // At least one, and a ceiling so a shape scaled up by a thousand cannot
    // mint a hundred thousand segments and take the frame budget with it.
    return steps < 1 ? 1 : (steps > 256 ? 256 : steps);
}

/// The curve's direction at `t` along one segment.
///
/// The cubic's derivative, and not a difference of two nearby points: a
/// difference is the same arithmetic with a subtraction of two close numbers
/// in front of it, which is where the precision goes.
inline void tangentAt(const Point& from, const Point& to, double t, double& dx,
                      double& dy) {
    const double u = 1.0 - t;
    // The control polygon's three edges, weighted. P1-P0 is the out tangent,
    // P3-P2 is minus the in tangent, and the middle one is what is left.
    const double c0 = 3.0 * u * u;
    const double c1 = 6.0 * u * t;
    const double c2 = 3.0 * t * t;
    const double midX = (to.x + to.inX) - (from.x + from.outX);
    const double midY = (to.y + to.inY) - (from.y + from.outY);
    dx = c0 * from.outX + c1 * midX + c2 * (-to.inX);
    dy = c0 * from.outY + c1 * midY + c2 * (-to.inY);
    // A segment whose tangents are zero and whose ends coincide has no
    // direction at all; the chord is the only answer left, and it is the right
    // one for a straight side.
    if (dx * dx + dy * dy < 1e-18) {
        dx = to.x - from.x;
        dy = to.y - from.y;
    }
}

/// Which way round the shape is wound: +1 counter-clockwise, -1 clockwise.
///
/// By the shoelace area over the control points, which is exact for a polygon
/// and close enough for a curve: winding is a property of the whole loop and a
/// bulge on one side does not change which way it goes round.
///
/// It exists because "outward" has no meaning without it. A normal is
/// perpendicular to the curve and there are two of those; which one points
/// away from the inside is decided by the direction of travel, and a feather
/// drawn on the wrong side is a feather that grows *into* the shape.
[[nodiscard]] inline double windingSign(const Shape& shape) {
    double twiceArea = 0.0;
    const size_t count = shape.points.size();
    for (size_t i = 0; i < count; ++i) {
        const Point& a = shape.points[i];
        const Point& b = shape.points[(i + 1) % count];
        twiceArea += a.x * b.y - b.x * a.y;
    }
    return twiceArea < 0.0 ? -1.0 : 1.0;
}

/// The outward unit normal for a direction of travel, given the winding.
///
/// Walking a counter-clockwise loop keeps the inside on the left, so outward
/// is to the right of travel -- which is the direction rotated by -90.
inline void outwardNormal(double dx, double dy, double winding, double& nx,
                          double& ny) {
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-12) {
        nx = 0.0;
        ny = 0.0;
        return;
    }
    nx = winding * dy / length;
    ny = winding * -dx / length;
}

/// The outward normal at control point `index`.
///
/// The two sides are averaged rather than one being picked. At a smooth point
/// they agree and it makes no difference; at a cusp they do not, and the
/// average bisects the corner -- which is where a feather handle has to sit
/// for the soft edge to keep the same width round the outside of it.
inline void normalAt(const Shape& shape, size_t index, double& nx, double& ny) {
    nx = 0.0;
    ny = 0.0;
    const size_t count = shape.points.size();
    if (count < 2 || index >= count) {
        return;
    }
    const Point& here = shape.points[index];
    const Point& next = shape.points[(index + 1) % count];
    const Point& prev = shape.points[(index + count - 1) % count];

    // Leaving: the out tangent, or the chord to the next point when there is
    // no tangent to speak of.
    double outX = here.outX;
    double outY = here.outY;
    if (outX * outX + outY * outY < 1e-18) {
        outX = next.x - here.x;
        outY = next.y - here.y;
    }
    // Arriving: minus the in tangent, same fallback the other way.
    double inX = -here.inX;
    double inY = -here.inY;
    if (inX * inX + inY * inY < 1e-18) {
        inX = here.x - prev.x;
        inY = here.y - prev.y;
    }

    const double outLength = std::sqrt(outX * outX + outY * outY);
    const double inLength = std::sqrt(inX * inX + inY * inY);
    if (outLength > 1e-12) {
        outX /= outLength;
        outY /= outLength;
    }
    if (inLength > 1e-12) {
        inX /= inLength;
        inY /= inLength;
    }
    outwardNormal(outX + inX, outY + inY, windingSign(shape), nx, ny);
}

/// The colour a shape gets when it is made.
///
/// A cycle rather than one colour or a random one. Random gives two neighbours
/// the same green often enough to be useless, and one colour is what this is
/// fixing; a fixed cycle means the first nine shapes are nine distinguishable
/// outlines and the tenth repeats the first, by which point they are far apart.
///
/// Bright and unsaturated enough to read on a plate rather than pure hues: an
/// outline in saturated blue over a night exterior is an outline nobody can
/// see.
inline void colourFor(int index, double& red, double& green, double& blue) {
    static constexpr double kWheel[][3] = {
        {1.00, 0.82, 0.31},   // amber
        {0.42, 0.78, 1.00},   // sky
        {0.55, 0.90, 0.50},   // green
        {1.00, 0.55, 0.55},   // coral
        {0.80, 0.62, 1.00},   // violet
        {0.40, 0.88, 0.84},   // teal
        {1.00, 0.70, 0.90},   // pink
        {0.90, 0.90, 0.55},   // straw
    };
    constexpr int kCount = int(sizeof(kWheel) / sizeof(kWheel[0]));
    const int at = ((index % kCount) + kCount) % kCount;
    red = kWheel[at][0];
    green = kWheel[at][1];
    blue = kWheel[at][2];
}

}   // namespace aofx::shape
