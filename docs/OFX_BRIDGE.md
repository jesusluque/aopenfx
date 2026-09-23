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
- **The picture on the device is kept too.** Each node's RGBA result is
  registered under its key before it is copied out for the host. The next
  node checks the key in its input's aofxData and, if the buffer is still
  there, uses it rather than uploading the host's copy. The host gets its
  picture; the chain never pays to bring it back.

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

## Where the pixels come from

- **The host's device memory**, where the host offers OpenFX 1.5 CUDA or
  Metal rendering: the bridge dispatches on the host's stream or queue and
  nothing is copied.
- **Host memory otherwise**: the first node uploads, each node downloads its
  RGBA for the host, and the registry spares every node but the first the
  upload.

## Open questions

- Which hosts: the plane suite decides whether the channel exists at all.
- Whether each target host loads a plugin's nodes in one process.
- How each target host caches plugin descriptions, and what makes it look
  again after a `.aofx` is added.
- The registry's budget, and whether it follows the host's own memory
  settings.
