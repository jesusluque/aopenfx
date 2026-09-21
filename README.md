# aopenfx

The **AOFX** effect SDK: a plugin interface for effects that run on the GPU
through Metal or CUDA, and a build that makes a plugin bundle with nothing but
the SDK.

This is **AOFX ABI 25** (`sdk/include/aofx/Version.h`). A bundle loads only in
a host built against the same ABI.

## What is here

| Path | What |
|---|---|
| `sdk/include/aofx/` | The SDK: C++20 headers, nothing to link |
| `sdk/cmake/AofxKernel.cmake` | `aofx_add_kernel()`: a Slang kernel compiled and embedded in the plugin |
| `host/` | The reference host: loads bundles, answers `aofx::Gpu` on gpe; a program declares what it brings (`Capabilities`) |
| `cmake/AofxToolchain.cmake` | Finds slangc and the Metal or CUDA compilers for the kernels |
| `cmake/AofxPlugin.cmake` | `aofx_add_plugin()`: a plugin as a `.aofx.bundle` |
| `examples/` | Eight plugins that need only the SDK |
| `tests/test_sdk_headers.cpp` | The headers, compiled and linked the way a plugin is |
| `docs/aofx-sdk.md` | The SDK reference |
| `docs/HOST_CHANGES.md` | What a host implementing the SDK must change, per version |
| `docs/ROADMAP.md` | Designed or wanted, not scheduled |

The examples, and what each shows:

| Example | Shows |
|---|---|
| `invert` | the smallest plugin |
| `crop` | region of definition and region of interest |
| `blur` | separable passes, radius at render scale |
| `grade` | colour parameters |
| `merge` | two inputs, a Choice |
| `transform` | Position and Angle parameters |
| `cornerpin` | two entry points in one blob; corners from an attachment; two effects in one bundle |
| `generate` | generators with no input; two kernel files |

## Requirements

- CMake 3.24 and a C++20 compiler.
- **slangc**, from a release at github.com/shader-slang/slang. It is looked for in `$SLANG_ROOT/bin` and `~/tools/slang/bin`, or pass `-DGPE_SLANGC=<path>`.
- **macOS:** full Xcode, not only the Command Line Tools, for `metal` and `metallib`.
- **Linux:** the CUDA toolkit, for `nvcc`. The PTX is built for the card this machine has; pass `-DGPE_CUDA_ARCH=sm_XX` to choose the oldest card the bundles must run on.
- **Windows:** CUDA as on Linux. *Not verified yet.*

### Tested with

| | macOS | Linux |
|---|---|---|
| OS / GPU | Apple silicon, Metal | Ubuntu 24.04, x86-64, NVIDIA L4 (driver 580) |
| Compiler | Apple clang 21.0.0 | GCC 13.3.0 |
| Kernel toolchain | slangc 2026.14.1, Xcode 26.6 | slangc 2026.14.1, CUDA 12.0 (nvcc) |
| CMake | 4.4.1 | 3.28.3 |

On both, all eight example bundles build, load in a host and render.

**Windows is not verified.** The CMake is prepared for it (CUDA, `Win64` bundle
folder, `__declspec(dllexport)` from `AOFX_EXPORT_EFFECTS`), but nothing has been
built or run there yet.

**CI** (GitHub Actions, Linux) builds every header on its own and runs the CPU
tests: the header test, the build-tag test (including libstdc++'s old and new
string ABI) and the blur regions test. It has no GPU toolchain, so the examples'
kernels are not built there.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The bundles land in `build/aofx/<Name>.aofx.bundle/Contents/<MacOS|Linux-x86-64|Win64>/`.

Every kernel carries a reflection trailer: its thread-group size and the byte sizes of its buffers and uniform block, which a host uses to refuse a dispatch that disagrees with the kernel instead of rendering black. `-DAOFX_KERNEL_REFLECTION=OFF` leaves it out; `-DAOFX_REQUIRE_REFLECTION=ON` turns that into a configure error.

## Load in a host

```sh
AOFX_PLUGIN_PATH=$PWD/build/aofx <your host's plugin listing>
```

`AOFX_PLUGIN_PATH` is searched first. A host that already ships the same identifiers keeps the first bundle it finds and says it skipped the other.

## A plugin of your own

```cmake
list(APPEND CMAKE_MODULE_PATH <aopenfx>/cmake <aopenfx>/sdk/cmake)
include(AofxToolchain)
add_subdirectory(<aopenfx>/sdk aofx)
include(AofxPlugin)
aofx_add_plugin(MyEffect SOURCES MyEffect.cpp KERNELS "myeffect ENTRY main")
```

Read `docs/aofx-sdk.md` first. `examples/invert` is the shortest complete plugin.

## Licence

BSD 3-Clause, the same licence as OpenFX. See `LICENSE`.
