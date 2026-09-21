// Copyright (c) 2026 aopenfx contributors.
#include "aofx_host/EffectRunner.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "aofx_host/Capabilities.h"
#include "gpe/args.h"
#include "gpe/kernels.h"
#include "gpe/pool.h"
#include "gpe/types.h"

#include "HostPages.h"

namespace aofx::host {

struct EffectRunner::Impl {
    gpe::PooledDevice*  device = nullptr;
    const Capabilities* capabilities = nullptr;
    /// Resolved once and remembered, so an effect may call `load` every frame
    /// rather than caching an id it would then have to invalidate.
    std::map<std::string, aofx::KernelId> loaded;
    /// The other way round, so a dispatch can be checked against the kernel's
    /// own reflection and refused by name.
    std::map<aofx::KernelId, std::string> names;
    std::vector<gpe::BufferId>            scratch;
    /// The effect instance this runner is rendering, for the model provider.
    std::string instance;
    uint64_t    dispatches = 0;
};

namespace {

Logger& loggerOf(const Capabilities* capabilities) {
    static Logger fallback;
    return capabilities != nullptr && capabilities->log != nullptr ? *capabilities->log
                                                                    : fallback;
}

/// The last complaint, and a lock. Process-wide because the runner is: one
/// render at a time on the GPU thread, and the reader is whoever is about to
/// turn a `false` into a message.
std::mutex& complaintGuard() {
    static std::mutex guard;
    return guard;
}
std::string& complaintText() {
    static std::string text;
    return text;
}

/// What `publish` has been told, for the whole process.
std::mutex& publishedGuard() {
    static std::mutex guard;
    return guard;
}
std::map<std::string, std::map<std::string, double>>& publishedMap() {
    static std::map<std::string, std::map<std::string, double>> published;
    return published;
}

/// What `keep` is holding, for the whole process.
///
/// Process-wide rather than per-runner because a runner lives for one render
/// and the whole point is to outlive that. Guarded, because there is one Effect
/// per plugin and several engines may render at once -- a prefetcher has its
/// own -- and two of them asking for the same cloud at the same moment must get
/// one buffer and one upload, not two.
struct Kept {
    std::mutex                                     mutex;
    std::unordered_map<std::string, gpe::BufferId> buffers;
    std::unordered_map<std::string, size_t>        sizes;
    /// The pages each borrowed key was bound to. A borrow is not device
    /// memory, so it is not in `bytes` either -- the pool does not count it.
    std::unordered_map<std::string, const void*> borrowed;
    size_t                                       bytes = 0;
    uint64_t                                     uploads = 0;
};

Kept& kept() {
    static Kept store;
    return store;
}

/// Takes a key's size out of the accounting, however it was made: a kept
/// buffer is device memory and counted in `bytes`, a borrowed one is not and
/// has its pages recorded instead. Every path that forgets a key uses this, so
/// the two can never be confused into `bytes` wrapping round.
void forgetAccounting(Kept& store, const std::string& key) {
    if (const auto bound = store.borrowed.find(key); bound != store.borrowed.end()) {
        store.borrowed.erase(bound);
    } else if (const auto size = store.sizes.find(key); size != store.sizes.end()) {
        store.bytes -= size->second;
    }
    store.sizes.erase(key);
}

/// A kept or borrowed buffer, described as one row of pixels: `bytes` worth of
/// float4s, packed, at the origin. What every caller of `keep` reads back.
aofx::Buffer linearBuffer(gpe::BufferId id, size_t bytes) {
    aofx::Buffer out;
    out.device = static_cast<uint64_t>(id);
    const size_t count = bytes / gpe::kBytesPerPixel;
    out.width = static_cast<int>(count);
    out.height = 1;
    out.stride = static_cast<int>(count);
    out.rect = aofx::Rect{0, 0, out.width, 1};
    return out;
}

/// The refusal a verb answers when the program declared no provider for it:
/// kept as the complaint every time, said in the log once.
void notAvailable(const Capabilities* capabilities, const char* verb, const char* which) {
    const std::string what = std::string(verb) + ": not available in this host, because it "
                                                 "declares no " +
                             which + " capability";
    EffectRunner::complain(what);
    static std::mutex            guard;
    static std::set<std::string> said;
    const std::lock_guard<std::mutex> held(guard);
    if (said.insert(verb).second) {
        loggerOf(capabilities).info(what);
    }
}

}   // namespace

EffectRunner::EffectRunner(gpe::PooledDevice& device, const Capabilities& capabilities)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = &device;
    impl_->capabilities = &capabilities;
}

EffectRunner::~EffectRunner() {
    for (const gpe::BufferId buffer : impl_->scratch) {
        impl_->device->release(buffer);
    }
}

gpe::PooledDevice& EffectRunner::device() noexcept { return *impl_->device; }

const Capabilities& EffectRunner::capabilities() const noexcept {
    return *impl_->capabilities;
}

std::string EffectRunner::lastComplaint() {
    const std::lock_guard<std::mutex> held(complaintGuard());
    return complaintText();
}

void EffectRunner::complain(std::string what) {
    const std::lock_guard<std::mutex> held(complaintGuard());
    complaintText() = std::move(what);
}

void EffectRunner::noteInstance(std::string instance) {
    impl_->instance = std::move(instance);
}

const std::string& EffectRunner::instance() const noexcept { return impl_->instance; }

// --- kernels ----------------------------------------------------------------

aofx::KernelId EffectRunner::load(const std::string& name) {
    if (const auto found = impl_->loaded.find(name); found != impl_->loaded.end()) {
        return found->second;
    }
    const gpe::KernelId kernel = impl_->device->load(name);
    if (kernel == gpe::kInvalidKernel) {
        // Once per name, not once per frame: a kernel that is missing is
        // missing for the life of the process, and sixty lines a second saying
        // so buries everything else.
        loggerOf(impl_->capabilities)
            .error("no AOFX kernel '" + name + "'; the plugin's build and its source disagree");
        // And kept, so the failure the caller reports can carry the name
        // instead of the caller having to go and find this line.
        complain("no kernel '" + name + "'; the plugin declares one its build does not have");
    }
    const auto id = static_cast<aofx::KernelId>(kernel);
    impl_->loaded.emplace(name, id);
    impl_->names.emplace(id, name);
    return id;
}

bool EffectRunner::run(aofx::KernelId kernel, aofx::Grid grid,
                       const std::vector<aofx::Buffer>& buffers, const void* uniforms,
                       size_t uniformBytes) {
    if (kernel == aofx::kInvalidKernel || buffers.empty()) {
        return false;
    }
    // Checked against the kernel's own reflection when its blob carries it: a
    // dispatch that would have bound the wrong number of buffers, or a uniform
    // block of the wrong size -- which used to render something, silently --
    // is a refusal with a reason instead.
    const auto                     name = impl_->names.find(kernel);
    std::optional<gpe::KernelInfo> info;
    if (name != impl_->names.end()) {
        info = gpe::kernelInfo(name->second);
    }
    if (info.has_value()) {
        if (info->elementBytes.size() != buffers.size()) {
            complain("kernel '" + name->second + "' declares " +
                     std::to_string(info->elementBytes.size()) + " buffers and was run with " +
                     std::to_string(buffers.size()));
            return false;
        }
        if (info->uniformBytes != uniformBytes) {
            complain("kernel '" + name->second + "' declares " +
                     std::to_string(info->uniformBytes) + " bytes of uniforms and was run with " +
                     std::to_string(uniformBytes));
            return false;
        }
    }
    gpe::Args args;
    for (size_t i = 0; i < buffers.size(); ++i) {
        if (buffers[i].device == gpe::kInvalidBuffer) {
            return false;
        }
        // The kernel's own element size when its reflection says; one byte
        // otherwise, deliberately. The count only feeds Slang's bound check,
        // whose units are the kernel's element type -- which the host cannot
        // know without the trailer: pictures are float4 but a tile list is
        // uint, and sixteen bytes here made CUDA's check reject the top three
        // quarters of every uint buffer. Counting in bytes admits every
        // legitimate index for every element type.
        const uint32_t element =
            info.has_value() && info->elementBytes[i] > 0 ? info->elementBytes[i] : 1u;
        args.buffer(static_cast<gpe::BufferId>(buffers[i].device), element);
    }
    if (uniforms != nullptr && uniformBytes > 0) {
        args.uniformBytes(uniforms, uniformBytes);
    }
    impl_->device->dispatch(static_cast<gpe::KernelId>(kernel),
                            gpe::Grid{grid.x, grid.y, grid.z}, args.data(), args.size());
    ++impl_->dispatches;
    return true;
}

aofx::Buffer EffectRunner::scratch(int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    const size_t bytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * gpe::kBytesPerPixel;
    const gpe::BufferId buffer = impl_->device->alloc(bytes);
    if (buffer == gpe::kInvalidBuffer) {
        // No room. A normal answer under pressure, and the effect is expected
        // to give up on the frame rather than render half of it.
        return {};
    }
    impl_->scratch.push_back(buffer);
    aofx::Buffer out;
    out.device = static_cast<uint64_t>(buffer);
    out.width = width;
    out.height = height;
    // Packed: a scratch buffer is ours, so it has no padding to carry. And at
    // the origin: it has no place in the picture -- it is working space -- so
    // the effect relates it to whatever it is passing through, and the
    // simplest honest answer is a rectangle of its own size at zero.
    out.stride = width;
    out.rect = aofx::Rect{0, 0, width, height};
    return out;
}

uint64_t EffectRunner::dispatches() const noexcept { return impl_->dispatches; }

uint64_t EffectRunner::scratchBuffers() const noexcept { return impl_->scratch.size(); }

// --- memory that outlives a render -------------------------------------------

bool EffectRunner::read(const aofx::Buffer& buffer, void* into, size_t bytes) {
    if (!buffer.isValid() || into == nullptr || bytes == 0) {
        return false;
    }
    // Synchronous by design: the device drains, then the bytes come. Small
    // answers only; a picture through here would be the slow path nobody
    // measured.
    impl_->device->download(into, static_cast<gpe::BufferId>(buffer.device), bytes);
    return true;
}

aofx::Buffer EffectRunner::keep(const std::string& key, const void* data, size_t bytes) {
    if (key.empty() || bytes % gpe::kBytesPerPixel != 0) {
        return {};
    }
    Kept&                             store = kept();
    const std::lock_guard<std::mutex> holding(store.mutex);

    if (const auto found = store.buffers.find(key); found != store.buffers.end()) {
        return linearBuffer(found->second, store.sizes[key]);
    }
    if (data == nullptr || bytes == 0) {
        // Asked for something that is not there, without the data to make it.
        // An honest empty answer: the effect should notice and load it.
        return {};
    }
    const gpe::BufferId buffer = impl_->device->alloc(bytes);
    if (buffer == gpe::kInvalidBuffer) {
        return {};
    }
    impl_->device->upload(buffer, data, bytes);
    store.buffers.emplace(key, buffer);
    store.sizes.emplace(key, bytes);
    store.bytes += bytes;
    ++store.uploads;
    return linearBuffer(buffer, bytes);
}

aofx::Buffer EffectRunner::borrow(const std::string& key, const void* pages, size_t bytes) {
    if (key.empty()) {
        return {};
    }
    Kept&                             store = kept();
    const std::lock_guard<std::mutex> holding(store.mutex);

    if (const auto found = store.buffers.find(key); found != store.buffers.end()) {
        const auto bound = store.borrowed.find(key);
        const bool same = pages == nullptr ||
                          (bound != store.borrowed.end() && bound->second == pages &&
                           store.sizes[key] == bytes);
        if (same) {
            return linearBuffer(found->second, store.sizes[key]);
        }
        // The key names other pages now -- a mapping replaced, a helper
        // restarted. The old binding goes rather than being handed back over
        // pages that may already be unmapped.
        impl_->device->release(found->second);
        forgetAccounting(store, key);
        store.buffers.erase(found);
    }
    if (pages == nullptr || bytes == 0 || bytes % gpe::kBytesPerPixel != 0) {
        // Like `keep`: asked for what is not there, without what to make it from.
        return {};
    }

    // The pages wrapped as a device buffer where the device can read host
    // memory in place, then adopted by the pool like any foreign buffer: it
    // binds and dispatches as one of the pool's own, and `drop` gives it back.
    const uint64_t native = wrapHostPages(impl_->device->backendDevice(), pages, bytes);
    if (native == 0) {
        return {};
    }
    const gpe::BufferId buffer = impl_->device->adopt(native, bytes);
    releaseWrappedPages(native);   // the pool holds its own reference now
    if (buffer == gpe::kInvalidBuffer) {
        return {};
    }
    store.buffers.emplace(key, buffer);
    store.sizes.emplace(key, bytes);
    store.borrowed.insert_or_assign(key, pages);
    return linearBuffer(buffer, bytes);
}

aofx::Buffer EffectRunner::importFd(const std::string& /*key*/, int /*fd*/, size_t /*bytes*/) {
    // Not on any backend: importing an exported descriptor is CUDA or Vulkan
    // external memory, which gpe does not reach. An invalid answer is the
    // contract's "use keep", and every caller has that path.
    return {};
}

void EffectRunner::drop(const std::string& key) {
    Kept&                             store = kept();
    const std::lock_guard<std::mutex> holding(store.mutex);
    const auto                        found = store.buffers.find(key);
    if (found == store.buffers.end()) {
        return;
    }
    impl_->device->release(found->second);
    forgetAccounting(store, key);
    store.buffers.erase(found);
}

size_t EffectRunner::dropWhere(const std::function<bool(const std::string&)>& going,
                               gpe::PooledDevice& device) {
    Kept&                             store = kept();
    const std::lock_guard<std::mutex> holding(store.mutex);
    size_t                            count = 0;
    for (auto it = store.buffers.begin(); it != store.buffers.end();) {
        if (!going(it->first)) {
            ++it;
            continue;
        }
        device.release(it->second);
        forgetAccounting(store, it->first);
        it = store.buffers.erase(it);
        ++count;
    }
    return count;
}

void EffectRunner::forgetScope(const std::string& scope, gpe::PooledDevice& device) {
    if (scope.empty()) {
        return;
    }
    // The buffers whose key names the scope, released on the device they came
    // from -- the caller is on its thread. Taken out of the store under its
    // lock, so a render elsewhere cannot find a buffer that is on its way out.
    (void)dropWhere(
        [&](const std::string& key) { return key.find(scope) != std::string::npos; },
        device);
    const std::lock_guard<std::mutex> held(publishedGuard());
    auto&                             published = publishedMap();
    for (auto it = published.begin(); it != published.end();) {
        if (it->first.rfind(scope, 0) == 0) {
            it = published.erase(it);
        } else {
            ++it;
        }
    }
}

size_t EffectRunner::keptBytes() {
    Kept&                             store = kept();
    const std::lock_guard<std::mutex> holding(store.mutex);
    return store.bytes;
}

uint64_t EffectRunner::keepUploads() {
    Kept&                             store = kept();
    const std::lock_guard<std::mutex> holding(store.mutex);
    return store.uploads;
}

// --- telling the outside world ---------------------------------------------

void EffectRunner::publish(const std::string& instance, const char* key, double value) {
    const std::lock_guard<std::mutex> held(publishedGuard());
    publishedMap()[instance][key] = value;
}

std::map<std::string, std::map<std::string, double>> EffectRunner::publishedState() {
    const std::lock_guard<std::mutex> held(publishedGuard());
    return publishedMap();
}

// --- media: the program's provider, or "not available" ----------------------

aofx::ClipId EffectRunner::clip(const std::string& path) {
    if (impl_->capabilities->media == nullptr) {
        notAvailable(impl_->capabilities, "clip", "media");
        return aofx::kInvalidClip;
    }
    return impl_->capabilities->media->clip(path);
}

aofx::ClipInfo EffectRunner::clipInfo(aofx::ClipId id) const {
    if (impl_->capabilities->media == nullptr) {
        return {};
    }
    return impl_->capabilities->media->clipInfo(id);
}

bool EffectRunner::decode(aofx::ClipId id, int frame, const aofx::Buffer& out) {
    if (impl_->capabilities->media == nullptr) {
        notAvailable(impl_->capabilities, "decode", "media");
        return false;
    }
    return impl_->capabilities->media->decode(id, frame, out);
}

bool EffectRunner::decodeNext(aofx::ClipId id, int direction, const aofx::Buffer& out,
                              bool& atStart, bool& atEnd) {
    atStart = false;
    atEnd = false;
    if (impl_->capabilities->media == nullptr) {
        notAvailable(impl_->capabilities, "decodeNext", "media");
        return false;
    }
    return impl_->capabilities->media->decodeNext(id, direction, out, atStart, atEnd);
}

bool EffectRunner::decodeAudio(aofx::ClipId id, int64_t firstSample, int64_t sourceSamples,
                               const aofx::AudioFormat& format, int64_t outFrames,
                               aofx::AudioBlock& out) {
    if (impl_->capabilities->media == nullptr) {
        notAvailable(impl_->capabilities, "decodeAudio", "media");
        return false;
    }
    return impl_->capabilities->media->decodeAudio(id, firstSample, sourceSamples, format,
                                                   outFrames, out);
}

aofx::RecorderId EffectRunner::recorder(const std::string& path, const aofx::RecorderDesc& desc) {
    if (impl_->capabilities->media == nullptr) {
        notAvailable(impl_->capabilities, "recorder", "media");
        return aofx::kInvalidRecorder;
    }
    return impl_->capabilities->media->recorder(path, desc);
}

bool EffectRunner::record(aofx::RecorderId id, const aofx::Buffer& picture, double frame,
                          const aofx::AudioBlock* audio) {
    if (impl_->capabilities->media == nullptr) {
        notAvailable(impl_->capabilities, "record", "media");
        return false;
    }
    return impl_->capabilities->media->record(id, picture, frame, audio);
}

void EffectRunner::closeRecorder(aofx::RecorderId id) {
    if (impl_->capabilities->media != nullptr) {
        impl_->capabilities->media->closeRecorder(id);
    }
}

// --- engines: the program's renderers, checked, or "none" --------------------

std::vector<std::string> EffectRunner::engines() const {
    std::vector<std::string> out;
    for (const EngineBackend* engine : impl_->capabilities->engines) {
        if (engine != nullptr) {
            out.push_back(engine->name());
        }
    }
    return out;
}

aofx::EngineResult EffectRunner::render(const aofx::EngineRequest& request) {
    aofx::EngineResult result;
    const auto refuse = [&](std::string why) {
        result.ok = false;
        result.complaint = std::move(why);
        complain(result.complaint);
        return result;
    };

    EngineBackend* engine = nullptr;
    for (EngineBackend* one : impl_->capabilities->engines) {
        if (one != nullptr && one->name() == request.engine) {
            engine = one;
            break;
        }
    }
    if (engine == nullptr) {
        const std::vector<std::string> have = engines();
        if (have.empty()) {
            return refuse("this host has no render engine");
        }
        std::string list;
        for (const std::string& name : have) {
            list += (list.empty() ? "" : ", ") + name;
        }
        return refuse("no engine named '" + request.engine + "' in this host (it has: " +
                      list + ")");
    }
    if (request.outputs.empty()) {
        return refuse("render asked for no output plane");
    }
    // Every output valid and live on this device, all one rectangle, and
    // each format one the engine produces. Checked here, once, so no engine
    // has to and none can forget to.
    const aofx::Rect& rect = request.outputs.front().plane.buffer.rect;
    for (const aofx::EngineOutput& output : request.outputs) {
        const aofx::Buffer& buffer = output.plane.buffer;
        if (!buffer.isValid() ||
            impl_->device->backendBuffer(static_cast<gpe::BufferId>(buffer.device)) == 0) {
            return refuse("output '" + output.plane.plane + "' is not a live device buffer");
        }
        if (buffer.rect.x1 != rect.x1 || buffer.rect.y1 != rect.y1 ||
            buffer.rect.x2 != rect.x2 || buffer.rect.y2 != rect.y2) {
            return refuse("output '" + output.plane.plane +
                          "' is not the rectangle the first output is");
        }
        if (!engine->produces(output.format)) {
            return refuse("engine '" + request.engine + "' does not produce format " +
                          std::to_string(static_cast<int>(output.format)) + " asked of output '" +
                          output.plane.plane + "'");
        }
    }
    for (const aofx::InputPlane& input : request.inputs) {
        if (!input.buffer.isValid()) {
            return refuse("input '" + input.clip + "' is not a device buffer");
        }
    }
    result = engine->render(request, *impl_->device);
    if (!result.ok) {
        if (result.complaint.empty()) {
            result.complaint = "engine '" + request.engine + "' could not draw the scene";
        }
        complain(result.complaint);
    }
    return result;
}

// --- models: the program's provider, or "not available" ---------------------

aofx::ModelId EffectRunner::model(const std::string& name) {
    if (impl_->capabilities->models == nullptr) {
        notAvailable(impl_->capabilities, "model", "model");
        return aofx::kInvalidModel;
    }
    return impl_->capabilities->models->model(name, impl_->instance);
}

aofx::ModelIo EffectRunner::modelInput(aofx::ModelId id, int index) const {
    if (impl_->capabilities->models == nullptr) {
        return {};
    }
    return impl_->capabilities->models->modelInput(id, index);
}

aofx::ModelIo EffectRunner::modelOutput(aofx::ModelId id, int index) const {
    if (impl_->capabilities->models == nullptr) {
        return {};
    }
    return impl_->capabilities->models->modelOutput(id, index);
}

bool EffectRunner::infer(aofx::ModelId id, const std::vector<aofx::Buffer>& inputs,
                         const std::vector<aofx::Buffer>& outputs) {
    if (impl_->capabilities->models == nullptr) {
        notAvailable(impl_->capabilities, "infer", "model");
        return false;
    }
    return impl_->capabilities->models->infer(id, inputs, outputs);
}

bool EffectRunner::inferShaped(aofx::ModelId id, const std::vector<aofx::Buffer>& inputs,
                               const std::vector<aofx::ModelIo>& inputShapes,
                               const std::vector<aofx::Buffer>& outputs,
                               const std::vector<aofx::ModelIo>& outputShapes) {
    if (impl_->capabilities->models == nullptr) {
        notAvailable(impl_->capabilities, "inferShaped", "model");
        return false;
    }
    return impl_->capabilities->models->inferShaped(id, inputs, inputShapes, outputs,
                                                    outputShapes);
}

bool EffectRunner::inferred(aofx::ModelId id) {
    if (impl_->capabilities->models == nullptr) {
        return false;
    }
    return impl_->capabilities->models->inferred(id);
}

}   // namespace aofx::host
