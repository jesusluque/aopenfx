// Copyright (c) 2026 aopenfx contributors.
#include "aofx_host/EffectRegistry.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

#include <dlfcn.h>

#include "aofx/Effect.h"
#include "aofx/Version.h"
#include "aofx_host/Capabilities.h"
#include "aofx_host/Channels.h"
#include "gpe/kernels.h"

#include "aofx_kernels_channels.h"

namespace fs = std::filesystem;

namespace aofx::host {
namespace {

/// The directory inside a bundle that holds the binary. The same names OpenFX
/// uses, so the two kinds of bundle can sit in the same tree and be built by
/// the same rules.
constexpr const char* kArchDir =
#if defined(__APPLE__)
    "MacOS";
#elif defined(_WIN32)
    "Win64";
#else
    "Linux-x86-64";
#endif

constexpr const char* kBundleSuffix = ".aofx.bundle";
constexpr const char* kBinarySuffix = ".aofx";

Logger& loggerOf(const Capabilities* capabilities) {
    static Logger fallback;
    return capabilities != nullptr && capabilities->log != nullptr ? *capabilities->log
                                                                    : fallback;
}

/// Split on the platform's path separator. Colon everywhere but Windows, where
/// a colon is the second character of half the paths on the machine.
std::vector<fs::path> splitSearchPath(const char* value) {
    std::vector<fs::path> out;
    if (value == nullptr) {
        return out;
    }
#if defined(_WIN32)
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::string all(value);
    size_t      start = 0;
    while (start <= all.size()) {
        const size_t at = all.find(separator, start);
        const std::string piece =
            all.substr(start, at == std::string::npos ? std::string::npos : at - start);
        if (!piece.empty()) {
            out.emplace_back(piece);
        }
        if (at == std::string::npos) {
            break;
        }
        start = at + 1;
    }
    return out;
}

/// Where the platform keeps them, beside where it keeps OpenFX plugins.
fs::path systemPath() {
#if defined(__APPLE__)
    return "/Library/AOFX/Plugins";
#elif defined(_WIN32)
    return "C:/Program Files/Common Files/AOFX/Plugins";
#else
    return "/usr/AOFX/Plugins";
#endif
}

/// A loaded bundle and everything it brought.
struct Bundle {
    fs::path                   path;
    void*                      handle = nullptr;
    std::vector<aofx::Effect*> effects;
    std::vector<std::string>   kernelNames;

    ~Bundle() {
        // The kernels go before the library does. A registered name pointing
        // into a page that is about to be unmapped is a dispatch that reads
        // freed memory, and the failure would surface inside a driver with
        // nothing on the stack to say where it came from.
        for (const std::string& name : kernelNames) {
            gpe::unregisterKernel(name);
        }
        if (handle != nullptr) {
            dlclose(handle);
        }
    }
};

/// The four symbols a bundle exports.
struct Entry {
    int (*abiVersion)() = nullptr;
    const char* (*buildTag)() = nullptr;
    int (*effectCount)() = nullptr;
    aofx::Effect* (*effect)(int) = nullptr;
};

/// Resolves the four exported symbols. Missing any one of them means this is
/// not an AOFX bundle, or was built with hidden visibility -- which looks
/// identical from here and is by far the more likely of the two.
bool resolve(void* handle, Entry& into, std::string& why) {
    into.abiVersion = reinterpret_cast<int (*)()>(dlsym(handle, "AofxGetAbiVersion"));
    into.buildTag = reinterpret_cast<const char* (*)()>(dlsym(handle, "AofxGetBuildTag"));
    into.effectCount = reinterpret_cast<int (*)()>(dlsym(handle, "AofxGetEffectCount"));
    into.effect = reinterpret_cast<aofx::Effect* (*)(int)>(dlsym(handle, "AofxGetEffect"));
    if (into.abiVersion == nullptr || into.buildTag == nullptr ||
        into.effectCount == nullptr || into.effect == nullptr) {
        why = "it does not export the AOFX entry points; if this is an AOFX "
              "bundle it was probably built with hidden visibility";
        return false;
    }
    return true;
}

/// The binary inside the bundle, by the layout OpenFX established.
fs::path binaryIn(const fs::path& bundle) {
    std::string stem = bundle.filename().string();
    stem = stem.substr(0, stem.size() - std::strlen(kBundleSuffix));
    return bundle / "Contents" / kArchDir / (stem + kBinarySuffix);
}

}   // namespace

struct EffectRegistry::Impl {
    const Capabilities*                     capabilities = nullptr;
    std::vector<fs::path>                   extraPaths;
    std::vector<std::unique_ptr<Bundle>>    bundles;
    std::vector<BundleReport>               reports;
    std::map<std::string, aofx::Effect*>    byIdentifier;
    std::map<std::string, aofx::EffectDesc> describedBy;
    bool                                    scanned = false;
};

namespace {

/// Opens one bundle, checks it, and keeps whatever it brought.
///
/// Every refusal is recorded rather than logged and forgotten. "The effect is
/// not in the menu" is the least useful thing a person can be told.
void load(EffectRegistry::Impl& impl, const fs::path& bundle) {
    Logger&      log = loggerOf(impl.capabilities);
    BundleReport report;
    report.path = bundle;

    const fs::path  binary = binaryIn(bundle);
    std::error_code ignored;
    if (!fs::exists(binary, ignored)) {
        report.reason = "no binary at " + binary.string();
        impl.reports.push_back(std::move(report));
        return;
    }

    // RTLD_LOCAL, deliberately: a plugin's symbols must not be visible to the
    // next plugin, or two bundles built from the same SDK would share one
    // another's statics and the second would quietly use the first's.
    void* handle = dlopen(binary.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* why = dlerror();
        report.reason = why == nullptr ? "it would not load" : why;
        impl.reports.push_back(std::move(report));
        return;
    }

    auto refuse = [&](std::string why) {
        dlclose(handle);
        report.reason = std::move(why);
        impl.reports.push_back(std::move(report));
    };

    Entry entry;
    if (std::string why; !resolve(handle, entry, why)) {
        refuse(std::move(why));
        return;
    }

    // The version first, because everything after it is a call through a vtable
    // whose shape this number describes.
    if (const int theirs = entry.abiVersion(); theirs != aofx::kAbiVersion) {
        refuse("it was built against AOFX ABI " + std::to_string(theirs) +
               ", and this host speaks " + std::to_string(aofx::kAbiVersion));
        return;
    }

    // Then the toolchain, which the version number cannot catch. Same
    // interface, different standard library, and a std::string that means
    // something different on each side of the call.
    if (const std::string theirs = entry.buildTag(); theirs != aofx::buildTag()) {
        refuse("it was built with " + theirs + ", and this host with " +
               std::string(aofx::buildTag()) + "; a C++ interface cannot cross that");
        return;
    }

    auto held = std::make_unique<Bundle>();
    held->path = bundle;
    held->handle = handle;

    const int count = entry.effectCount();
    for (int i = 0; i < count; ++i) {
        aofx::Effect* effect = entry.effect(i);
        if (effect == nullptr) {
            continue;
        }
        aofx::EffectDesc desc;
        effect->describe(desc);
        if (desc.identifier.empty()) {
            log.warn("an effect in " + bundle.string() + " has no identifier and was skipped");
            continue;
        }
        if (impl.byIdentifier.count(desc.identifier) != 0) {
            log.warn(bundle.string() + " offers '" + desc.identifier +
                     "', which is already loaded; keeping the first");
            continue;
        }

        // Its kernels, so the device can find them by name. Registered before
        // the effect is offered: an effect in the menu whose kernels are not
        // loadable is a node that fails the first time it is used.
        bool kernelsOk = true;
        for (const aofx::KernelDesc& kernel : effect->kernels()) {
            if (!gpe::registerKernel(kernel.name, kernel.entry, kernel.blob, kernel.blobBytes)) {
                log.warn("'" + desc.identifier + "' brought a kernel '" + kernel.name +
                         "' that could not be registered");
                kernelsOk = false;
                break;
            }
            held->kernelNames.push_back(kernel.name);
        }
        if (!kernelsOk) {
            continue;
        }

        held->effects.push_back(effect);
        impl.byIdentifier.emplace(desc.identifier, effect);
        impl.describedBy.emplace(desc.identifier, desc);
        report.effects.push_back(desc.identifier);
    }

    report.loaded = true;
    impl.bundles.push_back(std::move(held));
    impl.reports.push_back(std::move(report));
}

}   // namespace

EffectRegistry::EffectRegistry(const Capabilities* capabilities)
    : impl_(std::make_unique<Impl>()) {
    impl_->capabilities = capabilities;
}

EffectRegistry::~EffectRegistry() = default;

void EffectRegistry::addSearchPath(fs::path path) {
    impl_->extraPaths.push_back(std::move(path));
}

std::vector<fs::path> EffectRegistry::searchPaths() const {
    std::vector<fs::path> all = splitSearchPath(std::getenv("AOFX_PLUGIN_PATH"));
    all.push_back(systemPath());
    all.insert(all.end(), impl_->extraPaths.begin(), impl_->extraPaths.end());

    // The same directory twice is the same bundle twice, and the second copy
    // of every effect is refused with a warning -- forty lines of "already
    // loaded; keeping the first" on a host that is working perfectly. It
    // happens the moment somebody is explicit: an installed copy finds its
    // bundles beside the binary AND a service unit names that same directory
    // in AOFX_PLUGIN_PATH, which is a good thing for a unit to do.
    //
    // Compared canonically, because `<prefix>/current/aofx` and
    // `<prefix>/1.0.0/aofx` are one directory reached two ways, and string
    // equality would not see it.
    std::vector<fs::path> out;
    for (const fs::path& one : all) {
        std::error_code ignored;
        fs::path        real = fs::weakly_canonical(one, ignored);
        if (real.empty()) {
            real = one;
        }
        const bool seen = std::any_of(out.begin(), out.end(), [&](const fs::path& had) {
            std::error_code also;
            fs::path        other = fs::weakly_canonical(had, also);
            return (other.empty() ? had : other) == real;
        });
        if (!seen) {
            out.push_back(one);
        }
    }
    return out;
}

const std::vector<BundleReport>& EffectRegistry::reports() const noexcept {
    return impl_->reports;
}

const std::map<std::string, aofx::EffectDesc>& EffectRegistry::descriptions() const noexcept {
    return impl_->describedBy;
}

void EffectRegistry::scan(gpe::PooledDevice* device) {
    if (impl_->scanned) {
        return;
    }
    impl_->scanned = true;
    Logger& log = loggerOf(impl_->capabilities);

    if (device == nullptr) {
        // Not a failure and not silent. An AOFX effect is a kernel; without a
        // device there is nothing for it to run on, and a menu full of nodes
        // that fail the moment they are used is worse than a menu without them.
        log.info("no GPU; AOFX effects are not available on this machine");
        return;
    }

    // The host's own kernel, registered the same way a plugin's is. It puts
    // back the channels an effect was not applied to, and every effect gets it
    // whether or not its author ever thought about channels. Registering twice
    // (a second registry in one process) is harmless: the name is not gpe's
    // own, so re-registration replaces it with the same blob.
    if (!gpe::registerKernel(kChannelsKernel, kChannelsEntry, k_channels, k_channelsBytes)) {
        log.error("the channel-restore kernel could not be registered; the channel "
                  "control will not do anything");
    }

    for (const fs::path& root : searchPaths()) {
        std::error_code ignored;
        if (!fs::is_directory(root, ignored)) {
            continue;
        }
        std::vector<fs::path> found;
        for (const fs::directory_entry& entry : fs::directory_iterator(root, ignored)) {
            const std::string name = entry.path().filename().string();
            if (name.size() <= std::strlen(kBundleSuffix) ||
                name.compare(name.size() - std::strlen(kBundleSuffix), std::string::npos,
                             kBundleSuffix) != 0) {
                continue;
            }
            found.push_back(entry.path());
        }
        // Sorted: directory order is the filesystem's, and which of two
        // bundles offering one identifier wins should not depend on it.
        std::sort(found.begin(), found.end());
        for (const fs::path& bundle : found) {
            load(*impl_, bundle);
        }
    }
}

std::vector<std::string> EffectRegistry::extraPlanesOf(const std::string& identifier) const {
    aofx::Effect* effect = find(identifier);
    if (effect == nullptr) {
        return {};
    }
    aofx::EffectDesc desc;
    effect->describe(desc);
    std::vector<std::string> out;
    for (size_t i = 1; i < desc.outputs.size(); ++i) {
        out.push_back(desc.outputs[i].id);
    }
    return out;
}

aofx::Effect* EffectRegistry::find(const std::string& identifier) const {
    // An identifier in an older spelling names the same effect, when the
    // program says how: a client that has not caught up still finds what it
    // asked for.
    const std::string canonical =
        impl_->capabilities != nullptr && impl_->capabilities->canonicalId
            ? impl_->capabilities->canonicalId(identifier)
            : identifier;
    const auto found = impl_->byIdentifier.find(canonical);
    return found == impl_->byIdentifier.end() ? nullptr : found->second;
}

std::optional<aofx::ShownWhen> EffectRegistry::flowsWhen(const std::string& identifier) const {
    const auto found = impl_->describedBy.find(identifier);
    if (found == impl_->describedBy.end() || found->second.flowsWhen.param.empty()) {
        return std::nullopt;
    }
    return found->second.flowsWhen;
}

bool EffectRegistry::isOffline(const std::string& identifier) const {
    const auto found = impl_->describedBy.find(identifier);
    return found != impl_->describedBy.end() && found->second.offline;
}

}   // namespace aofx::host
