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

## The reference host lives here: `host/`

There is now one host, and it is in this repository beside the SDK. A program
that loads AOFX bundles adds `host/` after gpe and after `sdk/` and links
`aofx::host`; what it brings -- a logger, a media stack, an inference runtime,
a rule for identifiers in an older spelling -- it declares in an
`aofx::host::Capabilities` it keeps alive for the process. A verb the program
declared nothing for answers "not available in this host, because it declares
no <media|model> capability", from this code and not from a copy of it with
the verb cut out.

- **`kAbiVersion` does not move.** This is a move, not a change of shape: no
  struct, vtable or entry point is different, and a bundle built before it
  loads after it.
- **A program with a host of its own retires it**: the registry (discovery,
  the ABI and build-tag gates, kernel registration), the runner's device
  verbs (load, run, scratch, keep, drop, borrow, importFd, read, publish),
  the channel-restore kernel and the Apple page wrapping are `aofx::host`'s.
  The program keeps its translation of `aofx::EffectDesc` into whatever its
  menus speak, and moves the bodies of its media and model verbs behind
  `MediaBackend` and `ModelBackend`.
- **`run` checks a dispatch against the kernel's reflection** (buffer count,
  uniform bytes) when the blob carries a trailer, and refuses by name. A
  program whose kernels were built without one sees no change.
- **The channel-restore kernel is `aofx.host.channels`** (entry
  `channelsMain`), a neutral name; a program that dispatched it under a name
  of its own uses `aofx::host::kChannelsKernel` now.
- `aofx_add_kernel` gained an optional `SOURCE <file>`, for a kernel that
  lives under a subdirectory. Nothing in the headers moved.

Check: two programs built with the same toolchain list the same bundles with
the same build tag, and a bundle refused in one is refused in the other with
the same sentence.

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
