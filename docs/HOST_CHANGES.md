# Changes a host has to make

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
