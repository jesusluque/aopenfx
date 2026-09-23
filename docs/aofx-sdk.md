# The AOFX SDK

**A**lternative **o**pen**FX**: a plugin interface for effects that run on the GPU
through CUDA or Metal. It is meant to sit **beside** OpenFX in a host, not to
replace it.

OpenFX does reach GPUs. Its GPU rendering suite (`ofxGPURender.h`) covers
OpenGL, and since OpenFX 1.5 CUDA (`kOfxImageEffectPropCudaStream`) and Metal
(`kOfxImageEffectPropMetalRenderSupported`, `kOfxImageEffectPropMetalCommandQueue`);
whether a given effect gets them depends on what both the host and the plugin
implement. What AOFX offers is a smaller contract, not a capability OpenFX lacks:

- **One kernel source for both backends.** A kernel is written once in Slang and
  compiled at build time for Metal and for CUDA; the plugin never touches either
  API.
- **The host owns the machinery.** Buffers, scratch memory, dispatch, kept state
  and inference runtimes are the host's; a plugin describes work and asks for
  it.
- **No UI toolkit or host library in the plugin.** A plugin links nothing but
  these headers.

The whole SDK is C++ headers in `sdk/include/aofx/`. A plugin links nothing but
those headers: not the host, not Qt, not the image library. A plugin that linked
the host would share its allocator, its logging and its dependencies, and every
one of those is a way to take the application down.

## The shape of a plugin

A bundle, discovered on a search path, opened with `dlopen`:

```
Blur.aofx.bundle/Contents/MacOS/Blur.aofx      the module
Blur.aofx.bundle/Contents/Info.plist           macOS only
```

Four `extern "C"` entry points, all written by one macro:

```cpp
AOFX_EXPORT_EFFECTS(Blur)          // or several: AOFX_EXPORT_EFFECTS(Blur, Sharpen)
```

They are checked in this order, and the order is the point:

| Entry point | Why it is first |
|---|---|
| `AofxGetAbiVersion()` | A mismatch means the vtables have a different shape; calling through them is undefined |
| `AofxGetBuildTag()` | A version number cannot catch a toolchain mismatch. This can |
| `AofxGetEffectCount()` | A bundle may hold several |
| `AofxGetEffect(i)` | The host does not take ownership |

**One `Effect` instance serves every node of that plugin.** That is the design,
not a shortcut: an effect holds no per-node state, because state belongs to the
node and arrives in the `RenderRequest`. It matters when you reach for a cache —
see `keep` below.

## Writing an effect

Six virtuals, of which three are optional:

```cpp
class Blur final : public aofx::Effect {
    void describe(aofx::EffectDesc& into) override;              // what it is
    std::vector<aofx::KernelDesc> kernels() const override;      // what it brings
    bool process(const aofx::RenderRequest& request) override;   // do it

    bool isIdentity(const aofx::RenderRequest&) const override;  // optional
    aofx::Rect regionOfDefinition(double, const std::vector<aofx::Rect>&,
                                  const std::vector<aofx::ParamValue>&) const override;
    std::vector<aofx::Rect> regionOfInterest(double, const aofx::Rect& output,
                                             const std::vector<aofx::Rect>&,
                                             const std::vector<aofx::ParamValue>&) const override;
};
```

`isIdentity` is worth answering. A blur of zero, a grade at unity, a colour
conversion from a space to itself — the host then hands the input straight back
and never allocates an output. That is the state every node is in the moment it
is made, so it is the common case rather than a corner of one.

`regionOfDefinition` says where the picture ends up. The default is the union of
the inputs, which is right for anything that moves no pixel. A blur grows it; a
transform moves it *and* grows it, and must carry all four corners through the
matrix rather than two opposite ones — two corners are right only while the
transform is axis-aligned and silently wrong the moment anybody rotates
anything.

## Parameters

`ParamType` says what a parameter *is*, and it is a statement about meaning
rather than transport — the values travel identically either way:

| Type | What the panel gives you |
|---|---|
| `Double`, `Integer` | One to four spin boxes, with a slider |
| `Boolean` | A checkbox |
| `Choice` | A menu, `choices` holds the options; the value is the index |
| `String` | A field, with a Browse button for a path |
| `Colour` | A swatch, a picker and a **wheel**. Three or four components |
| `Curve` | An **editor you drag**, with a control point per component |

`ParamRole` says what the *numbers* mean, and it is how an effect asks for a
gizmo in the viewer:

```cpp
translate.role = aofx::ParamRole::Position;   // gets a draggable handle
rotate.role    = aofx::ParamRole::Angle;      // gets a ring, and a usable slider
scale.role     = aofx::ParamRole::Scale;      // a multiplier around one
```

**A plugin cannot draw its own gizmo, and should not be able to.** Drawing would
mean linking the toolkit the application is written in, sharing its event loop
and its coordinate systems, and one crash inside a handle would take the window
down. So the plugin says what its numbers are and the host draws the handle it
already knew how to draw.

`Angle` is worth more than the handle: a rotation with no declared range lands
on a 0..1 slider that cannot reach a single degree.

### Any other gizmo: `EffectDesc::gizmos`

Roles cover a place, an angle and a scale. For anything else (a crop box, a
corner pin, a radius you drag, a polygon, handles that turn with a transform, a
guide, the path a tracker followed, a light or a card in the scene) declare it from the primitives in
`aofx/Gizmo.h` and bind each to your parameters:

```cpp
aofx::GizmoDesc pin;
pin.id = "pin";
pin.kind = aofx::GizmoKind::Quad;
for (int i = 1; i <= 4; ++i) {
    const std::string name = "corner" + std::to_string(i);
    pin.bindings.push_back(aofx::bindParam(name, name));
}
into.gizmos.push_back(pin);
```

The same primitives work in 3D: in `World` space or inside a `Frame3D`, a place
has three numbers, and a `Camera` gizmo (often read from a solve upstream with
`bindInput`) projects them onto the picture with `gizmo::projectToPicture`, the
same function your render should call. `Sphere`, `Frame3D` and `Camera` are
3D-only; `Circle`, `Ellipse`, `Angle`, `Distance` and `Frame` are 2D-only.

The host draws it and turns a drag into a parameter edit, with one undo step;
the effect sees values as it always has. Something computed rather than set is
a `Drawing`: strokes you `attach` in `process`, drawn and never dragged. Call
`aofx::checkGizmos` from a test, because a gizmo bound to a renamed parameter
fails silently. The full contract, for plugins and hosts, is
[gizmos.md](gizmos.md).

### Choice indices are permanent

A script records the *index*. Appending to a choice list is safe; reordering it
changes what every saved script means.

## Kernels

Slang source, compiled at build time to a `.metallib` and a `.ptx` and embedded
as a byte array:

```cmake
aofx_add_kernel(aofx_blur blur ENTRY blurMain)
```

```cpp
std::vector<aofx::KernelDesc> kernels() const override {
    return {aofx::KernelDesc{"org.aopenfx.blur", "blurMain", k_blur, k_blurBytes}};
}
```

The name and the entry point are different things, and the first plugin written
against this SDK found that out the hard way: a kernel wants a globally unique
name, a Slang entry point wants to be called `blurMain`. Conflating them makes
the backend look for a function called `org.aopenfx.blur.blurMain`.

### Several entry points in one blob

```cmake
aofx_add_kernel(my_splat splat
    ENTRY splatProject ENTRY splatClear ENTRY splatBin ENTRY splatBlend)
```

```cpp
return {
    aofx::KernelDesc{"splatProject", "splatProject", k_splat, k_splatBytes},
    aofx::KernelDesc{"splatClear",   "splatClear",   k_splat, k_splatBytes},
    // ...
};
```

Four descriptors over one blob. They compile together because entry points in
one file share their buffer declarations and their parameter struct — which is
the whole reason to keep a multi-pass effect in one file. Four files would be
four copies of one layout, free to drift apart.

**And that sharing has a consequence that will cost you an evening if nobody
says it.** Buffer bindings are positional and belong to the *file*, not to the
entry point. A pass that reads only two of the four declared buffers still has
to be handed all four, in declaration order:

```cpp
const std::vector<aofx::Buffer> bound = {cloud, projected, tiles, out->buffer};
gpu->run(clear,   grid, bound, &params, sizeof(params));   // uses tiles only
gpu->run(project, grid, bound, &params, sizeof(params));   // uses two of them
```

Passing a subset compiles, links, runs, and renders black — the tiles arrive
bound where the cloud should be.

### The uniform struct

It is matched **byte for byte** by the kernel's parameter struct. Two rules,
both learned from real bugs:

```cpp
struct BlurUniforms {
    uint32_t width = 0;      // four-byte members, in declaration order
    uint32_t height = 0;
    // ...
};
static_assert(sizeof(BlurUniforms) == 64, "no padding, on any compiler");
```

1. **Four-byte members in declaration order, with a `static_assert` on the
   size.** Padding that differs between two compilers is a whole class of bug
   that appears only on the other backend.
2. **Never `float x[N]` in a constant buffer.** Every element of an array gets
   its own sixteen-byte slot, so `float x[8]` is 128 bytes on the kernel side
   and 32 on yours. Use `float4` members. This cost an afternoon and was
   credited to the wrong cause for three commits.

### A Grid counts threads, not groups

```cpp
gpu->run(kernel, aofx::Grid{buffer.width, buffer.height, 1}, ...);
```

The host divides by the kernel's own `numthreads`. Passing groups launches one
thread per group, which for a `[numthreads(16,16,1)]` blend means the top-left
corner of the picture is rendered and the rest is left untouched — and it does
not fail, it draws a sixteenth of a tile.

### And the group's *shape* is not yours to choose

The host picks it, and it is not the same everywhere: **a flat 256 for a 1-D
grid on CUDA, and 16×16 on Metal whatever you asked for**. Your `numthreads`
sets how the grid is divided, not how the threads are arranged inside a group.

So a kernel that declares `[numthreads(64,1,1)]` and then uses
`SV_GroupThreadID.x` to index `groupshared`, or `SV_GroupID` to index an output
row, is wrong on at least one backend. On Metal it gets sixteen live lanes of
the sixty-four it thinks it has, a tree reduction that folds slots nothing
wrote, and a group index that runs four times past the end of its buffer.

It does not crash and it does not draw a sixteenth of anything. The tracker's
reduction was written this way and reported a `support` of 14 592 pixels out of
230 400 — six per cent of the box, in a fixed comb pattern — which on a
synthetic field gives the exactly right answer and on real footage gives a
biased one. It took a test that planted a known affine and asked for it back to
see at all.

**If you need a reduction, do not use `groupshared`.** Give each thread of a
flat grid its own row of accumulators and fold the rows in a second dispatch of
one thread per term. Nothing about that depends on the group shape, which is
the only property worth having. `statsRow` and `statsFold` in
`examples/generate/imagestats.slang` are a worked example.

## The two regions, which are not the same question

`regionOfDefinition` is **how big your picture is**. The default is the union of
the inputs, which is right for anything that neither moves nor spreads. A blur
creates picture outside its input — that is what the soft edge is — and one that
does not say so gets cropped to its input and loses it.

`regionOfInterest` is **how much of each input you need** to fill what you were
asked for. The default is "the same rectangle", which is right for everything
that reads a pixel to write the one under it, and wrong for anything that
*moves* the picture.

It is wrong quietly, which is why this section exists. The host intersects what
you ask for with what the input has, so a corner pin that asks for the rectangle
it is producing is handed the part of its source that happens to lie under that
rectangle — for a quad elsewhere in the frame, most of it missing, and the
picture looks cropped rather than wrong.

A **spreading** effect answers it too. Its grown region of definition covers a
render of the whole picture, but a host may ask for part of it -- a viewer's
window, one tile -- and every pixel at the edge of that part sums neighbours
outside it; `examples/blur/Blur.cpp` grows the output by its reach, at the
render's scale. A **moving** effect answers it because there is no relationship
between where its output is and where its input is. Return one rectangle per input, in clip order;
an empty one means "the same as the output", so you can answer for one input and
leave the rest alone.

```cpp
std::vector<aofx::Rect> regionOfInterest(
    double, const aofx::Rect& output, const std::vector<aofx::Rect>& inputRods,
    const std::vector<aofx::ParamValue>&) const override {
    std::vector<aofx::Rect> wanted;
    for (const aofx::Rect& rod : inputRods) {
        wanted.push_back(rod.isEmpty() ? output : rod);   // all of it
    }
    return wanted;
}
```

## Buffers and rectangles

Every buffer carries a `rect`: where it sits in the picture. **Relate them; do
not assume they line up.**

```cpp
uniforms.srcOffsetX = target->buffer.rect.x1 - source->buffer.rect.x1;
uniforms.srcWidth   = source->buffer.width;
```

```hlsl
float4 fetchSource(uint2 xy) {
    const int sx = int(xy.x) + params.srcOffsetX;
    const int sy = int(xy.y) + params.srcOffsetY;
    if (sx < 0 || sy < 0 || sx >= int(params.srcWidth) || sy >= int(params.srcHeight)) {
        return float4(0.0, 0.0, 0.0, 0.0);   // outside is nothing
    }
    return src[uint(sy) * params.srcStride + uint(sx)];
}
```

"Same size in, same size out" is a fact about your effect, not a promise from
the host. Four kernels here assumed it and were right until a crop handed one an
output larger than its input; the frame filled with infinities, which is worse
than a crash because everything downstream keeps working.

## Generators

An effect with no compulsory input **must** answer `regionOfDefinition` itself.
The default is the union of the inputs, and the union of no inputs is empty — so
a generator that does not override it renders a rectangle of zero pixels and
looks for all the world like a plugin that failed to load.

```cpp
aofx::Rect regionOfDefinition(double, const std::vector<aofx::Rect>&,
                              const std::vector<aofx::ParamValue>& params) const override {
    // whatever the parameters ask for, at the render scale
}
```

## Extra planes

An effect may declare outputs beyond the picture — a matte, a depth, a motion
field. The idea a renderer calls an arbitrary output variable:

```cpp
into.outputs.push_back(aofx::PlaneDesc{"Color", "Colour", {"R","G","B","A"}});
into.outputs.push_back(aofx::PlaneDesc{"Clip", "Clipping matte", {"R","G","B","A"}});
```

A plane is allocated **only when somebody asks for it**
(`RenderEngine::setWantedPlanes`, driven by the viewer's Layer menu). Look it up
and cope with its absence:

```cpp
const aofx::OutputPlane* clip = request.output("Clip");
uniforms.wantClip = clip != nullptr ? 1u : 0u;
```

A kernel argument still has to be bound, so pass the picture's own buffer in its
place and let the flag stop the write. An unwritten alias is a second name for a
buffer; a written one is the picture destroyed by its own side effect.

Planes reach the file as EXR layers — `Clip.R`, `Clip.G` — in one part, which is
what every reader understands as a layer.

## Data the effect owns

`scratch(w, h)` is working space for one render. For anything that must **outlive
the frame** — a point cloud, a lookup table built on the CPU, a mesh:

```cpp
aofx::Buffer cloud = request.gpu->keep(path + ":" + modifiedTime, data, bytes);
```

Ask twice with the same key and the second answer costs nothing, so `data` may
be null once it is known to be there. Three things to know:

- **The key must identify the contents.** A path alone is not enough: the file
  changes under it.
- **One `Effect` serves every node**, and several engines render at once — the
  prefetcher has its own. Two nodes with different data and the same key get
  each other's.
- **There is no eviction.** The host cannot know whether you are about to ask
  for the same cloud again. `drop()` what you replace.

## Which node is this? `request.instance`

There is one `Effect` per plugin and not per node, and for nearly everything
that is right: a render's inputs, outputs and parameters all arrive in the
request, so the effect has nothing to remember.

Two things break that, and both arrived with the model nodes:

- **A buffer that has to outlive the render belongs to *this node*, not to the
  plugin.** `keep` is shared by key across every node of the type, so two Depth
  nodes in one graph would write into each other's working memory —
  intermittently, in proportion to how far apart their cadences had drifted,
  which is the worst way for a bug to behave.
- **Anything staggered between nodes needs to tell them apart.**

`request.instance` is the node's path through the groups — `"Grade/Depth1"` —
stable across frames and unique within a document. Key your `keep` buffers on
it. It is empty from a host that does not fill it in, and an effect that needs
one should refuse rather than share.

It changes when the node is renamed or moved between groups. A kept buffer keyed
on it is therefore lost once when that happens, and the old one is stranded
until the process ends; make the first frame after a reset produce something
sensible rather than whatever the card was holding.

## Sound

Audio rides on the picture. An effect sees the sound of
the frame it is rendering as `InputPlane::audio` on the Color plane -- a
block at the project's format, exactly `RenderRequest::audioFrames` long --
and hands back the sound of the frame it made through
`RenderRequest::setAudio`. Set nothing and the host applies
`EffectDesc::audio`: `Mix` (the default: every non-mask input's sound,
summed), `PassThrough` (the input the effect passes through), `Silent`.

An effect that only makes sound sets `audioOnly`: it gets no output buffer,
and the host hands on the input's picture under a handle of its own with
whatever sound the effect set. `Gpu::decodeAudio` gives a clip's sound by
sample run at the project's rate -- `sampleStart(k, rate, clip fps)` and
`sliceLength(k, …)` from `aofx/Audio.h` for the clip's frame *k*, stretched
to `outFrames` when the clip's rate is not the project's -- and `-1` for
"under the picture `decode`/`decodeNext` last served". `Gpu::record` takes the
block to mux beside the picture. ABI 20.

## Boxes, and which thing each one is

`aofx/Boxes.h` writes the sixty-four slot box format down once: corners in the
first row of a buffer, `(sure, score, id, 7734)` in the second, and the marker
so that a picture wired into a Boxes port is not read as coordinates. A
producer also attaches the same thing as numbers, under
`boxAttachmentId(instance)`, for everything that is not a kernel: an
expression asking where somebody is, a caption following them.

`meta.z` used to be the constant 1 and meant "this slot is filled". It now
carries the **identity** -- which is the same answer, because anything above
nought is filled and a producer with nothing to track writes 1. A tracker
writes the number it is keeping on that person, so `input.box(0).id` is a
player where `input.box(0)` is a rectangle. The attached form therefore has
seven numbers a box and not six; index by `kAttachedBoxStride` and never by a
literal. ABI 21.

## Running a network

Four more verbs, for a node whose answer comes from a model the host compiled:

```cpp
ModelId model(const std::string& name);
ModelIo modelInput (ModelId, int index) const;
ModelIo modelOutput(ModelId, int index) const;
bool    infer(ModelId, const std::vector<Buffer>& in,
                       const std::vector<Buffer>& out);
```

Named like a kernel, enqueued like a dispatch, and **it does not wait**: the
network lands on the device's queue between the kernel that fills its input and
the kernel that reads its output. Nothing crosses the bus.

`kInvalidModel` means this host has no such engine — a viewer, or a render host
that has not exported one. Refuse the render; do not produce a picture.

Set `EffectDesc::usesModel` so the graph can draw the node differently. A node
that runs a network costs tens of times what a kernel costs and takes a second
to become usable, and both are worth seeing without opening it.

Full story in [inference.md](inference.md).

## Asking the host to draw a scene

Two more verbs, since ABI 26, for a node whose picture comes from a renderer
the host owns — a path tracer, a splat rasteriser, a stage renderer — which is
the third large, stateful thing after a decoder and a network that cannot live
in a plugin, because it opens a device and the plugin never does:

```cpp
std::vector<std::string> engines() const;
EngineResult render(const EngineRequest&);
```

`engines()` names what this host has; empty is a normal answer, and a node
that needs one refuses with a sentence saying which. `render` takes an
`EngineRequest`: the engine by name, the scene as text in the host's own scene
format (the same text a scene node hands any renderer), named numbers for the
settings, the planes the engine may read, and the planes it must write — each
with a `PlaneFormat` saying what goes into the four floats of every pixel:

| format | what is in the plane |
|---|---|
| `Color` | the picture, premultiplied linear RGBA |
| `Depth` | view z in R (G and B the same), A = 1 |
| `Normal` | a world-space normal in RGB |
| `Id` | an integer per pixel, its bits in R; coverage in A |
| `Vector` | motion in RG, in pixels of this render |
| `Crypto` | a Cryptomatte layer: (id, coverage) pairs, two ranks a pixel |

A format the engine does not produce is refused by name, never reinterpreted.
Everything crosses on the device: the host binds the planes to the renderer
and the renderer's output to the planes, and the plugin never sees a handle.
Synchronous as `run` is — finished when `process` returns.

The scene text is the host's, not the SDK's: a host says what its scene nodes
write, and an engine that does not know a line ignores it.

## Doing the expensive thing only sometimes

`request.due(every)` answers "should this frame do the expensive work", and it
is four lines you should not write yourself:

```cpp
if (request.due(cadence)) {
    // run the network, fill the buffer everything below reads
}
```

It is computed from the **frame number**, so scrubbing, looping and rendering
out of order all pick the same frames. A counter kept by the effect would make
the answer depend on how many times the node happened to be asked, which is not
a property of the shot — two viewers open on the same graph would each advance
it.

And it is **staggered by a hash of `request.instance`**, which is the half that
is easy to leave out. Without the offset every node at the same cadence runs on
the same frames, and a graph with two networks pays for both on one frame in
three and neither on the other two. That halves the average and leaves the peak
exactly where it was — and it is the peak that drops frames. Measured: p90 went
16.61 to 13.44 ms on two model nodes at cadence three.

Reusing an answer unchanged makes it lag. If that shows, take a flow field as an
optional input and carry the answer forward rather than holding it still.

## What the host does for you

- **Channel switches.** Every AOFX node gets R/G/B/A switches for free; fill all
  four and the host puts the unselected ones back. Do not declare your own.
- **Caching.** Results are cached on a content hash of the plugin, its
  parameters and its inputs' hashes.
- **Premultiplied, scene-linear float, throughout.** Values above one are
  ordinary and clamping them is a decision somebody makes with a Clamp node.
- **Animation.** An effect sees a number, never a curve.
- **Groups.** A node inside a group is flattened away before the render, so
  being in one changes nothing except `request.instance`, which gains a prefix.
  A group can promote one of your parameters and drive it from outside; you see
  the value and nothing else.

## Measuring, which is not optional

The line is **40 ms** — one frame at 25 fps.

Render the same graph twice with only your node swapped for the one it
replaces, at the size you deliver, and time the node itself.

Time the reader separately and subtract it: at HD it costs more than most nodes,
and a figure including it says the same thing about everything.

**The GPU is not always faster, and the honest thing is to say so.** Measured
here: the blur is 9.0 ms against CImgBlur's 219.5 — and Colorspace is 2.5 ms
against the plugin's 2.3, *slower*, because a handful of operations per pixel
does not pay for the trip. Grouping twenty-six plugins into one node was still
worth it; the speed was not the reason.

Where a node replaces an existing one, compare them image to image and state the
number. Where they do not match, **say so in the code** rather than leaving the
claim standing — see the four HSL modes in `examples/merge/Merge.cpp`, which are
labelled as differing in the menu itself.
