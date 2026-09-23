# An OpenFX plugin that runs AOFX effects

A design, not an implementation. Nothing here has been built or run.

## What it is

One `.ofx.bundle` that is an ordinary OpenFX plugin to the host and an AOFX
host to the `.aofx` bundles it loads. Every AOFX effect it accepts appears in
the OpenFX host as a node of its own, and those nodes behave like any other:
each renders a real RGBA picture into its output clip, so the viewer, the
host's cache and the rest of the host's tools see nothing unusual.

What the host does not see is a second channel, **aofxData**, that travels
beside the picture from one AOFX node to the next. It carries what an AOFX
effect produces and OpenFX has no word for: boxes and their identities, the
extra planes, the keys of buffers already on the device. The chain ends in a
**DeliveryOFX** node, which reads the channel one last time, turns what it
holds into OpenFX's own structures, and ends the chain.

```
Read ─▶ [AOFX Detect] ─▶ [AOFX Track] ─▶ [AOFX Grade] ─▶ [DeliveryOFX] ─▶ the rest of the OpenFX graph
            RGBA            RGBA            RGBA            RGBA
            + aofxData ───▶ + aofxData ───▶ + aofxData ───▶ (channel ends here)
```

## Loading: one OpenFX node per AOFX effect

The host asks an OpenFX binary how many plugins it holds
(`OfxGetNumberOfPlugins`) and for each one (`OfxGetPlugin`). The bridge
answers from what it found on disk, so every AOFX effect has its own node,
with its own name, parameters and inputs, in the host's menus.

**Where it looks.** `AOFX_PLUGIN_PATH` first, as an AOFX host does, then a
folder inside the bridge's own bundle (`Contents/Resources/aofx`). The
second is the one that matters in practice: an application started from the
Dock or a desktop launcher does not inherit a shell's environment, and a
bridge that finds nothing there lists nothing.

**What it checks, in the order an AOFX host does.** Each `.aofx` is opened
and kept open for the life of the process. `AofxGetAbiVersion` must equal
the bridge's `kAbiVersion` and `AofxGetBuildTag` its `buildTag()`: the bridge
is the AOFX host here, so it is built with the same toolchain as the bundles
it loads. Then `AofxGetEffectCount` and `AofxGetEffect(i)`, and `describe`
on each. A bundle that fails any step is left out and the reason is logged,
never loaded half.

**Which effects make the list.** The subset is decided at load, from the
`EffectDesc`, so that a node the bridge cannot run never appears:

| `EffectDesc` says | Listed? |
|---|---|
| An image effect | Yes |
| `audioOnly` | No: OpenFX has no sound |
| `offline` | No: it is a delivery walked by the host's batch, which OpenFX does not have |
| `usesModel` | Only if the bridge carries an inference runtime |

What only shows at render time -- a `decode` by path, a channel the host
cannot carry -- refuses then, with a message.

**What each node is called.** The AOFX identifier, unchanged. `Descriptor.h`
puts AOFX identifiers in the same reverse-DNS space as OpenFX on purpose, so
a node saved in a project keeps working if the effect is ever built as a
native OpenFX plugin. `versionMajor`/`versionMinor` become the OpenFX
plugin version, `label` the name, `grouping` the menu under an `AOFX/`
prefix. Two bundles with one identifier: the first found wins and the other
is logged as skipped, as in an AOFX host. The DeliveryOFX is the bridge's
own, always first in the list.

**One entry point per node.** An OpenFX plugin's `mainEntry` receives no
pointer back to its plugin, so a single function cannot tell which effect
it is serving. The bridge carries a fixed table of entry points generated
by a template, `mainEntry<0>` to `mainEntry<N-1>`, and hands the i-th to
the i-th effect. `N` is the most effects one bridge can list; beyond it,
effects are logged as skipped.

**The host keeps its own list.** Hosts usually cache what an OpenFX binary
described and reuse it while the binary is unchanged. A new `.aofx` does
not change the bridge's binary, so a host may not notice it until its
cache is cleared or the bridge is touched. Installing an effect has to say
so.

## The rule that keeps it simple: an AOFX chain is not interrupted

AOFX nodes connect to AOFX nodes, from the first one to the DeliveryOFX. No
OpenFX node sits in between. That is a rule of use, not something the
bridge can enforce, because an OpenFX effect is never told what it is
connected to. So the bridge checks it instead of trusting it:

- The first node of a chain has a plain OpenFX picture as its input and no
  aofxData. That is how it knows it is first.
- Any other node that finds no aofxData, or finds one whose header does not
  check (magic, version, checksum), refuses the render and says so in the
  host's message, naming the rule. It does not quietly start a new chain:
  a chain cut in two would still render, and everything that travelled in
  the channel -- boxes, identities, kept buffers -- would be gone without a
  word.

## The aofxData channel

**Where it lives.** OpenFX gives an effect one output clip, so a second
channel has to be a second *plane* of that clip. The plane suite that does
this is an extension (`FnOfxImageEffectPlaneSuite`, implemented by Nuke and
Natron) and not part of the standard. A host without it cannot carry the
channel, and the bridge runs there in RGBA-only mode (below).

**The catch, with GPU rendering required.** The hosts known to implement
the plane suite (Nuke, Natron) are not the ones known to render OpenFX on
CUDA or Metal (Resolve is), and a host needs both for the full design. This
has to be checked host by host before anything is built; if no target host
has both, aofxData needs another carrier -- one that travels in the device
memory the host already passes between nodes, with no plane suite behind it.

**What it holds.** A small picture, not a frame-sized one: a header and a
payload, written as float texels.

| Row | Contents |
|---|---|
| 0 | Magic, format version, checksum, the chain's id, the producing node's `instance`, the frame |
| 1.. | Entries: a kind, a key into the bridge's registry, and inline data for what is small |

Small, frequent things travel **inline**: the sixty-four-slot box format of
`aofx/Boxes.h` is already two rows of texels and needs no translation.
Large things travel **by key**: an extra plane or a kept buffer stays on the
device, in the bridge's registry, and the channel carries only its key. The
next node finds it there without a copy.

A float holds integers exactly up to 2²⁴, so a 64-bit key is spread across
four channels of one texel, sixteen bits each.

## The registry

Every node of the bridge lives in the same binary and so in the same process
(this must be confirmed for each target host: a host that runs plugins out of
process breaks it). They share one AOFX engine -- the bridge's implementation
of `aofx::Gpu` -- and one registry of device buffers.

- **Keys identify contents**, as `keep` keys do: node instance, frame,
  and a hash of the parameters and of the input keys. A host that answers
  from its cache without calling a node's render still hands on an aofxData
  whose keys are valid, as long as the registry has not evicted them.
- **Eviction is least recently used, under a budget.** A key that is gone
  is not an error: the node that needs it asks for its input again, which
  re-renders the upstream node, which puts it back. Slower, never wrong.
- **The picture is not in it.** RGBA travels in the host's own device
  memory (see "Pixels never leave the device"), so the next node reads its
  input where the host put it. The registry holds only what the host has
  no clip for: extra planes and kept buffers.

## `request.instance`

OpenFX has no stable node path. Each node carries a hidden string parameter
holding a UUID, generated when the instance is created and saved with the
project. That is `request.instance`. Unlike AOFX's own path it does not
change when the node is renamed, so a kept buffer keyed on it is not
stranded (see "The life cycle of what `keep()` holds" in `ROADMAP.md`).
`kOfxActionDestroyInstance` tells the bridge when to drop what that
instance kept.

## Each node, action by action

| OpenFX action | AOFX node | DeliveryOFX |
|---|---|---|
| Describe | Clips from `EffectDesc::inputs`, parameters from `EffectDesc::params`, the aofxData plane declared | One input, one output; a Choice of what to deliver |
| Get region of definition | `regionOfDefinition` | The input's |
| Get regions of interest | `regionOfInterest` | The input's whole picture |
| Is identity | `isIdentity`; aofxData still passes on | Never |
| Render | Read the input's aofxData, run `process` on the device, write RGBA and aofxData | Read aofxData, write the chosen output as RGBA |

The R/G/B/A switches every AOFX node gets are the bridge's to implement:
four parameters, and one kernel that puts back the unselected channels.

## Gizmos: the bridge draws them, the effect never does

An AOFX effect cannot draw, and the bridge keeps it that way. The effect says
what its numbers *are* (`ParamRole`, `ParamType::Shape`); in an AOFX host the
host draws the handle, and here the bridge is the host. So every node that
has something to draw gets an OpenFX overlay interact
(`kOfxImageEffectPluginPropOverlayInteractV1`) written once, in the bridge,
and driven by the node's `EffectDesc`. The `.aofx` bundle contains no
drawing code and learns nothing new.

**How it draws.** With the Draw Suite (`ofxDrawSuite.h`, OpenFX 1.5) where
the host has it: lines, polygons, points and text, asked of the host, with
no OpenGL linked into the bridge. Where it does not, with OpenGL on the
context the host makes current for the draw action. Coordinates are
canonical; `kOfxInteractPropPixelScale` sizes handles in screen pixels so a
handle is the same size at any zoom.

**How it edits.** Pen down, motion and up (`kOfxInteractActionPenDown`,
`PenMotion`, `PenUp`) set the parameter the handle stands for, between
`paramEditBegin` and `paramEditEnd` (`OfxParameterSuiteV1`), so one drag is one undo step and an
animated parameter gets a key where the host would put one.

| The effect declares | The bridge draws | And a drag sets |
|---|---|---|
| `ParamRole::Position` (Double, 2) | A point | Both numbers, in pixels |
| `ParamRole::Angle` | A ring about the node's Position, or the frame's centre when it has none | Degrees |
| `ParamRole::Scale` | A radial handle on the same ring | The multiplier |
| `ParamType::Shape` | The outlines, their points, tangents and feather, as `aofx/Shape.h` lays them out | The numbers in that layout |
| `ItemCount` / `ItemIndex` | Only the handles of the item being edited | -- |
| Boxes in the input's aofxData | Rectangles with their `id`, to look at, not to edit | -- |

Three declarations the bridge honours exactly as an AOFX host does:

- **`ShownWhen` hides the handle with the row.** Eight points on the picture,
  six of which the node is ignoring, is worse than no handles.
- **`defaultsNormalised`** is `kOfxParamPropDefaultCoordinateSystem` set to
  normalised, so a default of `{0.5, 0.5}` is the centre at any format and
  the ring is drawn where the render turns.
- **`Angle` without a range** gets a display range in degrees, so its
  slider can reach one.

**Shapes are the expensive one.** A point is one parameter; a shape is a
list of them with tangents and feather, and its editor -- add a point,
break a tangent, drag a feather -- is most of a roto tool. It comes after
the point, ring and boxes, and until then a Shape parameter is edited as
its numbers.

**Boxes are drawn from numbers the producer already attached.** The overlay
does not read the device while it draws. A producer already reads its boxes
back once a frame for everything that is not a kernel (`attachedBoxes`, 512
bytes, see `aofx/Boxes.h`); the bridge keeps those numbers in the registry
under the instance and the frame, and the overlay draws from them. That is
coordinates for a drawing, not pixels: no picture is processed off the
device.

Where a host draws its own handle for an `XYAbsolute` parameter, a node would
get two. The bridge declares its positions as plain doubles in that host and
draws them itself, so every AOFX node has the same gizmo in every host.

## What DeliveryOFX gives back

| In the channel | Out as OpenFX |
|---|---|
| The picture | RGBA, float, premultiplied, as every OpenFX node expects |
| An extra plane (Clip, depth…) | Chosen with the node's Choice, or as planes where the host has the plane suite |
| Boxes | As the picture of `aofx/Boxes.h`, which is already an image; or drawn over the picture for review |
| Tracks, kept buffers | Nothing: they belong inside the chain |

Parameters cannot be written during a render in OpenFX, so nothing measured
in the chain comes back as a parameter value.

## What the bridge does not carry

- **Sound.** OpenFX has no audio; an effect's `setAudio` is dropped.
- **`decode` of a clip by path.** The host owns reading files.
- **`infer`**, unless the bridge ships its own inference runtime.

## RGBA-only mode

In a host without the plane suite, each node is a plain bridge: RGBA in,
RGBA out, no channel. Effects that need nothing but the picture work
exactly as in a host with the suite. Effects that need the channel -- a
tracker reading a detector's boxes -- say that they cannot run here and
refuse, rather than render without it.

## Pixels never leave the device

AOFX does not work on the CPU, and the bridge has no CPU path: no upload, no
download, no fallback.

- **The host must render on the GPU.** Each node declares OpenFX 1.5 CUDA
  or Metal rendering (`kOfxImageEffectPropCudaRenderSupported`,
  `kOfxImageEffectPropMetalRenderSupported`) and nothing else. The host
  hands it device memory and its own stream or queue; the bridge loads the
  kernels in that context and dispatches on that stream, so nothing waits
  and nothing crosses the bus.
- **A render the host offers on the CPU is refused**, with a message that
  says the node needs GPU rendering and where the host turns it on. A node
  that quietly copied to the device and back would render, and would be
  the slowest node in the graph with nothing to say why.
- **A host without GPU rendering for OpenFX does not get the bridge.** It is
  a host the bridge is not for, in the same way a host without the plane
  suite gets no aofxData.
- **One device.** The bridge's engine runs on the device and context the
  host gives it. A host that hands two nodes buffers on two different
  devices is refused rather than bridged with a copy.

## Open questions

- Which hosts: GPU rendering for OpenFX decides whether the bridge runs at
  all, and the plane suite whether the channel exists. A target host needs
  both, and none is known yet to have both.
- Whether each target host loads a plugin's nodes in one process.
- How each target host caches plugin descriptions, and what makes it look
  again after a `.aofx` is added.
- The registry's budget, and whether it follows the host's own memory
  settings.
