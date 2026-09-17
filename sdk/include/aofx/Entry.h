// Copyright (c) 2026 aopenfx contributors.
//
// The four symbols a bundle exports, and the macro that writes them.
//
// VISIBILITY IS LOAD-BEARING
//
// This project builds with hidden visibility, which is right for every library
// in it and fatal for a plugin: these are the only symbols a host looks for.
// Hidden, the bundle loads, `dlsym` returns null, and the effect is simply
// absent from the menu with nothing said anywhere -- which has already happened
// once here, to the OpenFX blur.
//
// So the macro marks them visible itself rather than trusting a build to have
// undone the default. A plugin's CMake should undo it as well, and the two
// together are cheaper than the afternoon spent finding out why an effect that
// clearly exists is not in the list.
#pragma once

#include <cstddef>

#include "aofx/Effect.h"
#include "aofx/Version.h"

#if defined(_WIN32)
#define AOFX_EXPORT __declspec(dllexport)
#else
#define AOFX_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

/// The ABI this bundle was built against. Checked first, before anything else
/// in the bundle is called: a mismatch means the vtables below have a different
/// shape and calling through them is undefined.
AOFX_EXPORT int AofxGetAbiVersion();

/// The compiler and standard library it was built with, from `aofx::buildTag()`.
/// The version number cannot catch a toolchain mismatch; this can.
AOFX_EXPORT const char* AofxGetBuildTag();

/// How many effects are in here. A bundle may hold several.
AOFX_EXPORT int AofxGetEffectCount();

/// One of them. The host does not take ownership: the objects live as long as
/// the bundle does, which is as long as the process. An effect holds no
/// per-node state, so one instance serving every node is the design rather
/// than a shortcut.
AOFX_EXPORT aofx::Effect* AofxGetEffect(int index);

}   // extern "C"

/// Writes all four, given a list of effect types.
///
/// Usage, at file scope in exactly one translation unit:
///
///     AOFX_EXPORT_EFFECTS(BlurEffect, SharpenEffect)
///
/// The instances are function-local statics, so they are constructed the first
/// time they are asked for and never destroyed -- which is what you want for
/// something the host may call during its own shutdown.
#define AOFX_EXPORT_EFFECTS(...)                                              \
    namespace {                                                               \
    template <typename... Effects>                                            \
    struct AofxEffectTable {                                                  \
        static constexpr int count = sizeof...(Effects);                      \
        template <typename One>                                               \
        static aofx::Effect* instance() {                                     \
            static One one;                                                   \
            return static_cast<aofx::Effect*>(&one);                          \
        }                                                                     \
        static aofx::Effect* at(int index) {                                  \
            static aofx::Effect* const kAll[] = {instance<Effects>()...};     \
            return index >= 0 && index < count ? kAll[index] : nullptr;       \
        }                                                                     \
    };                                                                        \
    using AofxEffects = AofxEffectTable<__VA_ARGS__>;                         \
    }                                                                         \
    extern "C" {                                                              \
    AOFX_EXPORT int AofxGetAbiVersion() { return aofx::kAbiVersion; }         \
    AOFX_EXPORT const char* AofxGetBuildTag() { return aofx::buildTag(); }    \
    AOFX_EXPORT int AofxGetEffectCount() { return AofxEffects::count; }       \
    AOFX_EXPORT aofx::Effect* AofxGetEffect(int index) {                      \
        return AofxEffects::at(index);                                        \
    }                                                                         \
    }
