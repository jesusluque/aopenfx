// Copyright (c) 2026 aopenfx contributors.
//
// Running an AOFX effect: the host side of `aofx::Gpu`.
//
// This is the whole of what a plugin can do to the machine. It can load a
// kernel it brought, run one over buffers this host chose, and ask for scratch
// this host owns. There is no device here to be handed out, no pool to be
// exhausted and no queue to be stalled. The media and model verbs go to the
// providers the program declared (Capabilities.h), and where it declared none
// they answer "not available in this host" and say why.
//
// EVERYTHING HAPPENS ON THE ONE GPU THREAD
//
// `run` must be called on the thread the program talks to its device from.
// Not a suggestion: the pool rewrites buffer handles in place and counts
// submissions in an order its arenas depend on, and a dispatch from another
// thread lands in the middle of that. The failure arrives later, as an illegal
// address inside a driver, with nothing on the stack to say where it came
// from. The program's own context is what knows which thread that is; this
// class trusts it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "aofx/Effect.h"

namespace gpe {
class PooledDevice;
}

namespace aofx::host {

struct Capabilities;

/// One render's worth of GPU access, handed to one effect and then thrown away.
///
/// Scratch is released when this dies, which is what makes `Gpu::scratch` safe
/// to call without a matching free: a plugin that forgot one would otherwise
/// leak the device a buffer per frame, and at video rates that is a stall
/// within the minute.
///
/// Not final: a program derives from it to add what is its own -- the resets,
/// the buttons, the statics its window reads -- on top of the verbs, which are
/// the same everywhere.
class EffectRunner : public aofx::Gpu {
public:
    /// Both references must outlive the runner. The device is the pool every
    /// buffer this runner hands out comes from; the capabilities say what the
    /// media and model verbs are backed by.
    EffectRunner(gpe::PooledDevice& device, const Capabilities& capabilities);
    ~EffectRunner() override;

    EffectRunner(const EffectRunner&) = delete;
    EffectRunner& operator=(const EffectRunner&) = delete;

    // --- the device verbs: the same in every program ---------------------

    [[nodiscard]] aofx::KernelId load(const std::string& name) override;
    bool run(aofx::KernelId kernel, aofx::Grid grid,
             const std::vector<aofx::Buffer>& buffers, const void* uniforms,
             size_t uniformBytes) override;
    [[nodiscard]] aofx::Buffer scratch(int width, int height) override;
    [[nodiscard]] aofx::Buffer keep(const std::string& key, const void* data,
                                    size_t bytes) override;
    void drop(const std::string& key) override;
    /// Pages the effect mapped, bound in place where the device reads host
    /// memory (a unified-memory Metal device): a buffer over the pages,
    /// adopted by the pool. Invalid elsewhere, which the contract reads as
    /// "use keep".
    [[nodiscard]] aofx::Buffer borrow(const std::string& key, const void* pages,
                                      size_t bytes) override;
    /// Invalid on every backend: importing another process's exported device
    /// memory is CUDA or Vulkan external memory, which gpe does not reach.
    /// Every caller has `keep` to fall back on.
    [[nodiscard]] aofx::Buffer importFd(const std::string& key, int fd,
                                        size_t bytes) override;
    [[nodiscard]] bool read(const aofx::Buffer& buffer, void* into, size_t bytes) override;
    void publish(const std::string& instance, const char* key, double value) override;

    // --- the media verbs: the program's provider, or "not available" -----

    [[nodiscard]] aofx::ClipId clip(const std::string& path) override;
    [[nodiscard]] aofx::ClipInfo clipInfo(aofx::ClipId) const override;
    bool decode(aofx::ClipId, int frame, const aofx::Buffer& out) override;
    bool decodeNext(aofx::ClipId, int direction, const aofx::Buffer& out,
                    bool& atStart, bool& atEnd) override;
    bool decodeAudio(aofx::ClipId, int64_t firstSample, int64_t sourceSamples,
                     const aofx::AudioFormat&, int64_t outFrames,
                     aofx::AudioBlock& out) override;
    [[nodiscard]] aofx::RecorderId recorder(const std::string& path,
                                            const aofx::RecorderDesc&) override;
    bool record(aofx::RecorderId, const aofx::Buffer& picture, double frame,
                const aofx::AudioBlock* audio) override;
    using aofx::Gpu::record;
    void closeRecorder(aofx::RecorderId) override;

    // --- the model verbs: the program's provider, or "not available" -----

    [[nodiscard]] aofx::ModelId model(const std::string& name) override;
    [[nodiscard]] aofx::ModelIo modelInput(aofx::ModelId, int index) const override;
    [[nodiscard]] aofx::ModelIo modelOutput(aofx::ModelId, int index) const override;
    bool infer(aofx::ModelId, const std::vector<aofx::Buffer>& inputs,
               const std::vector<aofx::Buffer>& outputs) override;
    [[nodiscard]] bool inferred(aofx::ModelId) override;
    bool inferShaped(aofx::ModelId, const std::vector<aofx::Buffer>& inputs,
                     const std::vector<aofx::ModelIo>& inputShapes,
                     const std::vector<aofx::Buffer>& outputs,
                     const std::vector<aofx::ModelIo>& outputShapes) override;

    // --- the engine verbs: the program's renderers, checked, or "none" -------

    [[nodiscard]] std::vector<std::string> engines() const override;
    [[nodiscard]] aofx::EngineResult render(const aofx::EngineRequest& request) override;

    // --- what the host keeps for the program --------------------------------

    /// Which effect instance this runner renders, so a model provider can note
    /// who asked and a program can scope what it keeps.
    void noteInstance(std::string instance);
    [[nodiscard]] const std::string& instance() const noexcept;

    /// The last thing that went wrong loading or running a kernel, if any.
    ///
    /// A plugin returns `false` and nothing else -- the SDK gives it no way to
    /// say why, on purpose, because a message crossing that boundary is a
    /// string a plugin allocates and the host must trust. So the host keeps
    /// what *it* knows: a kernel it could not find, and its name; a dispatch
    /// whose buffers or uniforms did not match the kernel's own reflection.
    /// Process-wide, because the runner is: one render at a time.
    [[nodiscard]] static std::string lastComplaint();
    static void complain(std::string what);

    /// `Gpu::publish`: the latest value per instance and key, process-wide. A
    /// snapshot for whoever ships a status stream.
    [[nodiscard]] static std::map<std::string, std::map<std::string, double>>
    publishedState();

    /// Lets go of every kept buffer whose key `keep` says so, releasing on
    /// `device`, which must be the pool they came from, on its thread. Returns
    /// how many went. What a program's soft reset is made of.
    static size_t dropWhere(const std::function<bool(const std::string& key)>& going,
                            gpe::PooledDevice& device);

    /// Lets go of everything kept under an instance scope -- the buffers whose
    /// key names it and the published values of its nodes. For a session that
    /// is being destroyed. `device` as for `dropWhere`.
    static void forgetScope(const std::string& scope, gpe::PooledDevice& device);

    /// How much `keep` is holding, in bytes, and how many uploads it has
    /// actually done. The second number is the one that matters: it should
    /// stop rising once a cloud is loaded. If it climbs with the frame counter
    /// the key is wrong and the bus is carrying the whole thing every frame --
    /// which will look like a slow kernel and is not one.
    [[nodiscard]] static size_t   keptBytes();
    [[nodiscard]] static uint64_t keepUploads();

    /// Dispatches issued, and scratch handed out, by this runner. A plugin
    /// asking for a new scratch buffer every frame is one the pool will be
    /// recycling rather than reusing, and the difference is visible here
    /// before it is visible in a frame time.
    [[nodiscard]] uint64_t dispatches() const noexcept;
    [[nodiscard]] uint64_t scratchBuffers() const noexcept;

    [[nodiscard]] gpe::PooledDevice& device() noexcept;
    [[nodiscard]] const Capabilities& capabilities() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace aofx::host
