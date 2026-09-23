# Gizmos: the standard

How an AOFX effect gets any handle it wants in the viewer without drawing one.
The declarations are in `sdk/include/aofx/Gizmo.h`; this document is the
contract a plugin and a host each keep. **MUST**, **SHOULD** and **MAY** are
used in their RFC 2119 sense.

Since **ABI 26**.

## The rule this keeps

The host draws every gizmo. A plugin cannot, and should not be able to: it
would have to link the application's UI toolkit, share its event loop and its
coordinate systems, and one crash in a handle would take the window down.

`ParamRole` already made that bargain for three cases: a place, an angle, a
scale. Everything else had nothing. A corner pin got four spin boxes, a crop
got two loose points, a blur's radius could not be dragged.

So the standard adds no way to draw. It adds a **vocabulary**: fifteen
primitives a host already knows how to draw and drag, a way to say which
numbers of which parameters each one is made of, and a frame the others can be
placed in. Any gizmo is a composition of those. The effect says *what*; the
host decides how it looks, how it is hit-tested, how it snaps and how it is
undone.

What cannot be expressed as an edit of parameters (the path a tracker
followed, the grid a lens solve found) is a `Drawing`: strokes the effect
computes and attaches to its output, which the host draws and never drags.

## The model

```
EffectDesc::gizmos : [GizmoDesc]
GizmoDesc          = id, kind, bindings[], space (+ parent | clip), style,
                     constraint, visibility, shownWhen/shownAlso, repeat, text
GizmoBinding       = slot  <-  param[component..] | attachment[component..] | constant
                     drawn = value * scale + offset
```

A **kind** is a list of **slots**. A slot is a group of numbers: `(2)` is a
point in the gizmo's space, `(1)` is one number. Each slot is filled by a
**binding** from exactly one **source**:

| Source | Drawn from | A drag |
|---|---|---|
| `param` | The parameter's value at the viewer's time, components `component .. component+width-1` | Writes the parameter |
| `attachment` | What the last render of this node `attach`ed under that id | Nothing: read-only |
| `constant` | The numbers themselves | Nothing |

With `param` or `attachment` set, `constant` is the **fallback** drawn while the
source is absent (an attachment exists only after a render).

## The primitives

| Kind | Slots | Handles a host MUST offer |
|---|---|---|
| `Point` | `at(2)` | One handle |
| `Line` | `from(2)`, `to(2)` | Both ends; the segment moves both |
| `Arrow` | `from(2)`, `to(2)` | As Line, with a head at `to` |
| `Box` | `corner1(2)`, `corner2(2)`, either order | Four corners, four edges, the inside moves all |
| `Circle` | `centre(2)`, `radius(1)` | Centre, and the rim for the radius |
| `Ellipse` | `centre(2)`, `radii(2)`, `angle(1)`? | Centre, a rim handle per axis, rotation when `angle` is bound |
| `Angle` | `centre(2)`, `angle(1)`, `radius(1)`? | A ring with a handle; unbound radius is a fixed size on screen |
| `Quad` | `corner1(2)` .. `corner4(2)`, anticlockwise from bottom left | Four corners, four edges, the inside moves all |
| `Polyline` | `points(2n)`, open | One handle per point |
| `Polygon` | `points(2n)`, closed | One handle per point; the inside moves all |
| `Distance` | `origin(2)`, `length(1)`, `angle(1)`? | One handle at `origin + length·(cos a, sin a)`, dragged along that direction only |
| `Frame` | `translate(2)`, `angle(1)`?, `scale(2)`?, `centre(2)`? | Centre, rotation ring, scale handles; the space of its children |
| `Label` | `at(2)` | None: text from `GizmoDesc::text` |
| `Crosshair` | `at(2)` | None |
| `Drawing` | `strokes(n)`, attachment only | None |

`?` marks an optional slot. An unbound optional slot takes its neutral value:
`angle` 0, `scale` (1, 1), `centre` (0, 0).

`points` and `strokes` have width zero: the slot MAY be bound several times,
and each binding adds its numbers in order. A `Polygon` over four `Position`
parameters is four bindings of `points`.

**Angles** are degrees, anticlockwise from +x, as everywhere in the SDK.

**A Frame's matrix** is `T(translate) · T(centre) · R(angle) · S(scale) · T(−centre)`:
`aofx/Transform.h`'s SRT with a pivot and no skew. A host SHOULD compute it
with those functions so the gizmo and the render cannot disagree.

## Spaces

| `GizmoSpace` | Numbers are | Use |
|---|---|---|
| `Canonical` (default) | Full-resolution project pixels, y up, origin bottom left | What `ParamRole::Position` means; almost everything |
| `Project` | Fractions of the project format, 0..1 | A guide that belongs to the frame: safe areas, thirds |
| `Input` | Fractions of an input's region of definition (`clip`, else the pass-through input) | A gizmo that belongs to the picture, not the frame |
| `Parent` | The frame of the `Frame` named in `parent` | Handles that turn with a transform |

Frames MAY nest: a Frame in `Parent` space is placed in its own parent. A host
MUST refuse a chain that loops (and `checkGizmos` names it).

A host MUST convert through the viewer's own mapping (zoom, pan, proxy scale,
pixel aspect). An effect never sees the screen. **Line widths and handle sizes
are screen points**, never picture pixels.

## Dragging: what a host MUST do

1. **Invert the chain.** A drag in screen space is carried back through the
   viewer, the parent frames and the binding's `scale`/`offset`:
   `value = (drawn − offset) / scale`. `scale` MUST NOT be zero.
2. **Write parameters, nothing else.** A drag is a parameter edit at the
   viewer's time, keyed or not by the host's own animation rules, clamped to
   the parameter's `hardMin`/`hardMax`. An Integer parameter is rounded.
3. **One gesture, one undo step,** named after the gizmo's `label` (or `id`),
   however many parameters it touched. A Box corner writes two parameters and
   undoes as one.
4. **Honour the gizmo:** `constraint` (`Horizontal`, `Vertical`, `Locked`),
   `readOnly` on a binding, and the read-only sources. A handle whose every
   slot is read-only is drawn and not hit-tested.
5. **Not write what is locked.** A parameter the host treats as locked
   (an expression, a link to another node, a group promotion driven from
   outside) is drawn and not dragged.
6. **Render as it would for a panel edit.** The effect cannot tell a drag from
   a typed number, and MUST NOT need to.

Hit-testing, snapping, modifier keys, hover and selection highlighting are the
host's own and SHOULD match its built-in gizmos. Where two handles overlap the
host SHOULD prefer the one declared later, so an effect lists the most specific
gizmo last.

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
  `visibility` does not gate its children).

## Pools: `repeat`

`repeat` names an Integer parameter with `ParamRole::ItemCount`. The gizmo is
drawn once per item in use, with `{i}` in binding names and in `text` replaced
by the item's number **from one** (`corner{i}` is `corner1`, `corner2`, …). The
item the pool's `ItemIndex` names is drawn as selected, and selecting a
repeated handle in the viewer SHOULD set `ItemIndex` to its item.

## Labels

`text` is shown as written, with `{name}` replaced by parameter `name`'s value
formatted as the panel formats it. A `{name}` that is not a parameter is shown
as written. `{i}` is the item number in a repeated gizmo.

## Style

`GizmoStyle` is a suggestion, not a pixel specification: colour (straight RGBA,
0..1; default the shape outline's yellow), line width in screen points, line
pattern, handle shape, and a faint fill for closed kinds. A host MAY brighten
a selected or hovered gizmo and MUST keep a gizmo legible against the picture
(an outline or shadow is the host's choice).

## Computed drawings

A `Drawing` gizmo binds `strokes` to an attachment. The effect fills it in
`process`:

```cpp
using aofx::gizmo::Stroke;
std::vector<Stroke> path(1);
path[0].points = {x0, y0, x1, y1, x2, y2};           // in the gizmo's space
request.attach("track.path", aofx::gizmo::encodeDrawing(path));
```

The layout, floats throughout:

```
[ version = 1, strokeCount,
  flags, r, g, b, a, pointCount, x, y, x, y, ...    one per stroke
  ... ]
```

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
  `GizmoConstraint`, `GizmoVisibility` and the style enums travel as ints.
- **A host that meets a value it does not know** skips that gizmo and says so
  once, naming the effect and the gizmo's id. It MUST NOT refuse the bundle
  and MUST NOT draw a different primitive in its place.
- **Anything that changes the shape of these structs** is an ABI bump, like
  every struct in `Descriptor.h`. A new kind is an ABI bump as well: same
  interface, different behaviour, which is what the number is for.
- **The drawing layout** carries its own version, as `aofx/Shape.h` does.

## Checking

`aofx::checkGizmos(const EffectDesc&)` returns one sentence per problem:
unbound or doubly bound slots, unknown slots, a parameter that does not exist
or is not a number or is too short, two sources on one binding, a zero scale,
a parent that is not a Frame or a loop of frames, `{i}` without `repeat`, a
`repeat` that is not an ItemCount, a Label with no text, a visibility rule on
a missing parameter, a Drawing that is not an attachment, a kind this SDK does
not know.

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
bound to the parameter of the same name.

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

`tests/test_gizmos.cpp` declares every one of these and checks them.

## What this deliberately does not do

- **No drawing callback.** Not now and not as an escape hatch: the first one
  is the end of the rule this whole document keeps.
- **No 3D.** Every space is the picture's plane. A camera or a card in a 3D
  scene needs a projection the viewer owns, and is a separate proposal.
- **No custom hit-testing or cursors.** A host's gizmos feel the same on every
  node, which is worth more than any one node's idea of a better cursor.
- **No interaction events to the effect.** A drag is a parameter edit; the
  effect sees values, as it always has.
