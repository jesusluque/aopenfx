# Gizmos: the standard

How an AOFX effect gets any handle it wants in the viewer, on the picture or in
the 3D scene, without drawing one. The declarations are in
`sdk/include/aofx/Gizmo.h`; this document is the contract a plugin and a host
each keep. **MUST**, **SHOULD** and **MAY** are used in their RFC 2119 sense.

Since **ABI 26**; 3D gizmos and attachments read on an input since **ABI 27**.

## The rule this keeps

The host draws every gizmo. A plugin cannot, and should not be able to: it
would have to link the application's UI toolkit, share its event loop and its
coordinate systems, and one crash in a handle would take the window down. In a
remote session the plugin is not even loaded where the window is.

`ParamRole` already made that bargain for three cases: a place, an angle, a
scale. Everything else had nothing. A corner pin got four spin boxes, a crop
got two loose points, a blur's radius could not be dragged, and nothing could
be placed in the scene at all.

So the standard adds no way to draw. It adds a **vocabulary**: eighteen
primitives a host already knows how to draw and drag, a way to say which
numbers of which parameters each one is made of, frames the others can be
placed in, and cameras that put the scene on the picture. Any gizmo is a
composition of those. The effect says *what*; the host decides how it looks,
how it is hit-tested, how it snaps and how it is undone.

What cannot be expressed as an edit of parameters (the path a tracker
followed, the point cloud a solve found) is a `Drawing`: strokes the effect
computes and attaches to its output, which the host draws and never drags.

## The model

```
EffectDesc::gizmos : [GizmoDesc]
GizmoDesc          = id, kind, bindings[], space (+ parent | clip), camera,
                     rotationOrder, style, constraint, visibility,
                     shownWhen/shownAlso, repeat, text
GizmoBinding       = slot  <-  param[component..]
                            |  attachment[component..]  (on the output, or on input `clip`)
                            |  constant
                     drawn = value * scale + offset
```

A **kind** is a list of **slots**. A slot is a group of numbers: `(p)` is a
place in the gizmo's space (two numbers in a 2D space, three in a 3D one), and
`(1)`, `(3)`, `(12)` are that many numbers whatever the space. Each slot is
filled by a **binding** from exactly one **source**:

| Source | Drawn from | A drag |
|---|---|---|
| `param` | The parameter's value at the viewer's time, components `component ..` | Writes the parameter |
| `attachment` | What the last render of this node `attach`ed under that id | Nothing: read-only |
| `attachment` + `clip` | What arrives at that input under that id (`InputPlane::values`): a tracker's corners, a solved camera | Nothing: read-only |
| `constant` | The numbers themselves | Nothing |

With `param` or `attachment` set, `constant` is the **fallback** drawn while the
source is absent (an attachment exists only after a render). The helpers
`bindParam`, `bindAttachment`, `bindInput` and `bindConstant` build each one.

## The primitives

| Kind | Dim | Slots | Handles a host MUST offer |
|---|---|---|---|
| `Point` | 2D, 3D | `at(p)` | One handle |
| `Line` | 2D, 3D | `from(p)`, `to(p)` | Both ends; the segment moves both |
| `Arrow` | 2D, 3D | `from(p)`, `to(p)` | As Line, with a head at `to` |
| `Box` | 2D, 3D | `corner1(p)`, `corner2(p)`, either order | 2D: four corners, four edges, the inside moves all. 3D: a cuboid with a handle per face |
| `Circle` | 2D | `centre(p)`, `radius(1)` | Centre, and the rim for the radius |
| `Ellipse` | 2D | `centre(p)`, `radii(2)`, `angle(1)`? | Centre, a rim handle per axis, rotation when `angle` is bound |
| `Angle` | 2D | `centre(p)`, `angle(1)`, `radius(1)`? | A ring with a handle; unbound radius is a fixed size on screen |
| `Quad` | 2D, 3D | `corner1(p)` .. `corner4(p)`, anticlockwise from bottom left | Four corners, four edges, the inside moves all. In 3D, a card |
| `Polyline` | 2D, 3D | `points(p·n)`, open | One handle per point |
| `Polygon` | 2D, 3D | `points(p·n)`, closed | One handle per point; the inside moves all |
| `Distance` | 2D | `origin(p)`, `length(1)`, `angle(1)`? | One handle at `origin + length·(cos a, sin a)`, dragged along that direction only |
| `Frame` | 2D | `translate(2)`, `angle(1)`?, `scale(2)`?, `centre(2)`? | Centre, rotation ring, scale handles; the space of its children |
| `Label` | 2D, 3D | `at(p)` | None: text from `GizmoDesc::text` |
| `Crosshair` | 2D, 3D | `at(p)` | None |
| `Drawing` | 2D, 3D | `strokes`, attachment only | None |
| `Sphere` | 3D | `centre(p)`, `radius(1)` | Centre, and one on the silhouette for the radius |
| `Frame3D` | 3D | `translate(3)`?, `rotate(3)`?, `scale(3)`?, `pivot(3)`?, or `matrix(12)`? instead | The host's 3D transform handle: an arrow and a plane per axis, a ring per angle, scale per axis and uniform; the space of its children |
| `Camera` | 3D | `translate(3)` + `rotate(3)`? or `matrix(12)`; `focal(1)`; `aperture(1)`?; `offset(2)`? | A frustum, dragged as a Frame3D; projects the 3D gizmos that name it onto the picture |

`?` marks an optional slot. An unbound optional slot takes its neutral value:
`angle` and `rotate` 0, `scale` 1, `centre` and `pivot` 0, `aperture` 24.576 mm,
`offset` 0. `gizmoWorksIn(kind, dimensions)` answers the Dim column.

`points` has any number of places: the slot MAY be bound several times, and
each binding adds its numbers in order. A `Polygon` over four `Position`
parameters is four bindings of `points`.

**Angles** are degrees, anticlockwise from +x (2D) or right-handed about each
axis (3D), as everywhere in the SDK.

**A Frame's matrix** is `T(translate) · T(centre) · R(angle) · S(scale) · T(−centre)`
(`gizmo::frameMatrix`). **A Frame3D's** is `xform::localMatrix` with SRT and
the gizmo's `rotationOrder` (Nuke's ZXY by default), which is what an Axis
means (`gizmo::frame3DMatrix`). `matrix(12)` is the upper 3×4, row-major
(`gizmo::matrixFromRows`), for a frame the effect already has as a matrix. A
host MUST compute them with these functions so the gizmo and the render cannot
disagree.

## Spaces

| `GizmoSpace` | Dim | Numbers are | Use |
|---|---|---|---|
| `Canonical` (default) | 2D | Full-resolution project pixels, y up, origin bottom left | What `ParamRole::Position` means; almost everything on the picture |
| `Project` | 2D | Fractions of the project format, 0..1 | A guide that belongs to the frame: safe areas, thirds |
| `Input` | 2D | Fractions of an input's region of definition (`clip`, else the pass-through input) | A gizmo that belongs to the picture, not the frame |
| `World` | 3D | Scene units, right-handed, Y up | The scene: lights, cards, cameras, axes |
| `Parent` | the parent's | The frame of the `Frame` (2D) or `Frame3D` (3D) named in `parent` | Handles that turn with a transform, a card on an axis |

`gizmoDimensions` gives a gizmo's dimension from its space and its chain of
parents. Frames MAY nest, a Frame in a Frame and a Frame3D in a Frame3D; a host
MUST refuse a chain that loops or that mixes the two (`checkGizmos` names
both).

A host MUST convert through the viewer's own mapping (zoom, pan, proxy scale,
pixel aspect). An effect never sees the screen. **Line widths and handle sizes
are screen points**, never picture pixels or scene units.

## 3D: where a 3D gizmo is drawn

A 3D gizmo is drawn in two places, and a host MUST offer the first it has:

1. **In the host's 3D view**, through the view's own camera, with the host's
   usual 3D handles. Parts hidden behind scene geometry SHOULD be drawn dimmed
   rather than not at all: a handle you cannot see is a handle you cannot
   grab.
2. **Over the picture**, projected through a **camera**: the `Camera` gizmo
   named in `camera`, or, when that is empty, the nearest parent's. A 3D gizmo
   with no camera anywhere up its chain is drawn only in the 3D view.

The projection is `gizmo::projectToPicture`, and the effect that renders
through the same camera SHOULD call it too, so a handle sits on the pixel it
moves:

```
p      = inverse(cameraToWorld) · world          the point in the camera's frame; it looks down −Z
u      = 2f·px / (−pz·A) + offsetX
v      = 2f·py / (−pz·A) + offsetY
x      = w/2 · (1 + u)                           canonical pixels
y      = h/2 + w·pixelAspect/2 · v
```

`f` is `focal`, `A` the horizontal `aperture` (both millimetres), `w`, `h` the
project format and `pixelAspect` the project's. The horizontal aperture spans
the width and the vertical follows from the format, as a Nuke camera does. A
point on or behind the camera's plane has no position: a host MUST clip lines
and outlines at the near side rather than draw them through infinity.

A camera usually comes from upstream, read-only:

```cpp
aofx::GizmoDesc shot;
shot.id = "shot";
shot.kind = aofx::GizmoKind::Camera;
shot.space = aofx::GizmoSpace::World;
shot.bindings = {aofx::bindInput("matrix", "Track", "camera.matrix"),
                 aofx::bindInput("focal", "Track", "camera.focal", 0, {35.0})};
```

or from the effect's own parameters, in which case dragging it in the 3D view
moves the camera and the whole projected overlay with it.

## Dragging: what a host MUST do

1. **Invert the chain.** A drag is carried back through the viewer, the
   camera (for a 3D gizmo over the picture), the parent frames and the
   binding's `scale`/`offset`: `value = (drawn − offset) / scale`. `scale`
   MUST NOT be zero.
2. **Write parameters, nothing else.** A drag is a parameter edit at the
   viewer's time, keyed or not by the host's own animation rules, clamped to
   the parameter's `hardMin`/`hardMax`. An Integer parameter is rounded.
3. **One gesture, one undo step,** named after the gizmo's `label` (or `id`),
   however many parameters it touched. A Box corner writes two parameters and
   undoes as one.
4. **Honour the gizmo:** `constraint`, `readOnly` on a binding, and the
   read-only sources. A handle whose every slot is read-only is drawn and not
   hit-tested. A `matrix(12)` slot is never written: twelve numbers do not go
   back into three angles unambiguously.
5. **Not write what is locked.** A parameter the host treats as locked
   (an expression, a link to another node, a group promotion driven from
   outside) is drawn and not dragged.
6. **Render as it would for a panel edit.** The effect cannot tell a drag from
   a typed number, and MUST NOT need to.

### Dragging in 3D

A 2D drag names two numbers and a 3D place has three, so the third comes from
the constraint:

| `constraint` | In the 3D view | Over the picture, through the camera |
|---|---|---|
| `Free` | The host's translate handle: an axis per arrow, a plane per square | On the plane through the point that faces the camera: the depth does not change |
| `Horizontal`, `Vertical`, `AlongZ` | Along the space's X, Y or Z axis | Along that axis, to the point on it nearest the ray under the cursor |
| `OnXY`, `OnXZ`, `OnYZ` | On that plane of the space | Where the ray under the cursor meets that plane (`OnXZ` is the ground) |
| `Locked` | Not dragged | Not dragged |

`gizmo::rayFromPicture` is the ray under a picture position, the exact inverse
of the projection. A drag whose ray is parallel to its plane, or meets it behind
the camera, MUST leave the value where it was rather than send it to infinity.

A **Frame3D's rings** each change their own component of `rotate` and no
other: the ring for X changes `rotate.x`, whatever the rotation order. They
are drawn about the axes those components turn about in `rotationOrder`, so
the ring under the cursor and the number that changes are always the same one.
A **Camera** drags as a Frame3D; `focal`, `aperture` and `offset` are not
dragged and are edited in the panel.

The 3D constraints (`AlongZ`, `OnXY`, `OnXZ`, `OnYZ`) are meaningless on the
picture, and `checkGizmos` names them on a 2D gizmo.

### Everything else about a drag

Hit-testing, snapping, modifier keys, hover and selection highlighting are the
host's own and SHOULD match its built-in gizmos, 2D and 3D. Where two handles
overlap the host SHOULD prefer the one declared later, so an effect lists the
most specific gizmo last; in 3D the nearer handle wins before declaration order
is asked.

## Showing: when a gizmo is drawn

A gizmo is drawn only if **all** of these hold:

- its `visibility` allows it: `WhenSelected` (default) while the node is
  selected or its panel is open, `Always` while its output is in the viewer,
  `WhileEditing` only while one of its parameters is being changed;
- its `shownWhen` and `shownAlso` hold, read exactly as a row's;
- every parameter it binds is shown in the panel: a handle for a hidden row is
  a handle for something the node is ignoring. In a repeated gizmo, a row
  hidden only because another item of the pool is selected still counts as
  shown;
- for `Parent` space, its frame's `shownWhen` and `shownAlso` hold (the frame's
  `visibility` does not gate its children);
- for a 3D gizmo over the picture, it has a camera and the camera has a
  position (a camera read from an input that has not produced one yet, with
  no fallback, projects nothing).

## Pools: `repeat`

`repeat` names an Integer parameter with `ParamRole::ItemCount`. The gizmo is
drawn once per item in use, with `{i}` in binding names and in `text` replaced
by the item's number **from one** (`corner{i}` is `corner1`, `corner2`, …). The
item the pool's `ItemIndex` names is drawn as selected, and selecting a
repeated handle in the viewer SHOULD set `ItemIndex` to its item.

## Labels

`text` is shown as written, with `{name}` replaced by parameter `name`'s value
formatted as the panel formats it. A `{name}` that is not a parameter is shown
as written. `{i}` is the item number in a repeated gizmo. A 3D label faces the
viewer and keeps its size on screen.

## Style

`GizmoStyle` is a suggestion, not a pixel specification: colour (straight RGBA,
0..1; default the shape outline's yellow), line width in screen points, line
pattern, handle shape, and a faint fill for closed kinds. A host MAY brighten
a selected or hovered gizmo and MUST keep a gizmo legible against the picture
(an outline or shadow is the host's choice). In 3D, a host MAY colour the axes
of a Frame3D its usual red, green and blue whatever the style says.

## Computed drawings

A `Drawing` gizmo binds `strokes` to an attachment. The effect fills it in
`process`:

```cpp
using aofx::gizmo::Stroke;
std::vector<Stroke> path(1);
path[0].points = {x0, y0, x1, y1, x2, y2};           // in the gizmo's space
request.attach("track.path", aofx::gizmo::encodeDrawing(path));

std::vector<Stroke> cloud(1);                        // a 3D one: triples
cloud[0].flags = aofx::gizmo::kStrokePoints;
cloud[0].points = {x0, y0, z0, x1, y1, z1};
request.attach("solve.points", aofx::gizmo::encodeDrawing(cloud, 3));
```

The layout, floats throughout:

```
[ version = 1, strokeCount,
  flags, r, g, b, a, pointCount, x, y[, z], x, y[, z], ...    one per stroke
  ... ]
```

A point has as many numbers as the gizmo's space: the array does not say, the
gizmo does, and the writer and the reader pass the same `dimensions`.

`flags` is a sum of `kStrokeClosed` (1), `kStrokePoints` (2, dots instead of
lines), `kStrokeDashed` (4) and `kStrokeColoured` (8, use the stroke's own
colour instead of the gizmo's). A host MUST read it with `decodeDrawing` or an
equivalent that is as strict: a version it does not know, a count that runs
past the end, trailing numbers or a coordinate that is not finite mean **draw
nothing**, never half a drawing. `kMaxStrokes` and `kMaxStrokePoints` bound what
a reader allocates.

The attachment travels with the picture like any other, so the drawing is
always the one for the frame on screen. It is read-only by construction; an
effect that wants an editable path declares parameters and a `Polyline`.

## Gizmos and roles

`ParamRole::Position`, `Angle` and `Scale` still give their handles. **A
parameter bound by a gizmo the host draws gets no handle from its role**, so
declaring a gizmo never draws a handle twice. A gizmo the host skips (a kind it
does not know, a problem `checkGizmos` named) suppresses nothing, and the role's
handle comes back.

An effect SHOULD therefore keep the roles on parameters it also binds: that
fallback is worth having, and the panel still gets the role's slider range.

## Compatibility

- **Enumerators are appended, never reordered.** `GizmoKind`, `GizmoSpace`,
  `GizmoConstraint`, `GizmoVisibility`, `xform::RotationOrder` and the style
  enums travel as ints.
- **A host that meets a value it does not know** skips that gizmo and says so
  once, naming the effect and the gizmo's id. It MUST NOT refuse the bundle
  and MUST NOT draw a different primitive in its place.
- **A host with no 3D view** still draws 3D gizmos over the picture through
  their camera, and skips (silently) those without one.
- **Anything that changes the shape of these structs** is an ABI bump, like
  every struct in `Descriptor.h`. A new kind is an ABI bump as well: same
  interface, different behaviour, which is what the number is for.
- **The drawing layout** carries its own version, as `aofx/Shape.h` does.

## Checking

`aofx::checkGizmos(const EffectDesc&)` returns one sentence per problem:

- slots: unbound, doubly bound, not a slot of the kind, a constant of the wrong
  size for the space;
- sources: a parameter that does not exist, is not a number or is too short
  for the space; two sources on one binding; a `clip` without an attachment or
  naming no input; a zero scale; `{i}` without `repeat`;
- spaces: a parent that is not a Frame or Frame3D, a chain that loops, a 2D
  kind in a 3D space or the reverse, a 3D constraint on a 2D gizmo;
- 3D: a `camera` on a 2D gizmo or naming something that is not a Camera,
  `matrix` beside the slots it replaces, a Camera with neither `translate` nor
  `matrix`, a rotation order this SDK does not know;
- the rest: a `repeat` that is not an ItemCount, a Label with no text, a
  visibility rule on a missing parameter, a Drawing that is not an attachment,
  a kind this SDK does not know.

A plugin SHOULD call it from a test: a gizmo bound to a renamed parameter is a
handle that silently stops appearing. A host SHOULD call it at load and skip
the gizmos it names, reporting each once.

## Examples

A crop box (`examples/crop`):

```cpp
aofx::GizmoDesc box;
box.id = "box";
box.label = "Crop box";
box.kind = aofx::GizmoKind::Box;
box.bindings = {aofx::bindParam("corner1", "corner1"),
                aofx::bindParam("corner2", "corner2")};
into.gizmos.push_back(box);
```

A corner pin (`examples/cornerpin`): a `Quad` with `corner1`..`corner4` each
bound to the parameter of the same name, and beside it a read-only `Quad` of
where the tracker on the Track input says the corners are:

```cpp
for (int index = 0; index < 4; ++index) {
    tracked.bindings.push_back(aofx::bindInput(
        "corner" + std::to_string(index + 1), "Track", "corners", index * 2));
}
```

A transform with a blur size that turns with it:

```cpp
aofx::GizmoDesc xf;
xf.id = "xf";
xf.kind = aofx::GizmoKind::Frame;
xf.bindings = {aofx::bindParam("translate", "translate"),
               aofx::bindParam("angle", "rotate"),
               aofx::bindParam("scale", "scale"),
               aofx::bindParam("centre", "center")};

aofx::GizmoDesc radius;
radius.id = "radius";
radius.kind = aofx::GizmoKind::Distance;
radius.space = aofx::GizmoSpace::Parent;
radius.parent = "xf";
radius.bindings = {aofx::bindParam("origin", "center"),
                   aofx::bindParam("length", "size")};
radius.bindings[1].scale = 0.5;                 // a diameter, drawn as a radius
radius.visibility = aofx::GizmoVisibility::WhileEditing;
```

A pool of points, one handle per item:

```cpp
aofx::GizmoDesc points;
points.id = "points";
points.kind = aofx::GizmoKind::Point;
points.bindings = {aofx::bindParam("at", "point{i}")};
points.repeat = "count";                        // an ItemCount parameter
```

What a tracker found, drawn and never dragged:

```cpp
aofx::GizmoDesc found;
found.id = "found";
found.kind = aofx::GizmoKind::Crosshair;
found.bindings = {aofx::bindAttachment("at", "track.position", {0.0, 0.0})};
found.visibility = aofx::GizmoVisibility::Always;
```

In 3D, with the `shot` camera above: a light dragged on the ground over the
picture, its reach, and a card on an axis that inherits the camera:

```cpp
aofx::GizmoDesc light;
light.id = "light";
light.kind = aofx::GizmoKind::Point;
light.space = aofx::GizmoSpace::World;
light.camera = "shot";
light.constraint = aofx::GizmoConstraint::OnXZ;
light.bindings = {aofx::bindParam("at", "light")};           // a Double of 3

aofx::GizmoDesc reach;
reach.id = "reach";
reach.kind = aofx::GizmoKind::Sphere;
reach.space = aofx::GizmoSpace::World;
reach.camera = "shot";
reach.bindings = {aofx::bindParam("centre", "light"),
                  aofx::bindParam("radius", "reach")};

aofx::GizmoDesc axis;
axis.id = "axis";
axis.kind = aofx::GizmoKind::Frame3D;
axis.space = aofx::GizmoSpace::World;
axis.camera = "shot";
axis.bindings = {aofx::bindParam("translate", "axisTranslate"),
                 aofx::bindParam("rotate", "axisRotate")};

aofx::GizmoDesc card;
card.id = "card";
card.kind = aofx::GizmoKind::Quad;
card.space = aofx::GizmoSpace::Parent;                        // 3D, from the axis
card.parent = "axis";                                         // and so is its camera
card.bindings = {aofx::bindConstant("corner1", {-1, -1, 0}),
                 aofx::bindConstant("corner2", { 1, -1, 0}),
                 aofx::bindConstant("corner3", { 1,  1, 0}),
                 aofx::bindConstant("corner4", {-1,  1, 0})};
```

`tests/test_gizmos.cpp` declares every one of these and checks them, together
with the projection and its inverse.

## What this deliberately does not do

- **No drawing callback.** Not now and not as an escape hatch: the first one
  is the end of the rule this whole document keeps.
- **No custom hit-testing or cursors.** A host's gizmos feel the same on every
  node, which is worth more than any one node's idea of a better cursor.
- **No interaction events to the effect.** A drag is a parameter edit; the
  effect sees values, as it always has.
- **No lens distortion in the projection.** A gizmo over a distorted plate is
  projected through a pinhole. An effect that undistorts can attach a
  `Drawing` of the distorted outline instead.
