# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

The **AOFX** effect SDK ("Alternative OpenFX"): a header-only C++20 plugin interface for effects whose kernels are written once in Slang and compiled at build time to PTX (CUDA) or a metallib (Metal). This repo contains the SDK, the CMake that turns a plugin into a `.aofx.bundle`, eight example plugins, and CPU tests. **No host lives here**: nothing links a host, and that is a design rule, not an omission.

## Commands

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
ctest --test-dir build -R aofx_blur_regions_tests --output-on-failure   # one test
```

Tests (`tests/CMakeLists.txt`): `aofx_sdk_tests` (every header compiled and linked the way a plugin is), `aofx_abi_tag_tests`, `aofx_abi_tag_differs` (libstdc++ only: old vs new string ABI must give different build tags), `aofx_blur_regions_tests` (the blur example's regions checked on the CPU using `examples/blur/BlurMath.h`).

CI (`.github/workflows/ci.yml`, Linux only) also checks that each header compiles on its own. Reproduce it locally with:

```sh
for h in sdk/include/aofx/*.h; do printf '#include "aofx/%s"\n' "$(basename "$h")" | g++ -std=c++20 -Wall -Wextra -Werror -fsyntax-only -x c++ -Isdk/include -; done
```

Kernel toolchain: `slangc` (looked for in `$SLANG_ROOT/bin`, `~/tools/slang/bin`, or `-DGPE_SLANGC=`), plus full Xcode on macOS or `nvcc` elsewhere (`-DGPE_CUDA_ARCH=sm_XX` to set the PTX floor). **Without a toolchain the backend is `none` and every example is skipped with a status message.** Only the CPU tests build then, which is the case in CI and most likely in this container. Kernel changes cannot be verified here; say so rather than claim they work.

Options: `AOFX_BUILD_EXAMPLES`, `AOFX_BUILD_TESTS`, `AOFX_KERNEL_REFLECTION` (default ON), `AOFX_REQUIRE_REFLECTION`. Bundles go to `build/aofx/<Name>.aofx.bundle/Contents/<MacOS|Linux-x86-64|Win64>/`.

## Architecture

**The build pipeline.**
- `cmake/AofxToolchain.cmake` finds the compilers and sets the `GPE_*` variables, plus `AOFX_HAVE_KERNELS` and `AOFX_ARCH_DIR`. The `GPE_` prefix is shared with the host's compute engine ("gpe").
- `sdk/cmake/AofxKernel.cmake` (`aofx_add_kernel`) uses those variables and never searches for compilers itself, so a host can supply them. It compiles `<name>.slang` to a blob. At build time a script then appends the reflection trailer (`sdk/cmake/AofxKernelTrailer.cmake`, magic `GPEK`) and generates `aofx_kernels_<name>.h` with `k_<name>` / `k_<name>Bytes`.
- `cmake/AofxPlugin.cmake` (`aofx_add_plugin`) builds a MODULE target `aofx_<lowercase name>` that links only `aofx::aofx` (the INTERFACE target in `sdk/CMakeLists.txt`). It also copies `Info.plist` on Apple.
- Everything under `sdk/` must stay usable standalone inside a host's build. Keep the split: toolchain discovery belongs in `cmake/`, the SDK's own files in `sdk/cmake/`.

**The plugin ABI.** `AOFX_EXPORT_EFFECTS(...)` (`Entry.h`) writes four `extern "C"` entry points: ABI version, build tag, effect count, and effect by index. The interface is C++ across the boundary (vtables, `std::string`, `std::vector`). Two things guard it:
- `kAbiVersion` in `sdk/include/aofx/Version.h` is currently 25. It is bumped whenever **anything in the headers changes shape**, and each bump adds a line to the history comment above it.
- `buildTag()` catches toolchain and standard-library mismatches.

When you change shape: bump `kAbiVersion`, add its history line, add a newest-first entry to `docs/HOST_CHANGES.md` (what a host must change and how to check it worked), and update the ABI number in `README.md`.

**An effect.** One `Effect` instance serves every node of that plugin, so an effect holds no per-node state. Per-node data arrives in `RenderRequest`, and anything that must be per-node and persistent is keyed on `request.instance`. The virtuals are `describe`, `kernels`, and `process`, plus the optional `isIdentity`, `regionOfDefinition`, and `regionOfInterest`. `Effect.h` is the core; the other headers are feature areas (Boxes, Tracks, Audio, Planar, Transform, Shape, Delivery, Features, Descriptor).

## Rules that bite (from `docs/aofx-sdk.md`, read it before writing a plugin or kernel)

- The C++ uniform struct must match the Slang parameter struct **byte for byte**. Use 4-byte members in declaration order, add a `static_assert(sizeof(...) == N)`, and never put `float x[N]` in a constant buffer (use `float4`).
- Buffer bindings are positional per *file*. Every entry point in a multi-entry blob must be handed all of the file's buffers, in declaration order.
- `Grid` counts threads, not groups. The group shape is the host's choice (flat 256 on CUDA, 16×16 on Metal), so never index with `SV_GroupThreadID`/`SV_GroupID` or use `groupshared` for reductions. See `statsRow`/`statsFold` in `examples/generate/imagestats.slang`.
- Buffers carry a `rect`. Relate source and destination offsets and never assume same-size in and out.
- Generators must override `regionOfDefinition`. Moving and spreading effects must answer `regionOfInterest`.
- `outputRod` is the whole picture; `renderWindow` is the part being rendered.
- Choice indices are permanent. Appending is fine; reordering is not.

## Conventions

- Comments and docs are prose-heavy and explain *why*, often with the bug that motivated a rule. Match that voice and spelling (British: "colour", "licence").
- Example identifiers use `org.aopenfx.<name>`. `KernelDesc` takes a globally unique kernel name separate from the Slang entry-point name.
- Adding an example means creating `examples/<name>/` with `CMakeLists.txt`, `.cpp`, `.slang`, and `Info.plist`, then adding it to the list in `examples/CMakeLists.txt` and the README table.
- Commit subjects are short and descriptive, prefixed by area (`sdk:`, `docs:`, `CI:`, or an example name). Roadmap and review items are cited as `(P2-1)` and similar.
