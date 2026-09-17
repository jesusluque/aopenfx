# Changes a host has to make

## The gpe checkout for kernel trailers is `AOFX_GPE_DIR`

`sdk/cmake/AofxKernel.cmake` now reads `AOFX_GPE_DIR` to find gpe's
`KernelTrailer.cmake`, and falls back to the host's own name for it only when `AOFX_GPE_DIR` is unset.

- **Set `AOFX_GPE_DIR`** in the host's CMake to the same gpe checkout, before
  its plugins call `aofx_add_kernel`. Nothing breaks until the fallback is
  removed in a later version.

Check: the host's configure log, or the size of a generated
`aofx_kernels_<name>.h`, shows the trailer is still appended.

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
