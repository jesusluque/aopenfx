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
// The same vocabulary works in three dimensions. A gizmo in `World` space, or
// placed in a `Frame3D`, has points of three numbers instead of two; the host
// draws it in its 3D view, and over the picture through the `Camera` gizmo it
// names -- so a tracked camera's ground plane, a light's position or a card's
// corners sit on the shot where they belong. A handle dragged over the
// picture moves on the plane that faces that camera, or along an axis.
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

#include "aofx/Transform.h"
#include "aofx/Types.h"

namespace aofx {

/// What a gizmo is. Appended, never reordered: the value travels as an int,
/// and a host that meets a number it does not know skips that gizmo (and says
/// so) rather than drawing something else.
///
/// Each kind is a list of *slots* -- the numbers it is made of -- listed by
/// `gizmoSlots`. The comment on each kind names them; `(p)` is a point in the
/// gizmo's space -- two numbers in a 2D space, three in a 3D one -- and `(1)`,
/// `(3)`, `(12)` that many numbers whatever the space.
///
/// Most kinds work in both. Circle, Ellipse, Angle, Distance and Frame are
/// flat by nature and only 2D; Sphere, Frame3D and Camera only 3D.
/// `gizmoWorksIn` says which.
enum class GizmoKind : int {
    /// `at(p)`. A handle that drags freely, or along `constraint`.
    Point = 0,
    /// `from(p)`, `to(p)`. A segment with a handle at each end; dragging the
    /// middle moves both.
    Line,
    /// `from(p)`, `to(p)`. A Line with a head at `to`: a direction, a light,
    /// a motion vector somebody sets by hand.
    Arrow,
    /// `corner1(p)`, `corner2(p)`, axis-aligned, in either order. Four corner
    /// handles, four edge handles, and the inside to move it; in 3D a cuboid
    /// with a handle on each face.
    Box,
    /// `centre(p)`, `radius(1)`. 2D. A handle at the centre and one on the rim.
    Circle,
    /// `centre(p)`, `radii(2)`, `angle(1)` optional. 2D. A rim handle per axis,
    /// and a rotation handle when `angle` is bound.
    Ellipse,
    /// `centre(p)`, `angle(1)` in degrees, `radius(1)` optional. 2D. A ring with
    /// a handle on it; unbound, the ring is a fixed size on screen.
    Angle,
    /// `corner1(p)` .. `corner4(p)`, anticlockwise from the bottom left. A
    /// corner pin: four handles and the edges between them. In 3D, a card.
    Quad,
    /// `points(p·n)`, open. Each binding of `points` adds its pairs, in order.
    Polyline,
    /// `points(p·n)`, closed. As Polyline, with the last point joined to the
    /// first and the inside to move all of them.
    Polygon,
    /// `origin(p)`, `length(1)`, `angle(1)` optional. 2D. A single number dragged
    /// as a distance from a place: a blur's radius, a feather, a glow's
    /// reach. `angle` (degrees, anticlockwise from +x) says which way the
    /// handle points; unbound it points along +x. The origin is not dragged
    /// by this gizmo -- bind it to a Point as well if it should be.
    Distance,
    /// `translate(2)`, `angle(1)`, `scale(2)`, `centre(2)`; all but
    /// `translate` optional. 2D. A coordinate frame: drawn as the transform
    /// handle (a centre, a ring and scale handles), and the space every gizmo
    /// that names it as `parent` is placed in. The matrix is
    /// `T(translate) * T(centre) * R(angle) * S(scale) * T(-centre)`, which is
    /// `aofx/Transform.h`'s SRT with a pivot and no skew.
    Frame,
    /// `at(p)`. Text at a place, from `GizmoDesc::text`. Never dragged.
    Label,
    /// `at(p)`. A cross at a place. Never dragged: for a place the effect
    /// found rather than one somebody set.
    Crosshair,
    /// `strokes(n)`: an attachment in the layout `encodeDrawing` writes, and
    /// nothing else. Whatever the effect computed, drawn and never dragged.
    Drawing,
    /// `centre(p)`, `radius(1)`. 3D. A handle at the centre and one on the
    /// silhouette: a light's reach, a region of influence, a soft selection.
    Sphere,
    /// `translate(3)`, `rotate(3)`, `scale(3)`, `pivot(3)`, all optional, or
    /// `matrix(12)` instead of all four. 3D. A coordinate frame: drawn as the
    /// host's 3D transform handle (axes, rotation rings, scale handles), and
    /// the space of every gizmo that names it as `parent`. The matrix is
    /// `xform::localMatrix` with `GizmoDesc::rotationOrder` and SRT, which is
    /// what an Axis means; `matrix` is its upper 3x4, row-major, for a frame
    /// the effect already has as a matrix -- read-only, because twelve numbers
    /// do not go back into three angles unambiguously.
    Frame3D,
    /// `translate(3)`, `rotate(3)` or `matrix(12)` as for Frame3D, `focal(1)`
    /// in millimetres, `aperture(1)` (horizontal, millimetres; 24.576
    /// unbound) and `offset(2)` (the window translate, in half-widths; 0
    /// unbound). 3D. Drawn as a frustum in the 3D view, dragged as a Frame3D,
    /// and what every 3D gizmo naming it in `GizmoDesc::camera` is projected
    /// onto the picture through. `gizmo::projectToPicture` is the projection.
    Camera,
};

/// The number of kinds, for a host checking an int it read from a bundle.
inline constexpr int kGizmoKindCount = static_cast<int>(GizmoKind::Camera) + 1;

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
    /// The frame of the `Frame` or `Frame3D` gizmo named by
    /// `GizmoDesc::parent`, and as many dimensions as it has. A handle that
    /// turns with a transform is one of these.
    Parent,
    /// The 3D scene's units: right-handed, Y up, as `aofx/Transform.h` and
    /// the host's 3D view. Points have three numbers.
    World,
};

/// Which way a point handle may move. The host's own modifiers (a key held
/// for "horizontal only") still apply on top.
///
/// `Free` in 3D means: in the host's 3D view, its own translate handle (an
/// arrow per axis, a square per plane); over the picture, on the plane through
/// the point that faces the camera, which is the only plane a 2D drag can
/// name without guessing a depth.
enum class GizmoConstraint : int {
    Free = 0,
    Horizontal,
    Vertical,
    /// Drawn and never dragged, although the numbers it shows are editable
    /// elsewhere -- the panel, a second gizmo.
    Locked,
    /// 3D only, from here on. Along the space's Z axis. In 3D, `Horizontal`
    /// and `Vertical` are its X and Y axes.
    AlongZ,
    /// On one of the space's planes: `OnXZ` is the ground.
    OnXY,
    OnXZ,
    OnYZ,
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

    /// For a closed kind (Box, Circle, Ellipse, Quad, Polygon, Sphere): tint the
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
    /// Look the attachment up on what arrives at this input, by clip name,
    /// instead of on this node's output: the corners a tracker upstream
    /// found, the camera a solve upstream produced. Empty is the output.
    std::string clip;

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

/// A slot filled from what arrives at input `clip`: numbers a node upstream
/// attached, shown and never dragged.
[[nodiscard]] inline GizmoBinding bindInput(std::string slot, std::string clip,
                                            std::string id, int component = 0,
                                            std::vector<double> fallback = {}) {
    GizmoBinding binding = bindAttachment(std::move(slot), std::move(id),
                                          std::move(fallback));
    binding.clip = std::move(clip);
    binding.component = component;
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
    /// For `GizmoSpace::Parent`: the id of a `Frame` or `Frame3D` gizmo of
    /// this effect. Its kind decides whether this gizmo is 2D or 3D.
    std::string parent;
    /// For a 3D gizmo: the id of the `Camera` gizmo that projects it onto the
    /// picture. Empty takes the nearest parent's; with none anywhere up the
    /// chain it is drawn only in the host's 3D view.
    std::string camera;
    /// For a Frame3D or Camera: the order its `rotate` angles are applied in,
    /// spelled as `aofx/Transform.h` spells it. Nuke's default.
    xform::RotationOrder rotationOrder = xform::RotationOrder::ZXY;
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
    /// How many numbers one binding of it takes, when `point` is false. Zero
    /// means "the whole array": a Drawing's strokes.
    int  width = 0;
    bool optional = false;
    /// A place in the gizmo's space: two numbers in 2D, three in 3D, and
    /// `width` is ignored. With `list` it is any number of places: the slot
    /// may be bound more than once, and each binding adds a whole number of
    /// them -- a parameter's components from `component` on, an attachment's
    /// array from `component` on, or the constant.
    bool point = false;
    bool list = false;
};

/// How many numbers one binding of `slot` takes in a space of `dimensions`,
/// or zero for "a whole number of places" (a list) or "the whole array".
[[nodiscard]] constexpr int gizmoSlotWidth(const GizmoSlot& slot, int dimensions) noexcept {
    return slot.list ? 0 : slot.point ? dimensions : slot.width;
}

/// The slots of a kind, in the order the comments on `GizmoKind` list them.
/// Empty for a number that is not a kind.
[[nodiscard]] inline std::vector<GizmoSlot> gizmoSlots(GizmoKind kind) {
    const auto at = [](const char* name, bool optional = false) {
        return GizmoSlot{name, 0, optional, true, false};
    };
    const auto numbers = [](const char* name, int width, bool optional = false) {
        return GizmoSlot{name, width, optional, false, false};
    };
    switch (kind) {
        case GizmoKind::Point:
        case GizmoKind::Label:
        case GizmoKind::Crosshair:
            return {at("at")};
        case GizmoKind::Line:
        case GizmoKind::Arrow:
            return {at("from"), at("to")};
        case GizmoKind::Box:
            return {at("corner1"), at("corner2")};
        case GizmoKind::Circle:
        case GizmoKind::Sphere:
            return {at("centre"), numbers("radius", 1)};
        case GizmoKind::Ellipse:
            return {at("centre"), numbers("radii", 2), numbers("angle", 1, true)};
        case GizmoKind::Angle:
            return {at("centre"), numbers("angle", 1), numbers("radius", 1, true)};
        case GizmoKind::Quad:
            return {at("corner1"), at("corner2"), at("corner3"), at("corner4")};
        case GizmoKind::Polyline:
        case GizmoKind::Polygon:
            return {GizmoSlot{"points", 0, false, true, true}};
        case GizmoKind::Distance:
            return {at("origin"), numbers("length", 1), numbers("angle", 1, true)};
        case GizmoKind::Frame:
            return {numbers("translate", 2), numbers("angle", 1, true),
                    numbers("scale", 2, true), numbers("centre", 2, true)};
        case GizmoKind::Drawing:
            return {numbers("strokes", 0)};
        case GizmoKind::Frame3D:
            return {numbers("translate", 3, true), numbers("rotate", 3, true),
                    numbers("scale", 3, true), numbers("pivot", 3, true),
                    numbers("matrix", 12, true)};
        case GizmoKind::Camera:
            return {numbers("translate", 3, true), numbers("rotate", 3, true),
                    numbers("matrix", 12, true), numbers("focal", 1),
                    numbers("aperture", 1, true), numbers("offset", 2, true)};
    }
    return {};
}

/// Whether a kind can be drawn in a space of `dimensions` (2 or 3).
[[nodiscard]] constexpr bool gizmoWorksIn(GizmoKind kind, int dimensions) noexcept {
    switch (kind) {
        case GizmoKind::Circle:
        case GizmoKind::Ellipse:
        case GizmoKind::Angle:
        case GizmoKind::Distance:
        case GizmoKind::Frame:
            return dimensions == 2;
        case GizmoKind::Sphere:
        case GizmoKind::Frame3D:
        case GizmoKind::Camera:
            return dimensions == 3;
        default:
            return dimensions == 2 || dimensions == 3;
    }
}

/// How many numbers a place has in `gizmo`'s space: 3 in `World` or in a
/// Frame3D, 2 otherwise. Zero for a parent that is missing or is not a frame,
/// or a chain of parents that loops.
[[nodiscard]] inline int gizmoDimensions(const std::vector<GizmoDesc>& all,
                                         const GizmoDesc& gizmo) {
    if (gizmo.space == GizmoSpace::World) {
        return 3;
    }
    if (gizmo.space != GizmoSpace::Parent) {
        return 2;
    }
    // The first parent decides; the rest of the chain must still reach a
    // frame that is not itself placed in another, or nothing is placed at all.
    int dimensions = 0;
    const GizmoDesc* at = &gizmo;
    for (size_t steps = 0; steps <= all.size(); ++steps) {
        const GizmoDesc* up = nullptr;
        for (const GizmoDesc& candidate : all) {
            if (candidate.id == at->parent) {
                up = &candidate;
            }
        }
        if (up == nullptr ||
            (up->kind != GizmoKind::Frame && up->kind != GizmoKind::Frame3D)) {
            return 0;
        }
        if (dimensions == 0) {
            dimensions = up->kind == GizmoKind::Frame3D ? 3 : 2;
        }
        if (up->space != GizmoSpace::Parent) {
            return dimensions;
        }
        at = up;
    }
    return 0;
}

/// Whether a kind can be dragged at all. The others only show.
[[nodiscard]] constexpr bool gizmoIsEditable(GizmoKind kind) noexcept {
    return kind != GizmoKind::Label && kind != GizmoKind::Crosshair &&
           kind != GizmoKind::Drawing;
}

// --- 3D ---------------------------------------------------------------------
//
// The arithmetic a host and an effect must agree on, written once: how a
// Frame3D's numbers become a matrix, and how a camera puts a point of the
// scene on the picture. An effect that renders through the same camera calls
// the same function, so a handle and the pixels under it cannot disagree.

namespace gizmo {

/// A Frame3D's local matrix from its slots: `xform::localMatrix` with SRT.
[[nodiscard]] inline xform::Mat4 frame3DMatrix(const xform::Vec3& translate,
                                               const xform::Vec3& rotate,
                                               const xform::Vec3& scale,
                                               const xform::Vec3& pivot,
                                               xform::RotationOrder order) {
    xform::Transform t;
    t.translate = translate;
    t.rotate = rotate;
    t.scale = scale;
    t.pivot = pivot;
    t.rotationOrder = order;
    t.transformOrder = xform::TransformOrder::SRT;
    return xform::localMatrix(t);
}

/// A matrix from a `matrix(12)` slot: the upper 3x4, row-major.
[[nodiscard]] inline xform::Mat4 matrixFromRows(const double* rows) {
    xform::Mat4 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.at(r, c) = rows[r * 4 + c];
        }
    }
    return out;
}

/// A 2D Frame's matrix, in the plane z = 0:
/// `T(translate) * T(centre) * R(angle) * S(scale) * T(-centre)`.
[[nodiscard]] inline xform::Mat4 frameMatrix(double tx, double ty, double angle,
                                             double sx, double sy, double cx,
                                             double cy) {
    return frame3DMatrix({tx, ty, 0.0}, {0.0, 0.0, angle}, {sx, sy, 1.0},
                         {cx, cy, 0.0}, xform::RotationOrder::ZXY);
}

/// Unbound, a camera's horizontal aperture: Nuke's default, in millimetres.
inline constexpr double kDefaultAperture = 24.576;

/// What a Camera gizmo's slots amount to.
struct CameraView {
    /// Camera to world. The camera looks down its own -Z, Y up.
    xform::Mat4 cameraToWorld;
    double focal = 50.0;                   ///< millimetres
    double aperture = kDefaultAperture;    ///< horizontal, millimetres
    /// The window translate, in half-widths of the picture on both axes.
    double offsetX = 0.0;
    double offsetY = 0.0;
};

/// Where `world` lands on the picture, in canonical pixels of a project
/// `width` by `height` with `pixelAspect`. False when the point is behind the
/// camera, or on its plane, and there is nowhere to put it.
///
/// The horizontal aperture spans the width; the vertical follows from the
/// format, as Nuke's camera does, so the aperture's own height never enters:
///
///     x = w/2 * (1 + 2f·px / (-pz·A) + offsetX)
///     y = h/2 + w·pixelAspect/2 * (2f·py / (-pz·A) + offsetY)
///
/// with (px, py, pz) the point in the camera's own frame.
[[nodiscard]] inline bool projectToPicture(const CameraView& camera,
                                           const xform::Vec3& world, double width,
                                           double height, double pixelAspect,
                                           double& x, double& y) {
    const xform::Vec3 p = xform::viewFromCameraWorld(camera.cameraToWorld).point(world);
    if (!(p.z < -1e-9) || !(camera.aperture > 0.0) || !(camera.focal > 0.0)) {
        return false;
    }
    const double u = 2.0 * camera.focal * p.x / (-p.z * camera.aperture) + camera.offsetX;
    const double v = 2.0 * camera.focal * p.y / (-p.z * camera.aperture) + camera.offsetY;
    x = width * 0.5 * (1.0 + u);
    y = height * 0.5 + width * pixelAspect * 0.5 * v;
    return std::isfinite(x) && std::isfinite(y);
}

/// The ray under a picture position: where a drag over the picture points
/// into the scene. `origin` is the camera's position, `direction` is not
/// normalised. The inverse of `projectToPicture` for every depth.
inline void rayFromPicture(const CameraView& camera, double x, double y, double width,
                           double height, double pixelAspect, xform::Vec3& origin,
                           xform::Vec3& direction) {
    const double u = (width > 0.0 ? 2.0 * x / width - 1.0 : 0.0) - camera.offsetX;
    const double v = (width * pixelAspect > 0.0
                          ? 2.0 * (y - height * 0.5) / (width * pixelAspect)
                          : 0.0) -
                     camera.offsetY;
    const xform::Vec3 local{u * camera.aperture / (2.0 * camera.focal),
                            v * camera.aperture / (2.0 * camera.focal), -1.0};
    origin = camera.cameraToWorld.translation();
    direction = camera.cameraToWorld.direction(local);
}

}   // namespace gizmo

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
// A point is as many numbers as the gizmo's space has: x, y in 2D and
// x, y, z in 3D. The array does not say which; the gizmo does, and the writer
// and the reader pass the same `dimensions`.
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
    /// x, y pairs in 2D, x, y, z triples in 3D, in the gizmo's space.
    std::vector<float> points;
};

/// Hard ceilings, so a reader never allocates what a corrupt array claims.
/// A drawing is for a person to look at; past these it is not a drawing.
inline constexpr size_t kMaxStrokes = 4096;
inline constexpr size_t kMaxStrokePoints = 65536;

/// Packs strokes into the array an attachment carries.
[[nodiscard]] inline std::vector<float> encodeDrawing(
    const std::vector<Stroke>& strokes, int dimensions = 2) {
    const size_t stride = dimensions == 3 ? 3 : 2;
    std::vector<float> out;
    out.push_back(kDrawingVersion);
    out.push_back(static_cast<float>(strokes.size()));
    for (const Stroke& stroke : strokes) {
        out.push_back(static_cast<float>(stroke.flags));
        out.push_back(stroke.red);
        out.push_back(stroke.green);
        out.push_back(stroke.blue);
        out.push_back(stroke.alpha);
        out.push_back(static_cast<float>(stroke.points.size() / stride));
        out.insert(out.end(), stroke.points.begin(),
                   stroke.points.begin() +
                       static_cast<std::ptrdiff_t>(stroke.points.size() / stride * stride));
    }
    return out;
}

/// Reads what `encodeDrawing` wrote. False, and `into` empty, for anything
/// that is not exactly that: a version this does not know, a count that runs
/// past the end, a number that is not finite. A host shows nothing rather
/// than half a drawing.
[[nodiscard]] inline bool decodeDrawing(const std::vector<float>& data,
                                        std::vector<Stroke>& into,
                                        int dimensions = 2) {
    into.clear();
    const size_t stride = dimensions == 3 ? 3 : 2;
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
        if (data.size() - at < points * stride) {
            return fail();
        }
        stroke.points.assign(data.begin() + static_cast<std::ptrdiff_t>(at),
                             data.begin() + static_cast<std::ptrdiff_t>(at + points * stride));
        for (float value : stroke.points) {
            if (!std::isfinite(value)) {
                return fail();
            }
        }
        at += points * stride;
        into.push_back(std::move(stroke));
    }
    if (at != data.size()) {
        return fail();
    }
    return true;
}

}   // namespace gizmo

}   // namespace aofx
