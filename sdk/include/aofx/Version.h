// Copyright (c) 2026 aopenfx contributors.
//
// AOFX — an alternative to OpenFX, for effects that run on the GPU.
//
// WHAT IT IS FOR
//
// OpenFX is a C ABI designed around host-allocated pixel buffers. Its GPU
// rendering suite (ofxGPURender.h) covers OpenGL and, since OpenFX 1.5, CUDA
// and Metal -- where both the host and the plugin implement them. An effect
// that uses those still writes its kernel once per backend and manages the
// device API itself.
//
// AOFX is a smaller contract. An effect declares its kernels in Slang, they
// are compiled at build time to PTX and to a metallib, and the host runs them
// on whichever device the machine has, owning the buffers, the dispatch and
// any inference. One filter source, both backends, no device API in the
// plugin.
//
// THE VERSION HANDSHAKE, AND WHY IT IS TWO NUMBERS AND A STRING
//
// The interface is C++ -- virtual functions, std::string, std::vector across
// the boundary. That is a deliberate choice and it has a price: a plugin built
// with a different compiler or standard library than the host will link, load,
// and then corrupt memory in ways that look like anything but the real cause.
//
// So a bundle states what it was built against and the host refuses anything it
// does not recognise. Refusing is the feature. A plugin that will not load and
// says why is a plugin somebody can fix; one that loads and misbehaves is a bug
// report about the wrong thing entirely.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace aofx {

/// Bumped whenever anything in these headers changes shape.
///
/// Not a semantic version: there is no "compatible minor" here, because a
/// vtable layout is compatible or it is not. One number, and a mismatch is a
/// refusal.
/// 2: `Gpu::publish` -- a new virtual on the host interface. A bundle built
/// against 1 would call through a vtable one slot short of the host's.
/// 3: `Gpu::recorder`/`record`/`closeRecorder` -- writing a movie file, the
/// mirror of the clip verbs. Three more slots, same argument. The request also
/// gains the project's rate and whether this render is a delivery, which a
/// node that writes files cannot do without.
/// 4: `ParamDesc::text` -- a String parameter can say it holds a path, and
/// get the chooser button the built-in nodes have always had. A struct the
/// plugin fills and the host reads, so its shape is as much of an agreement
/// as a vtable is.
/// 5: `ParamType::Shape` -- a parameter that is a list of closed shapes, and
/// `aofx/Shape.h` beside it saying how one is laid out inside the numbers a
/// `ParamValue` already carries. No new type crosses the boundary; what is new
/// is an agreement about the contents of an array, which is as much of a
/// contract as a vtable and is version-stamped in its own first slot for the
/// case a vtable cannot cover -- a document older than the reader.
/// 6: `ParamDesc::shownWhen` -- a control can say which other control decides
/// whether it is worth showing. A member in the middle of a struct the plugin
/// fills and the host reads, so every field after it moves: a bundle built
/// against 5 hands the host a `ParamDesc` whose hints, choices and defaults are
/// all at the wrong offsets. Forgetting to bump this is what took the render
/// host down in a restart loop rather than refusing one stale bundle, which is
/// the entire argument for the number existing.
/// 7: `ParamDesc::parent` and `ParamType::Group` -- a control can sit under a
/// heading instead of in a flat list. The member moves every field after it,
/// like 6 did; the enumerator does not, and is bundled here rather than
/// spending a number of its own.
/// 8: `Gpu::inferShaped` -- a network exported with dynamic dimensions can be
/// given a canvas. A new virtual on the host interface, same argument as 2.
/// 9: `ClipDesc::wantsPlanes` -- an input can ask for every plane of what
/// feeds it. A member of a struct the plugin fills, same argument as 6.
/// Still 9 with `aofx/Transform.h` added: inline functions only, nothing
/// crosses the boundary, so a bundle built without the header is a bundle
/// built against the same interface.
/// 10: `Gpu::read`, a buffer back to the CPU -- for the small answers a model
/// leaves in one (a pose, a count), never a picture. Every plugin rebuilt.
/// 11: `RecorderDesc::depth` and `::pixelAspect`, and `RenderRequest::
/// pixelAspect` beside the rate. Members appended to two structs the plugin
/// fills and the host reads -- the same argument as 6, and the reason a Write
/// of ours could not yet say what the built-in one always could: half against
/// float, and the aspect that goes in the header.
/// 12: `ParamDesc::shownAlso` -- a second condition on a row, both of which
/// must hold. A member in the middle of a struct the plugin fills, so every
/// field after it moves: the same argument as 6, which is the member it sits
/// next to.
/// 13: `ParamRole::ItemCount` and `::ItemIndex` -- an effect can say that N
/// slots are a list, and get one instead of two spin boxes. Enumerators, which
/// move no member; bundled with a number of their own anyway because the host
/// now behaves differently for a bundle that uses them, and "same interface,
/// different behaviour" is exactly what the number is for.
/// 17: `ParamDesc::startsDelivery` -- a button can say it means "run this node
/// now" rather than "set a flag for later". A member in the middle of a struct
/// the plugin fills, so every field after it moves: the same argument as 6.
/// Without it a delivery node's button did nothing visible, because the work
/// it arms happens in a delivery the operator had to know to go and start.
/// 14: `RenderRequest::analysing` and the range it is walking. Members
/// appended to a struct the plugin fills and the host reads -- the same
/// argument as 6 -- and the reason an effect with state can exist at all: the
/// ordinary render path prefetches and drops frames, so this is where the host
/// promises a range in order, once each.
/// 15: `ClipDesc::hostWired` -- an input the host fills by itself, which is
/// therefore not a port. The 3D renderer's four texture registers are the
/// case: the engine walks the scene, renders each piece of geometry's picture
/// and binds it, and nobody wires one by hand. Drawn as ports they left the
/// Scene with nowhere to go.
/// 16: `EffectDesc::offline` -- an effect whose work happens only in a
/// delivery. A member appended to a struct the plugin fills and the host
/// reads, and a host that now treats such a node as a delivery target and
/// waits on the job it reports; a plugin built against 15 is read as
/// `offline = false`, which is what it was.
/// 18: `ParamDesc::stampsFrameInto` -- a button can name the Integer
/// parameter the host writes the current frame into when it is pressed: a
/// tracker's "Start again" on frame 20 starts from frame 20. Appended to the
/// struct, so a plugin built against 17 reads as empty.
/// 19: `Effect::regionOfDefinition(const RegionQuery&)` -- the region
/// question carries a way to ask the host how big a clip is, so a source
/// can answer with its picture's own rectangle instead of the project's.
/// A 1300-square PNG declared 1920 by 1080 for a year, and every Crop,
/// Transform and gizmo above it was placed against a box that was not
/// there. A new virtual appended to the class, defaulting to the old
/// question; a plugin built against 18 is asked the old question. And
/// `InputPlane::rod`, the whole picture an input buffer is a piece of,
/// appended to the struct: a Transform pivoting on "the middle of the
/// picture" pivoted on the middle of whatever piece it had asked for. And
/// `regionOfInterest(const RegionQuery&, const Rect&)`, the region of
/// interest with the render's scale in it, for an effect whose knobs are in
/// canonical pixels and whose rectangles are not.
/// 20: sound. `aofx/Audio.h`; `InputPlane::audio`, `RenderRequest::audio`
/// with the project's `audioFormat` and `audioFrames`, `ClipInfo::hasAudio`
/// and `RecorderDesc::audioCodec` appended to structs the plugin fills and
/// the host reads (the argument of 6); `EffectDesc::audio` and `::audioOnly`
/// appended likewise; `Gpu::record` grows a fourth argument and
/// `Gpu::decodeAudio` is a new virtual (the argument of 2). Every plugin
/// rebuilt, and a bundle built against 19 refused rather than handed a
/// request whose fields it would read at the wrong offsets.
/// 21: identity in a box. `aofx::kAttachedBoxStride` goes from six to seven
/// and `meta.z` of a box buffer stops being the constant 1 and starts being
/// the track number -- McByte follows a player through a match and the number
/// it gives him had nowhere to travel. Not a vtable and not a struct layout:
/// an agreement about the contents of an array, which is exactly what 5 was
/// bumped for. A bundle built against 20 attaches rows of six that the host
/// reads as rows of seven, and every box after the first is the previous
/// one's numbers shifted along.
/// 22: `RenderRequest::bufferFrames`, the machine's latency budget -- the
/// depth of the live jitter buffer -- so a node that needs to look ahead
/// before it answers spends the number this system already states instead of
/// inventing a second one. A struct layout change, so every bundle is
/// rebuilt: one built against 21 would read every field after it from the
/// wrong offset.
/// 23: `ClipDesc::alsoAtParam`, a clip at the frame a parameter names --
/// a tracker's reference frame beside every frame it renders, so it can
/// read its reference again without the playhead going there. A struct
/// layout change, so every bundle is rebuilt.
/// 24: `EffectDesc::flowsWhen`, a node that is a live source while a
/// parameter says so, `RenderRequest::projectWidth`/`projectHeight` and
/// `complaint`, and the `Gpu::borrow` and `Gpu::importFd` verbs -- a frame
/// another process paints bound where it is instead of copied. A struct
/// layout and vtable change, so every bundle is rebuilt.
/// 25: `buildTag()` says which ABI of its standard library a translation unit
/// was built with, and what that library's containers measure. The same GCC
/// with `_GLIBCXX_USE_CXX11_ABI=0` and `=1` used to write the same tag while
/// `std::string` was 8 bytes on one side and 32 on the other -- `EffectDesc`
/// 136 against 280 -- and MSVC wrote no version at all. No struct or vtable
/// moves; what changes is the *contents* of the tag, and the host compares
/// tags, so a host and a bundle built against 24 and 25 no longer match and
/// both are rebuilt.
/// 26: `Gpu::engines` and `Gpu::render` -- a scene handed to a renderer the
/// host owns, its planes crossing in both directions on the device.
/// `EngineRequest`, `EngineOutput`, `EngineResult` and `PlaneFormat` are new
/// types nothing older reads. Two virtuals appended to the host interface,
/// the argument of 2: defaulted, so no effect changes, and every bundle is
/// rebuilt all the same, because a vtable two slots short is a call into the
/// wrong function.
inline constexpr int kAbiVersion = 26;

/// What this translation unit was compiled with.
///
/// Compiled into the plugin and into the host from the same header, so the two
/// strings are produced by the two compilers that actually built them. Comparing
/// them catches the case the version number cannot: same interface, different
/// toolchain, and a `std::string` that means something different on each side.
///
/// WHAT IS IN IT, AND WHY EACH PART
///
/// A compiler name and version is not enough, and that was learned the hard way
/// on paper before it was learned in a crash: GCC 13.3 with
/// `_GLIBCXX_USE_CXX11_ABI=0` and with `=1` wrote the same "gcc-13.3.0
/// libstdc++" while `std::string` was 8 bytes on one side and 32 on the other.
/// So the tag carries, in this order:
///
///   - the compiler and its version (MSVC: `_MSC_FULL_VER`, where it used to say
///     only "msvc");
///   - the standard library, and the ABI switch that library has:
///     `_GLIBCXX_USE_CXX11_ABI` for libstdc++, `_LIBCPP_ABI_VERSION` for libc++,
///     `_ITERATOR_DEBUG_LEVEL` and debug or release for Microsoft's, whose
///     containers change shape with both;
///   - a fingerprint of what crosses the boundary: the sizes of `std::string`,
///     `std::vector<int>` and `std::function<void()>`, and of a pointer. The
///     switches above are the causes; these are the effects, and they catch a
///     cause nobody thought to list.
///
/// Built once, at compile time, into a constant this function returns. The
/// pointer is stable for the life of the module and the text never allocates.
namespace detail {

/// A string built by a constant expression: text and unsigned numbers, appended.
struct BuildTagText {
    char        text[384]{};
    std::size_t length = 0;

    constexpr void put(const char* more) {
        for (std::size_t i = 0; more[i] != '\0' && length + 1 < sizeof(text); ++i) {
            text[length++] = more[i];
        }
    }
    constexpr void put(unsigned long long value) {
        char digits[24]{};
        std::size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && count < sizeof(digits));
        while (count > 0 && length + 1 < sizeof(text)) {
            text[length++] = digits[--count];
        }
    }
};

[[nodiscard]] constexpr BuildTagText makeBuildTag() {
    BuildTagText tag;
#if defined(__clang__)
    tag.put("clang-");
    tag.put(__clang_version__);
#elif defined(__GNUC__)
    tag.put("gcc-");
    tag.put(__VERSION__);
#elif defined(_MSC_VER)
    tag.put("msvc-");
    tag.put(static_cast<unsigned long long>(_MSC_FULL_VER));
#else
    tag.put("unknown-compiler");
#endif

#if defined(_LIBCPP_VERSION)
    tag.put(" libc++");
#  if defined(_LIBCPP_ABI_VERSION)
    tag.put(" abi=");
    tag.put(static_cast<unsigned long long>(_LIBCPP_ABI_VERSION));
#  endif
#elif defined(__GLIBCXX__)
    tag.put(" libstdc++");
#  if defined(_GLIBCXX_USE_CXX11_ABI)
    tag.put(" cxx11abi=");
    tag.put(static_cast<unsigned long long>(_GLIBCXX_USE_CXX11_ABI));
#  endif
#elif defined(_MSVC_STL_VERSION) || defined(_MSC_VER)
    tag.put(" msvcstl");
#  if defined(_ITERATOR_DEBUG_LEVEL)
    tag.put(" idl=");
    tag.put(static_cast<unsigned long long>(_ITERATOR_DEBUG_LEVEL));
#  endif
#  if defined(_DEBUG)
    tag.put(" debug");
#  else
    tag.put(" release");
#  endif
#else
    tag.put(" unknown-stdlib");
#endif

    tag.put(" sizes:string=");
    tag.put(static_cast<unsigned long long>(sizeof(std::string)));
    tag.put(",vector=");
    tag.put(static_cast<unsigned long long>(sizeof(std::vector<int>)));
    tag.put(",function=");
    tag.put(static_cast<unsigned long long>(sizeof(std::function<void()>)));
    tag.put(",pointer=");
    tag.put(static_cast<unsigned long long>(sizeof(void*)));
    return tag;
}

inline constexpr BuildTagText kBuildTag = makeBuildTag();

}   // namespace detail

[[nodiscard]] inline const char* buildTag() {
    return detail::kBuildTag.text;
}

}   // namespace aofx
