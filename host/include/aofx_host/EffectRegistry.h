// Copyright (c) 2026 aopenfx contributors.
//
// Finding AOFX bundles, loading them, and refusing the ones that would break.
//
// A bundle is a directory laid out the way an OpenFX one is, because that
// layout already works on all three platforms and inventing a second is work
// with nothing at the end of it:
//
//     Blur.aofx.bundle/Contents/Info.plist          (macOS only)
//     Blur.aofx.bundle/Contents/MacOS/Blur.aofx     (or Win64/, Linux-x86-64/)
//
// WHY A BUNDLE CAN BE REFUSED, AND WHY THAT IS THE FEATURE
//
// The interface is C++: virtual calls, std::string and std::vector across the
// boundary. That was chosen deliberately and it has a price. A bundle built
// with a different compiler or standard library will load, resolve its symbols,
// and then corrupt memory in ways that look like anything but the real cause.
//
// So a bundle states what it was built against, and anything that does not
// match is refused with a reason. Refusing is the point. A plugin that will not
// load and says why is a plugin somebody can fix; one that loads and misbehaves
// is a bug report about the wrong thing entirely.
//
// ONE HOST
//
// This is the reference host, and it is the only one: every program that loads
// AOFX bundles loads them through this code, with what it brings declared in
// `Capabilities`. A bundle refused here is refused everywhere with the same
// sentence, and a bundle that loads here loads everywhere -- provided the
// build tag matches, which is the toolchain's promise and not this file's.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aofx/Descriptor.h"

namespace aofx {
class Effect;
}

namespace gpe {
class PooledDevice;
}

namespace aofx::host {

struct Capabilities;

/// A bundle that was looked at, whether or not it loaded.
///
/// Failures are kept rather than dropped. "The effect is not in the menu" is
/// the least useful thing a person can be told, and a list of what was found
/// and why each one was turned away is the difference between a support
/// question and a fix.
struct BundleReport {
    std::filesystem::path    path;
    bool                     loaded = false;
    std::string              reason;    ///< empty when it loaded
    std::vector<std::string> effects;   ///< identifiers, when it loaded
};

class EffectRegistry {
public:
    /// `capabilities` may be null: a registry with nothing declared, which
    /// still loads every bundle and logs to stderr. It is not copied, and it
    /// must outlive the registry.
    explicit EffectRegistry(const Capabilities* capabilities = nullptr);
    ~EffectRegistry();

    EffectRegistry(const EffectRegistry&) = delete;
    EffectRegistry& operator=(const EffectRegistry&) = delete;

    /// Where bundles are looked for, in order: `$AOFX_PLUGIN_PATH` (colon or
    /// semicolon separated), then the platform's own location, then whatever
    /// was added here. The same directory reached two ways is searched once.
    ///
    /// Adding a path after `scan` has no effect until the next `scan`.
    void addSearchPath(std::filesystem::path path);
    [[nodiscard]] std::vector<std::filesystem::path> searchPaths() const;

    /// Finds and loads everything on the search path.
    ///
    /// `device` is the process's compute device. **Null means nothing is
    /// loaded at all**, and that is the whole design rather than a limitation:
    /// an AOFX effect is a kernel, and a kernel needs a device. Registering
    /// effects that could never run would put nodes in a menu that fail the
    /// moment they are used, which is worse than an empty menu and a line
    /// saying why.
    ///
    /// Bundles are loaded in sorted order, so which of two bundles offering
    /// one identifier wins does not depend on the filesystem. Idempotent:
    /// scanning twice does not load a bundle twice.
    void scan(gpe::PooledDevice* device);

    /// What the scan found, loaded and refused alike.
    [[nodiscard]] const std::vector<BundleReport>& reports() const noexcept;

    /// Every effect that loaded, by identifier, exactly as it described itself.
    /// The program translates these into whatever vocabulary its menus and
    /// panels speak; the host does not know that vocabulary and must not.
    [[nodiscard]] const std::map<std::string, aofx::EffectDesc>& descriptions() const noexcept;

    /// The extra planes an effect declares, by id, without the picture itself.
    ///
    /// The picture is `outputs[0]` and is left out: it is not a layer somebody
    /// chooses, it is what they are looking at already.
    [[nodiscard]] std::vector<std::string> extraPlanesOf(const std::string& identifier) const;

    /// The effect itself, for rendering. Null if there is no such effect. An
    /// identifier in an older spelling is mapped through
    /// `Capabilities::canonicalId` first, when the program set one.
    ///
    /// Owned by the registry, which owns the bundle it came from: an effect
    /// outliving its bundle is a vtable pointing into an unmapped page.
    [[nodiscard]] aofx::Effect* find(const std::string& identifier) const;

    /// True where the effect said its work happens only in a delivery
    /// (`EffectDesc::offline`). False for an unknown identifier.
    [[nodiscard]] bool isOffline(const std::string& identifier) const;

    /// When an effect's nodes are live sources (`EffectDesc::flowsWhen`).
    /// Nothing for an unknown identifier or an effect that never flows.
    [[nodiscard]] std::optional<aofx::ShownWhen> flowsWhen(const std::string& identifier) const;

    /// Visible so the loader in the .cpp can fill it in; not part of the API.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}   // namespace aofx::host
