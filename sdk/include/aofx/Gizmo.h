// Copyright (c) 2026 aopenfx contributors.
//
// Gizmos: any handle an effect wants in the viewer, declared rather than drawn.
//
// THE RULE THIS KEEPS
//
// The host draws every gizmo. A plugin cannot, and should not be able to: it
// would have to link the toolkit the application is written in, share its
// event loop and its coordinate systems, and one crash in a handle would take
// the window down with it. `ParamRole` already made that bargain for three
// cases -- a place, an angle, a scale -- and every effect that needed a fourth
// had nothing: a corner pin got four spin boxes, a crop got two loose points
// instead of a box, a blur's radius could not be dragged at all.
//
// So this header does not add a way to draw. It adds a *vocabulary*: a small
// set of primitives the host already knows how to draw and drag -- a point, a
// line, a box, a circle, a ring, a quad, a polygon, a frame the others can sit
// in -- and a way to say which numbers of which parameters each one is made
// of. Any gizmo an effect needs is a composition of those, the same way any
// picture is a composition of pixels. The effect says what; the host decides
// how it looks, how it is hit, how it snaps, how it is undone.
//
// And for what cannot be edited -- the path a tracker followed, the grid a
// lens solve found, the outline of what a network segmented -- a `Drawing`
// reads strokes the effect *computed*, attached to its output, and the host
// draws them without any way to drag them. Read-only is the price of arbitrary:
// a stroke the effect computed has no parameter to write a drag back into.
//
// WHAT CROSSES THE BOUNDARY
//
// `EffectDesc::gizmos`, a vector of the plain structs below, filled once in
// `describe` like every other part of the descriptor. And, for a `Drawing`, a
// flat array of floats under an attachment id, in the layout `encodeDrawing`
// writes -- the same trade `aofx/Shape.h` makes, for the same reason: no new
// C++ type crosses at render time, only an agreement about the contents of an
// array, version-stamped in its first slot.
//
// The full standard, with what a host must do for each primitive, is in
// `docs/gizmos.md`.
#pragma once

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "aofx/Types.h"

namespace aofx {

/// What a gizmo is. Appended, never reordered: the value travels as an int,
/// and a host that meets a number it does not know skips that gizmo (and says
/// so) rather than drawing something else.
///
/// Each kind is a list of *slots* -- the numbers it is made of -- listed by
/// `gizmoSlots`. The comment on each kind names them; `(2)` is a point in the
/// gizmo's space, `(1)` a single number.
enum class GizmoKind : int {
    /// `at(2)`. A handle that drags freely, or along `constraint`.
    Point = 0,
    /// `from(2)`, `to(2)`. A segment with a handle at each end; dragging the
    /// middle moves both.
    Line,
    /// `from(2)`, `to(2)`. A Line with a head at `to`: a direction, a light,
    /// a motion vector somebody sets by hand.
    Arrow,
    /// `corner1(2)`, `corner2(2)`, axis-aligned, in either order. Four corner
    /// handles, four edge handles, and the inside to move it.
    Box,
    /// `centre(2)`, `radius(1)`. A handle at the centre and one on the rim.
    Circle,
    /// `centre(2)`, `radii(2)`, `angle(1)` optional. A rim handle per axis,
    /// and a rotation handle when `angle` is bound.
    Ellipse,
    /// `centre(2)`, `angle(1)` in degrees, `radius(1)` optional. A ring with
    /// a handle on it; unbound, the ring is a fixed size on screen.
    Angle,
    /// `corner1(2)` .. `corner4(2)`, anticlockwise from the bottom left. A
    /// corner pin: four handles and the edges between them.
    Quad,
    /// `points(2n)`, open. Each binding of `points` adds its pairs, in order.
    Polyline,
    /// `points(2n)`, closed. As Polyline, with the last point joined to the
    /// first and the inside to move all of them.
    Polygon,
    /// `origin(2)`, `length(1)`, `angle(1)` optional. A single number dragged
    /// as a distance from a place: a blur's radius, a feather, a glow's
    /// reach. `angle` (degrees, anticlockwise from +x) says which way the
    /// handle points; unbound it points along +x. The origin is not dragged
    /// by this gizmo -- bind it to a Point as well if it should be.
    Distance,
    /// `translate(2)`, `angle(1)`, `scale(2)`, `centre(2)`; all but
    /// `translate` optional. A coordinate frame: drawn as the transform
    /// handle (a centre, a ring and scale handles), and the space every gizmo
    /// that names it as `parent` is placed in. The matrix is
    /// `T(translate) * T(centre) * R(angle) * S(scale) * T(-centre)`, which is
    /// `aofx/Transform.h`'s SRT with a pivot and no skew.
    Frame,
    /// `at(2)`. Text at a place, from `GizmoDesc::text`. Never dragged.
    Label,
    /// `at(2)`. A cross at a place. Never dragged: for a place the effect
    /// found rather than one somebody set.
    Crosshair,
    /// `strokes(n)`: an attachment in the layout `encodeDrawing` writes, and
    /// nothing else. Whatever the effect computed, drawn and never dragged.
    Drawing,
};

/// The number of kinds, for a host checking an int it read from a bundle.
inline constexpr int kGizmoKindCount = static_cast<int>(GizmoKind::Drawing) + 1;

/// Where a gizmo's numbers are measured.
enum class GizmoSpace : int {
    /// Full-resolution pixels of the project, y up, (0,0) at the bottom left:
    /// what `ParamRole::Position` means, and what almost every effect wants.
    Canonical = 0,
    /// Fractions of the project's format, 0..1 on each axis.
    Project,
    /// Fractions of an input's region of definition: `GizmoDesc::clip`, or
    /// the pass-through input when that is empty. For a gizmo that belongs to
    /// the picture rather than to the frame -- a crop of a still that is not
    /// the project's size.
    Input,
    /// The frame of the `Frame` gizmo named by `GizmoDesc::parent`. A handle
    /// that turns with a transform is one of these.
    Parent,
};

/// Which way a point handle may move. The host's own modifiers (a key held
/// for "horizontal only") still apply on top.
enum class GizmoConstraint : int {
    Free = 0,
    Horizontal,
    Vertical,
    /// Drawn and never dragged, although the numbers it shows are editable
    /// elsewhere -- the panel, a second gizmo.
    Locked,
};

/// When the viewer shows a gizmo.
enum class GizmoVisibility : int {
    /// While the node is selected, or its panel is open. What a handle is for.
    WhenSelected = 0,
    /// Whenever the node's output is in the viewer. For a guide somebody
    /// works against: a safe area, a horizon, the region being tracked.
    Always,
    /// Only while one of its parameters is being edited, in the panel or by
    /// dragging. For a radius that is clutter until somebody changes it.
    WhileEditing,
};

/// How a gizmo looks. Suggestions: a host may draw a selected gizmo brighter
/// or thicker, and draws a hovered handle differently whatever this says.
struct GizmoStyle {
    /// Nought to one, straight (not premultiplied). The default is the shape
    /// outline's yellow, so a gizmo and a roto look like the same family.
    double red = 1.0;
    double green = 0.82;
    double blue = 0.31;
    double alpha = 1.0;
    /// Width of lines, in screen points -- never picture pixels, or a gizmo
    /// on a zoomed-out 8K frame would vanish.
    double width = 1.0;

    enum class Line : int { Solid = 0, Dashed, Dotted };
    Line line = Line::Solid;

    enum class Handle : int { Square = 0, Round, Diamond, Cross, None };
    Handle handle = Handle::Square;

    /// For a closed kind (Box, Circle, Ellipse, Quad, Polygon): tint the
    /// inside faintly. Off by default, because a gizmo over the picture is
    /// there to be seen past.
    bool fill = false;
};

/// Where one slot of a gizmo gets its numbers.
///
/// Exactly one source: a parameter (and the gizmo edits it), an attachment
/// the effect produced (and the gizmo shows it, read-only), or `constant`.
struct GizmoBinding {
    /// Which slot of the kind this fills, by the name `gizmoSlots` gives.
    std::string slot;

    /// A numeric parameter of this effect (Double or Integer), by name. The
    /// slot takes its width in components, starting at `component`.
    ///
    /// With `GizmoDesc::repeat` set, `{i}` in the name is the item's number,
    /// counting from one: `corner{i}` is `corner1`, `corner2`, ...
    std::string param;
    int         component = 0;

    /// A value the effect `attach`ed to its output, by id. Read-only by
    /// construction: nothing writes an attachment back. The slot takes its
    /// width starting at `component`, as for a parameter. `{i}` works here
    /// too.
    std::string attachment;

    /// The numbers themselves, when neither of the above is set. Also what a
    /// parameter or attachment slot shows when its source is absent -- an
    /// attachment only exists once a render has produced it.
    std::vector<double> constant;

    /// What is drawn is `value * scale + offset`, component by component; a
    /// drag writes `(drawn - offset) / scale` back. For a parameter measured
    /// differently from its handle: a size that is a diameter drawn as a
    /// radius (`scale = 0.5`), a softness in percent of a box.
    double scale = 1.0;
    double offset = 0.0;

    /// Draw this slot but never write it, although its source is a parameter.
    bool readOnly = false;
};

/// A slot filled from a parameter: the ordinary case, and the one a drag
/// writes back into.
[[nodiscard]] inline GizmoBinding bindParam(std::string slot, std::string param,
                                            int component = 0) {
    GizmoBinding binding;
    binding.slot = std::move(slot);
    binding.param = std::move(param);
    binding.component = component;
    return binding;
}

/// A slot filled from what the effect attached to its output, shown and never
/// dragged. `fallback` is what is drawn before any render has produced it;
/// empty draws nothing until one has.
[[nodiscard]] inline GizmoBinding bindAttachment(std::string slot, std::string id,
                                                 std::vector<double> fallback = {}) {
    GizmoBinding binding;
    binding.slot = std::move(slot);
    binding.attachment = std::move(id);
    binding.constant = std::move(fallback);
    return binding;
}

/// A slot that is always the same numbers: a guide, a fixed pivot.
[[nodiscard]] inline GizmoBinding bindConstant(std::string slot,
                                               std::vector<double> values) {
    GizmoBinding binding;
    binding.slot = std::move(slot);
    binding.constant = std::move(values);
    return binding;
}

/// One gizmo.
struct GizmoDesc {
    /// Unique within the effect. What `parent` names, and what a host keys
    /// its hover and selection state on.
    std::string id;
    /// What a person reads: the tooltip on a handle, and the undo step a
    /// drag makes ("Move Corner 1"). Empty uses `id`.
    std::string label;

    GizmoKind kind = GizmoKind::Point;
    std::vector<GizmoBinding> bindings;

    GizmoSpace space = GizmoSpace::Canonical;
    /// For `GizmoSpace::Parent`: the id of a `Frame` gizmo of this effect.
    std::string parent;
    /// For `GizmoSpace::Input`: the clip, by name. Empty is the pass-through.
    std::string clip;

    /// For a Label. `{name}` is replaced by the parameter `name`'s value, as
    /// the panel would show it, so a label can say what a handle is set to.
    std::string text;

    GizmoConstraint constraint = GizmoConstraint::Free;
    GizmoStyle      style;
    GizmoVisibility visibility = GizmoVisibility::WhenSelected;

    /// The same rule as a row's, read the same way. A gizmo whose parameters
    /// are hidden is hidden too, whatever this says -- a handle for a control
    /// the panel is not showing is a handle for something the node ignores.
    ShownWhen shownWhen;
    ShownWhen shownAlso;

    /// An Integer parameter with `ParamRole::ItemCount`, or empty. Set, this
    /// gizmo is drawn once per item in use, with `{i}` in its bindings' names
    /// (and in `text`) standing for the item's number from one. The item the
    /// pool's `ItemIndex` names is drawn as selected.
    std::string repeat;
};

/// One slot of a kind.
struct GizmoSlot {
    const char* name = "";
    /// How many numbers one binding of it takes. Zero means "any number of
    /// points": the slot may be bound more than once, and each binding adds
    /// an even count -- a parameter's components from `component` on, an
    /// attachment's whole array, or the constant.
    int  width = 0;
    bool optional = false;
};

/// The slots of a kind, in the order the comments on `GizmoKind` list them.
/// Empty for a number that is not a kind.
[[nodiscard]] inline std::vector<GizmoSlot> gizmoSlots(GizmoKind kind) {
    switch (kind) {
        case GizmoKind::Point:
        case GizmoKind::Label:
        case GizmoKind::Crosshair:
            return {{"at", 2, false}};
        case GizmoKind::Line:
        case GizmoKind::Arrow:
            return {{"from", 2, false}, {"to", 2, false}};
        case GizmoKind::Box:
            return {{"corner1", 2, false}, {"corner2", 2, false}};
        case GizmoKind::Circle:
            return {{"centre", 2, false}, {"radius", 1, false}};
        case GizmoKind::Ellipse:
            return {{"centre", 2, false}, {"radii", 2, false}, {"angle", 1, true}};
        case GizmoKind::Angle:
            return {{"centre", 2, false}, {"angle", 1, false}, {"radius", 1, true}};
        case GizmoKind::Quad:
            return {{"corner1", 2, false}, {"corner2", 2, false},
                    {"corner3", 2, false}, {"corner4", 2, false}};
        case GizmoKind::Polyline:
        case GizmoKind::Polygon:
            return {{"points", 0, false}};
        case GizmoKind::Distance:
            return {{"origin", 2, false}, {"length", 1, false}, {"angle", 1, true}};
        case GizmoKind::Frame:
            return {{"translate", 2, false}, {"angle", 1, true},
                    {"scale", 2, true}, {"centre", 2, true}};
        case GizmoKind::Drawing:
            return {{"strokes", 0, false}};
    }
    return {};
}

/// Whether a kind can be dragged at all. The others only show.
[[nodiscard]] constexpr bool gizmoIsEditable(GizmoKind kind) noexcept {
    return kind != GizmoKind::Label && kind != GizmoKind::Crosshair &&
           kind != GizmoKind::Drawing;
}

// --- computed drawings --------------------------------------------------------
//
// What a `Drawing` gizmo reads: strokes an effect worked out while rendering,
// attached to its output with `RenderRequest::attach`. The host draws them in
// the gizmo's space and style, recolouring each stroke where it carries a
// colour of its own.
//
// [ version, strokeCount,
//   flags, r, g, b, a, pointCount, x, y, x, y, ...   one per stroke
//   ... ]
//
// `flags` is a sum of the `kStroke*` bits. Floats, because that is what an
// attachment carries; a stroke is for looking at, and float is more than a
// screen can show.

namespace gizmo {

/// Bumped when the layout below changes. In the first slot, for the reason
/// `shape::kEncodingVersion` is: a reader must be able to refuse a layout it
/// does not know instead of reading somebody else's meaning out of it.
inline constexpr float kDrawingVersion = 1.0F;

/// The last point joins the first.
inline constexpr int kStrokeClosed = 1;
/// Draw a dot at each point instead of lines between them.
inline constexpr int kStrokePoints = 2;
/// Dashed, whatever the gizmo's style says.
inline constexpr int kStrokeDashed = 4;
/// Use the stroke's own r, g, b, a instead of the gizmo's colour.
inline constexpr int kStrokeColoured = 8;

/// How many floats a stroke takes before its points.
inline constexpr size_t kStrokeHeaderStride = 6;

struct Stroke {
    int   flags = 0;
    float red = 1.0F;
    float green = 1.0F;
    float blue = 1.0F;
    float alpha = 1.0F;
    /// x, y pairs, in the gizmo's space.
    std::vector<float> points;
};

/// Hard ceilings, so a reader never allocates what a corrupt array claims.
/// A drawing is for a person to look at; past these it is not a drawing.
inline constexpr size_t kMaxStrokes = 4096;
inline constexpr size_t kMaxStrokePoints = 65536;

/// Packs strokes into the array an attachment carries.
[[nodiscard]] inline std::vector<float> encodeDrawing(
    const std::vector<Stroke>& strokes) {
    std::vector<float> out;
    out.push_back(kDrawingVersion);
    out.push_back(static_cast<float>(strokes.size()));
    for (const Stroke& stroke : strokes) {
        out.push_back(static_cast<float>(stroke.flags));
        out.push_back(stroke.red);
        out.push_back(stroke.green);
        out.push_back(stroke.blue);
        out.push_back(stroke.alpha);
        out.push_back(static_cast<float>(stroke.points.size() / 2));
        out.insert(out.end(), stroke.points.begin(),
                   stroke.points.begin() +
                       static_cast<std::ptrdiff_t>(stroke.points.size() / 2 * 2));
    }
    return out;
}

/// Reads what `encodeDrawing` wrote. False, and `into` empty, for anything
/// that is not exactly that: a version this does not know, a count that runs
/// past the end, a number that is not finite. A host shows nothing rather
/// than half a drawing.
[[nodiscard]] inline bool decodeDrawing(const std::vector<float>& data,
                                        std::vector<Stroke>& into) {
    into.clear();
    const auto fail = [&into] {
        into.clear();
        return false;
    };
    const auto wholeCount = [](float value, size_t ceiling, size_t& out) {
        if (!std::isfinite(value) || value < 0.0F ||
            value != std::floor(value) || value > static_cast<float>(ceiling)) {
            return false;
        }
        out = static_cast<size_t>(value);
        return true;
    };
    if (data.size() < 2 || data[0] != kDrawingVersion) {
        return fail();
    }
    size_t count = 0;
    if (!wholeCount(data[1], kMaxStrokes, count)) {
        return fail();
    }
    size_t at = 2;
    for (size_t s = 0; s < count; ++s) {
        if (data.size() - at < kStrokeHeaderStride) {
            return fail();
        }
        Stroke stroke;
        size_t flags = 0;
        size_t points = 0;
        if (!wholeCount(data[at], 15, flags) ||
            !wholeCount(data[at + 5], kMaxStrokePoints, points)) {
            return fail();
        }
        stroke.flags = static_cast<int>(flags);
        stroke.red = data[at + 1];
        stroke.green = data[at + 2];
        stroke.blue = data[at + 3];
        stroke.alpha = data[at + 4];
        at += kStrokeHeaderStride;
        if (data.size() - at < points * 2) {
            return fail();
        }
        stroke.points.assign(data.begin() + static_cast<std::ptrdiff_t>(at),
                             data.begin() + static_cast<std::ptrdiff_t>(at + points * 2));
        for (float value : stroke.points) {
            if (!std::isfinite(value)) {
                return fail();
            }
        }
        at += points * 2;
        into.push_back(std::move(stroke));
    }
    if (at != data.size()) {
        return fail();
    }
    return true;
}

}   // namespace gizmo

}   // namespace aofx
