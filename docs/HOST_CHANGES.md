# Changes a host has to make

## Kernel reflection trailers come from the SDK

`sdk/cmake/AofxKernel.cmake` appends the reflection trailer with the SDK's own
generator (`sdk/cmake/AofxKernelTrailer.cmake`), on by default. It no longer
looks for a gpe checkout to find one. The trailer's bytes are unchanged:
same layout, version 1, magic `GPEK`.

- **Nothing to set.** A host whose build passed a gpe directory only so kernels
  would carry the trailer can stop; `AOFX_KERNEL_REFLECTION` (default ON)
  controls it now.
- **The host still reads the trailer** exactly as before.

Check: a generated `aofx_kernels_<name>.h` is byte-identical to the one built
with the previous generator.

## `RenderRequest::outputRod` is the whole picture, not the render window

Found while verifying the blur example: a host that sets
`request.outputRod = request.renderWindow` breaks every effect that needs to
know where the picture ends. `Effect.h` documents the two as different: the
window is the part being rendered, the output RoD is the whole picture that
window belongs to. When they are the same rectangle, a pattern generator draws
its pattern across whatever was asked for -- a blur downstream that asks for a
margin moved every colour-bar boundary, and a tiled render would draw a full
set of bars per tile.

- **Set `outputRod`** to the node's region of definition at the render scale,
  intersected with whatever the host clips to (the project format), and
  `renderWindow` to the part being rendered.
- The generator examples no longer rely on it (they work out their frame from
  their own region), but other effects may.

Check: render a ColorBars node alone, and again under a blur of size 8; the bar
boundaries in the blurred picture are where they are in the unblurred one.

What changed in the SDK that a host implementing it must follow. Newest first.
Each entry says what to change and how to tell it worked.

## ABI 26 — gizmos

`EffectDesc::gizmos` (`sdk/include/aofx/Descriptor.h`) and `aofx/Gizmo.h`: an
effect declares any viewer handle, on the picture or in the 3D scene, as
primitives bound to its parameters, and the host draws them. The contract is `docs/gizmos.md`. `kAbiVersion` is 26.

- **Rebuild the host and every bundle.** `EffectDesc` grew a member; a bundle
  built against 25 is refused.
- **`ShownWhen` moved** from `Descriptor.h` to `Types.h`. Same struct, same
  namespace; code that includes either header is unchanged.
- **Draw the eighteen kinds** and offer the handles `docs/gizmos.md` lists for
  each, in the five spaces, converting through the viewer's own mapping.
- **3D gizmos** (`World` space, or placed in a `Frame3D`): draw them in the 3D
  view if the host has one, and over the picture through their `Camera` with
  `aofx::gizmo::projectToPicture`, clipped at the camera's plane. Build frame
  matrices with `gizmo::frame3DMatrix`/`frameMatrix`, never by hand. Drag them
  as the constraint table says, using `gizmo::rayFromPicture` over the picture,
  and leave the value alone when the ray misses its plane. A Frame3D ring
  changes only its own component of `rotate`; a `matrix` slot is never
  written.
- **Attachments on an input**: a binding with `clip` reads the values arriving
  at that input (`InputPlane::values`) of the frame on screen, not the node's
  output.
- **Turn a drag into parameter edits**: invert the frames and each binding's
  `scale`/`offset`, clamp to `hardMin`/`hardMax`, round Integers, one undo
  step per gesture named after the gizmo, nothing written for read-only
  bindings, attachments, constants or locked parameters.
- **Suppress role handles** for parameters bound by a gizmo you draw, and only
  those: a skipped gizmo suppresses nothing.
- **Visibility**: `visibility`, `shownWhen`/`shownAlso`, hidden rows, and the
  parent frame's rules, as the document says; `repeat` over an `ItemCount`
  pool with `{i}` from one.
- **Drawings**: read the attachment of the frame on screen with
  `aofx::gizmo::decodeDrawing`, passing the gizmo's dimensions
  (`aofx::gizmoDimensions`); draw nothing when it refuses.
- **Validate at load** with `aofx::checkGizmos`; skip what it names and report
  each once. An enumerator you do not know is skipped the same way, never
  drawn as something else and never a reason to refuse the bundle.

Check: the Crop example shows one dashed box and no loose corner handles, and
dragging a corner is one undo step that changes `corner1` or `corner2`; the
CornerPin example shows a quad whose corners drag the four `corner` parameters,
and, with a tracker on its Track input, a dashed quad where the tracker puts
them. For 3D: a point at the world position of a solved camera's look-at
target lands on the same pixel over the picture and in the render.

## ABI 25 — the build tag carries the standard library's ABI

`aofx::buildTag()` (`sdk/include/aofx/Version.h`) now includes the standard
library's ABI switch (`_GLIBCXX_USE_CXX11_ABI`, `_LIBCPP_ABI_VERSION`, or
`_ITERATOR_DEBUG_LEVEL` and debug/release on MSVC), `_MSC_FULL_VER` on MSVC, and
the sizes of `std::string`, `std::vector<int>`, `std::function<void()>` and a
pointer. `kAbiVersion` is 25.

- **Rebuild the host against these headers.** The host compares the tag a
  bundle returns from `AofxGetBuildTag()` with its own `aofx::buildTag()`, and
  every tag written by an ABI 24 build is now different text: a host still on
  24 refuses every bundle built on 25, and a host on 25 refuses every bundle
  built on 24. That is the intended outcome, but it is all of them at once.
- **Rebuild every bundle** the host loads, including ones built outside this
  repository.
- **Nothing else moves.** No struct, vtable or entry point changed shape; a host
  that only compares the tag and the ABI number needs no code change.
- **If the host shows the refusal to a person**, the new tag is longer and says
  more; it is worth showing both tags in full rather than truncating them.

Check: the host lists every bundle it loaded with no "different build" or ABI
refusal after the rebuild.
