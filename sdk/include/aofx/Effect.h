// Copyright (c) 2026 aopenfx contributors.
//
// The interface an effect implements, and the only way it reaches the GPU.
//
// THE PLUGIN NEVER TOUCHES THE DEVICE
//
// There is no device here, no memory pool, no command queue and no way to
// allocate anything that outlives a render. An effect says which kernel to run
// over which buffers with which uniforms, and the host runs it -- on its own
// thread, with its own pool, in its own order.
//
// That is not politeness. The pool rewrites buffer handles in place and counts
// submissions in an order its arenas depend on; a plugin dispatching from its
// own thread would corrupt that in a way that surfaces as an illegal address
// inside a driver, hours from the mistake. Handing out the device would make
// every plugin's bug the host's crash.
//
// What it costs: an effect cannot do something the host has no verb for. What
// it buys: a plugin cannot exhaust the GPU's memory, cannot stall the player's
// queue, and cannot free a buffer the viewer is still reading.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "aofx/Descriptor.h"
#include "aofx/Types.h"

namespace aofx {

/// A kernel the host has loaded. Opaque; only useful to hand back to `run`.
using KernelId = uint32_t;
inline constexpr KernelId kInvalidKernel = 0;

/// A compiled network the host holds, named the same way a kernel is.
using ModelId = uint32_t;
inline constexpr ModelId kInvalidModel = 0;

/// The shape of one of a model's inputs or outputs.
///
/// Four numbers rather than a general shape, because every vision model this
/// hosts is NCHW rank 4, and a rank-4 answer is one an effect can actually use:
/// it has to write a pre-process kernel over `width` by `height` and pack
/// `channels` planes, and a `std::vector<int64_t>` would only make it work that
/// out again at every call site.
///
/// All zero for a binding that does not exist, or a model that does not.
struct ModelIo {
    int batch = 0;
    int channels = 0;
    int height = 0;
    int width = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return batch > 0 && channels > 0 && height > 0 && width > 0;
    }
    /// In float4s, which is the unit `scratch` counts in.
    [[nodiscard]] constexpr int quads() const noexcept {
        return (batch * channels * height * width + 3) / 4;
    }
};

/// The GPU, as much of it as an effect gets.
class Gpu {
public:
    virtual ~Gpu() = default;

    // --- reading a clip the host opened --------------------------------------
    //
    // Same shape as the model verbs below and for the same reason. A decoder is
    // a large, stateful, third-party thing: it holds a file open, it keeps a
    // keyframe index, it wants to decode forward and it is very slow if it
    // cannot. None of that can live in an effect -- one Effect serves every
    // node of its type and holds no state by contract -- and none of it should:
    // a plugin with its own decoder is a plugin with its own idea of when to
    // seek, and the host is the one that knows what the playhead is doing.
    //
    // So the host owns it and the effect names it, exactly as with a network.
    // The frame lands in a device buffer, which is what makes this worth having
    // rather than just tidy: on a card with a hardware decoder the picture is
    // never on the CPU at all.

    /// Opens `path`, or hands back the one already open for it.
    ///
    /// Cheap to call every frame: the host keys these by path and keeps them.
    /// `kInvalidClip` for a file it cannot read, and an effect should refuse
    /// the render rather than produce a black frame somebody has to diagnose.
    [[nodiscard]] virtual ClipId clip(const std::string& path) = 0;

    /// What is in it. All zero for an invalid id.
    [[nodiscard]] virtual ClipInfo clipInfo(ClipId) const = 0;

    /// Decodes one frame into `out`, which must be the clip's own size.
    ///
    /// Frame zero is the first. Asking for the frame after the last one served
    /// is the fast path and the one playback takes; anything else is a seek and
    /// the host pays for it. Out of range is clamped, because a timeline is
    /// allowed to run past the end of a clip and what to show there is the
    /// node's policy rather than the decoder's.
    [[nodiscard]] virtual bool decode(ClipId, int frame, const Buffer& out) = 0;

    /// One frame on from wherever the clip is, in `direction`.
    ///
    /// For a node that is playing rather than being scrubbed: a decoder asked
    /// for "the next one" does a decode, and asked for "frame 137" does a
    /// search. On a long-GOP clip that is the difference between a frame and a
    /// second. `atEnd` and `atStart` come back true when it could not go
    /// further, so a caller can turn round without counting frames.
    [[nodiscard]] virtual bool decodeNext(ClipId, int direction,
                                          const Buffer& out, bool& atStart,
                                          bool& atEnd) = 0;

    /// The kernel by the name it was declared with. Zero if there is no such
    /// kernel, which means the plugin's build and its source disagree.
    ///
    /// Cheap to call: the host resolves once and remembers. An effect may call
    /// it every frame rather than caching an id it would then have to
    /// invalidate.
    [[nodiscard]] virtual KernelId load(const std::string& name) = 0;

    /// Runs `kernel` over `buffers`, with `uniforms` copied into the launch.
    ///
    /// The buffers are bound in the order given, and that order is the contract
    /// between this call and the kernel's declarations. Nothing checks it,
    /// because nothing can: get it wrong and the kernel reads the wrong picture.
    ///
    /// `uniforms` must match the kernel's parameter struct byte for byte. Use
    /// four-byte members in declaration order and a `static_assert` on the
    /// size; padding that differs between two compilers is a whole class of bug
    /// that only appears on the other backend.
    ///
    /// Returns false if the kernel is unknown or a buffer is not live. It never
    /// throws: an exception crossing back into the host's GPU thread is an
    /// exception on the wrong thread.
    virtual bool run(KernelId kernel, Grid grid,
                     const std::vector<Buffer>& buffers, const void* uniforms,
                     size_t uniformBytes) = 0;

    /// A temporary buffer for the duration of this render, and no longer.
    ///
    /// A separable blur needs somewhere to put its first pass. Asking the host
    /// rather than allocating means the memory comes from the same pool as
    /// everything else, is counted against the same budget, and is recycled
    /// rather than returned to the driver -- which at video rates is the
    /// difference between a temporary and a stall.
    ///
    /// An invalid Buffer back means there was no room. That is a normal answer
    /// under pressure and the effect should give up on the frame rather than
    /// render half of it.
    [[nodiscard]] virtual Buffer scratch(int width, int height) = 0;

    /// Data the effect owns, put on the device once and kept there.
    ///
    /// Everything else an effect touches arrives as a picture: an input, an
    /// output, or working space the size of one. That is enough for every
    /// effect written against this SDK so far, and it is not enough for the
    /// next kind -- a renderer whose input is a point cloud, a lookup table
    /// built on the CPU, a mesh. Those are the effect's *own* data, they are
    /// large, and re-uploading them every frame is not a slow way to do it but
    /// no way to do it at all: a Gaussian splat cloud is hundreds of megabytes.
    ///
    /// `key` names the data, and naming it is the whole mechanism. Ask twice
    /// with the same key and the second call hands back the first buffer
    /// without touching the bus -- so `data` may be null once it is known to be
    /// there. A key must identify the *contents*: a file path is not enough on
    /// its own, because the file can change under it; a path and a modification
    /// time is.
    ///
    /// The buffer is linear and counted in float4s, which is what a kernel
    /// indexes. `bytes` must be a multiple of sixteen.
    ///
    /// It is kept until `drop`, or until the process ends. There is no eviction
    /// yet and that is a real limitation rather than an oversight: the host has
    /// no way to know whether a plugin is about to ask for the same thing
    /// again, and throwing away a cloud that is about to be used is worse than
    /// holding one that is not. An effect that loads something large should
    /// drop what it replaces.
    ///
    /// Shared, and this is the part that catches people: there is one Effect
    /// instance per plugin, not per node, and several engines render at once --
    /// the prefetcher has its own. Two nodes with the same key get the same
    /// buffer, which is the point; two nodes with *different* data and the same
    /// key get each other's, which is the bug this paragraph exists to prevent.
    [[nodiscard]] virtual Buffer keep(const std::string& key, const void* data,
                                      size_t bytes) = 0;

    /// Forgets what `keep`, `borrow` or `importFd` was holding for this key,
    /// freeing it.
    virtual void drop(const std::string& key) = 0;

    /// Pages the effect has mapped, used by the device where they are.
    ///
    /// `keep` copies, and for data that changes every frame -- a picture
    /// another process paints into shared memory, sixty times a second -- the
    /// copy is the whole cost. On a device that shares memory with the host
    /// (Apple silicon) there is nothing to copy: the same pages can be bound
    /// to a kernel as they are.
    ///
    /// `pages` must be page-aligned, `bytes` a whole number of pages and of
    /// sixteen, and the mapping must stay valid until `drop(key)`: the device
    /// reads it in place, whenever a kernel bound to it runs. Keyed like
    /// `keep`, and like `keep` a second call with the same key hands back the
    /// first buffer.
    ///
    /// A second call with the same key and different `pages` or `bytes` drops
    /// the first binding and makes a new one. That is a safety net, not the
    /// way to change a mapping: by the time the effect calls with new pages it
    /// has already unmapped the old ones, and a kernel still queued may read
    /// them. To replace a mapping: `drop` the key, wait for the device (below),
    /// then unmap, map the new pages and `borrow` again.
    ///
    /// UNMAPPING. `drop` releases the binding at once, but a kernel already
    /// queued may still read the pages. Before unmapping them the effect waits
    /// for the device: `drop`, then a synchronous `read` of a few values from
    /// any buffer the device writes after that kernel -- the rule every slow
    /// node already follows. Unmapping first is a read of freed memory
    /// on the GPU, which is not an error anybody sees, only a wrong picture.
    ///
    /// An invalid Buffer where the device cannot -- a discrete GPU, a host too
    /// old -- and the effect uses `keep` instead.
    [[nodiscard]] virtual Buffer borrow(const std::string& /*key*/, const void* /*pages*/,
                                        size_t /*bytes*/) {
        return {};
    }

    /// Device memory another process exported as a file descriptor, imported.
    ///
    /// The discrete-GPU half of `borrow`: a Vulkan buffer exported as an opaque
    /// descriptor, say, holding a frame another process's GPU wrote. Imported
    /// once per key and bound in place; the descriptor stays the effect's to
    /// close. `bytes` must be a whole number of sixteen.
    ///
    /// An invalid Buffer where the device cannot import it.
    [[nodiscard]] virtual Buffer importFd(const std::string& /*key*/, int /*fd*/,
                                          size_t /*bytes*/) {
        return {};
    }

    // --- running a network the host compiled --------------------------------
    //
    // The four calls above are enough for anything an effect can express as a
    // kernel, and that is nearly everything. It is not enough for a neural
    // network: what a network is, at this level, is a graph somebody else
    // compiled, and `run` can only launch kernels this SDK's toolchain built.
    //
    // The alternative was for a plugin to link an inference runtime itself.
    // That fails on all three of the things this interface exists to protect:
    // the plugin would need a device handle it is deliberately never given, it
    // would want its own queue and get the ordering wrong, and every bundle
    // would carry its own copy of a very large library. So the host owns the
    // runtime and the plugin names a model, exactly as it names a kernel.
    //
    // WHAT MAKES THIS USABLE IN A FRAME
    //
    // `infer` queues and returns. It does not wait, and an effect must not make
    // it wait by asking `inferred` in a loop. A network that takes 60 ms cannot
    // run inside a 40 ms frame however fast the machine is; what it can do is
    // run alongside forty milliseconds and be ready two frames later. So the
    // shape of an effect that uses this is: ask whether last time's answer has
    // arrived, use it if so, start another if none is running, and produce this
    // frame from whatever it already had. The node decides how to carry an
    // answer forward -- warping it along optical flow is the usual way -- and
    // that decision is the whole of whether the graph plays.

    /// A compiled network by the name the host keeps it under. Zero if this
    /// machine does not have it, which is a normal answer: models are installed,
    /// not built in, and a viewer has none.
    [[nodiscard]] virtual ModelId model(const std::string& name) = 0;

    /// What a model takes and gives, so an effect can size its scratch and
    /// write its pre-process against the real thing rather than a number
    /// copied out of a README that has since changed.
    [[nodiscard]] virtual ModelIo modelInput(ModelId, int index) const = 0;
    [[nodiscard]] virtual ModelIo modelOutput(ModelId, int index) const = 0;

    /// Queues one pass. Returns false if the model is unknown or a buffer is
    /// the wrong size; the host says which, in the log, once.
    ///
    /// The buffers are bound in the order the model declared, like `run`. They
    /// are ordinary device buffers -- `scratch` is where they come from -- and
    /// they are read and written in place, with nothing copied.
    virtual bool infer(ModelId model, const std::vector<Buffer>& inputs,
                       const std::vector<Buffer>& outputs) = 0;

    /// Has the pass queued by the last `infer` on this model finished?
    ///
    /// A query. It asks the device and returns; it never waits. False means
    /// carry on with what you have.
    [[nodiscard]] virtual bool inferred(ModelId model) = 0;

    /// A buffer's first `bytes` bytes, brought back to the CPU. Synchronous:
    /// the device finishes what it has queued first, so this is for the small
    /// answers a model leaves in a buffer -- a pose, a count, a handful of
    /// numbers -- and never for a picture. What comes back can be attached
    /// to the output with `RenderRequest::attach`, where the host reads it.
    [[nodiscard]] virtual bool read(const Buffer& buffer, void* into, size_t bytes) = 0;

    /// `infer`, for a model whose declared shapes have holes in them.
    ///
    /// A network exported with dynamic dimensions answers `modelInput` with
    /// -1 where the size is the caller's to choose -- a vision transformer
    /// that interpolates its own position embedding takes any canvas, and its
    /// cost is the canvas squared. `infer` cannot serve such a model, because
    /// it builds the tensors from the declared shapes on purpose; this is the
    /// one place an effect is allowed to name a size, and only into the holes.
    /// A dimension the model declared is still the model's: naming a different
    /// one fails, in the log, once.
    ///
    /// Shapes are given in binding order, like the buffers, and `ModelIo` is
    /// read the way `modelInput` writes it: rank 4 as NCHW, rank 3 as N1HW.
    virtual bool inferShaped(ModelId model, const std::vector<Buffer>& inputs,
                             const std::vector<ModelIo>& inputShapes,
                             const std::vector<Buffer>& outputs,
                             const std::vector<ModelIo>& outputShapes) = 0;

    // --- telling the outside world something ---------------------------------

    /// Publishes one number of this instance's state, by name.
    ///
    /// For state a control surface needs and a parameter cannot carry: which
    /// input a Switch has ON AIR (it swaps on every Take, so no document value
    /// says it), how far a transition has run. The host keeps the latest value
    /// per instance and key and ships the lot in its status stream; nothing is
    /// rendered differently because of it, and publishing on every frame is
    /// fine -- it is a map write, not a message.
    ///
    /// The instance is passed rather than remembered because this object
    /// serves every effect on the thread, exactly like `keep`'s keys.
    virtual void publish(const std::string& /*instance*/, const char* /*key*/,
                         double /*value*/) {}

    // --- writing a movie the host holds open ---------------------------------
    //
    // The exact mirror of `clip`/`decode` above, and for the same reasons. An
    // encoder is large, stateful and third-party; it holds a file open, it
    // wants frames in order, and closing it is what makes the file playable at
    // all. None of that can live in an effect that holds no state by contract
    // and serves every node of its type.
    //
    // And the frame goes in as a **device buffer**, which is the point rather
    // than the tidiness: on a machine with a hardware encoder the picture is
    // never brought down to the processor. That is the difference between a
    // delivery that keeps up on the render host and one that does not.

    /// Opens `path` for writing, or hands back the one already open for it.
    ///
    /// Cheap to call every frame: the host keys these by path. Invalid when
    /// this machine cannot write that codec, and an effect should refuse the
    /// render naming what it asked for -- silently delivering another codec is
    /// worse than not delivering.
    [[nodiscard]] virtual RecorderId recorder(const std::string& /*path*/,
                                              const RecorderDesc&) {
        return kInvalidRecorder;
    }
    /// Writes one frame. `frame` is the timeline's, for the host's own log and
    /// for ordering; the file's own numbering is its position in the file.
    ///
    /// `audio` is the sound under that frame, or null. A recorder opened with
    /// an `audioCodec` writes exactly one frame's worth per call whatever is
    /// handed in -- silence for null, the block trimmed or padded otherwise --
    /// because the muxer cannot have a frame of picture without its sound; one
    /// opened without ignores it.
    [[nodiscard]] virtual bool record(RecorderId, const Buffer& /*picture*/,
                                      double /*frame*/, const AudioBlock* /*audio*/) {
        return false;
    }
    /// The same, for a node that has no sound to hand over.
    [[nodiscard]] bool record(RecorderId id, const Buffer& picture, double frame) {
        return record(id, picture, frame, nullptr);
    }
    /// Finishes the file. **Not optional**: a movie whose muxer never closed
    /// has no index and does not open anywhere. The node that knows its last
    /// frame calls this on it; the host also closes what is left when a script
    /// goes away, because a cancelled delivery must not leave a corpse.
    virtual void closeRecorder(RecorderId) {}

    /// A run of a clip's sound, as the project hears it.
    ///
    /// `firstSample` and `sourceSamples` are in the clip's own sound at
    /// `format.rate`, sample zero being where its first picture starts -- so
    /// for the picture at clip frame k they are `sampleStart(k, rate, clip
    /// fps)` and `sliceLength(k, ...)`, by the rule in `aofx/Audio.h`. The
    /// block comes back at `format`, exactly `outFrames` long: resampled by
    /// the host, and stretched to fit when `sourceSamples` is not
    /// `outFrames`, which is what a clip at another rate shown one picture
    /// per frame sounds like -- the sound follows the picture.
    ///
    /// `firstSample` of -1 means "the sound under the picture `decode` or
    /// `decodeNext` last served", continuing where the previous such call
    /// left off when the pictures were consecutive; the host then chooses
    /// `sourceSamples` itself. That is what a Movie playing a file on its own
    /// clock wants, and it needs no frame index.
    ///
    /// False for a clip with no sound, or one wholly past its end.
    [[nodiscard]] virtual bool decodeAudio(ClipId, int64_t /*firstSample*/,
                                           int64_t /*sourceSamples*/, const AudioFormat&,
                                           int64_t /*outFrames*/, AudioBlock& /*out*/) {
        return false;
    }
};

/// Everything one render needs.
struct RenderRequest {
    double time = 0.0;
    /// Bumped by the host's temporal reset. An effect holding state between
    /// frames outside the keep store -- a worker's memory bank, a static map
    /// -- starts over when this changes; everything kept through `keep` is
    /// already gone by the time a request carries the new number.
    uint32_t resetEpoch = 0;
    double scaleX = 1.0;
    double scaleY = 1.0;

    /// The project's frame rate, as a ratio so 30000/1001 stays exact.
    ///
    /// Carried because a delivery is written at the rate of the timeline that
    /// produced it, whatever the sources were shot at -- **the project defines
    /// the final rate** -- and the node writing a movie file is the one that
    /// has to tell the encoder so.
    int fpsNumerator = 25;
    int fpsDenominator = 1;

    /// The project's pixel aspect. Beside the rate and for the same reason: a
    /// node writing a file has to put it in the header, and nothing else in
    /// this request says what it is.
    double pixelAspect = 1.0;

    /// The project's sound, and how many samples of it this frame is.
    ///
    /// `audioFrames` is the length rule's answer for `time` -- 1920 at 25, and
    /// 1601 or 1602 at 30000/1001 -- and every block an effect hands back is
    /// brought to it. Zero channels is a project without sound; an effect
    /// then has nothing to do.
    AudioFormat audioFormat;
    int64_t     audioFrames = 0;

    /// How many frames this machine is prepared to be behind.
    ///
    /// The depth of the live jitter buffer -- what an SRT arriving out of
    /// order is given to sort itself out in, twelve frames unless the project
    /// panel says otherwise. It is here because it is the one place this
    /// system states a LATENCY BUDGET, and a node that wants to look ahead
    /// before it answers should spend that budget rather than invent a second
    /// one. A pose filter with a window can throw a spike away for certain
    /// instead of guessing, and the price is exactly this many frames.
    int bufferFrames = 0;

    /// True when this render is a delivery rather than somebody looking.
    ///
    /// A node with a side effect must not do it while a viewer is scrubbing:
    /// that is how looking at a graph overwrites a shot. The host already
    /// draws this line for its own Write node (`RenderEngine::setWritesEnabled`)
    /// and an effect that writes files needs the same answer.
    bool writesEnabled = false;

    /// True while the host is walking a range through this node, in order, so
    /// an effect can do the one thing this host otherwise forbids.
    ///
    /// WHY A TRACKER NEEDS THIS AND FLOW DOES NOT
    ///
    /// `ClipDesc::temporal` covers an effect that wants *a* neighbouring frame:
    /// it names the offsets and the host has them ready. A tracker wants
    /// something else -- every frame of a range, once each, in order -- and no
    /// list of offsets says that. It is also the one thing the ordinary render
    /// path cannot promise: this host prefetches and drops frames, so "the
    /// frame after the one I just saw" is a fact about the machine's load.
    ///
    /// So the host walks the range itself and says so here. An effect that sees
    /// this may feed a model with state; an effect that does not see it must
    /// behave as though every frame were the first, because it might be.
    ///
    /// Off by default and for the same reason `writesEnabled` is: engines are
    /// made for the viewer, for every prefetch worker and for thumbnails, and
    /// an analysis running because somebody looked at a graph is an analysis
    /// nobody asked for.
    bool analysing = false;

    /// The first and last frame of the walk, while `analysing`. An effect knows
    /// it is at the beginning when `time` is `analysisFirst` -- which is where
    /// it seeds -- and at the end when it is `analysisLast`, which is where it
    /// hands the answer back.
    ///
    /// Carried rather than left to the effect's own range parameters: the
    /// walk's range is what the host actually rendered, and an effect
    /// comparing against its own knob would be comparing against a number
    /// somebody could change halfway through.
    double analysisFirst = 0.0;
    double analysisLast = 0.0;

    /// Which node this is, as a path through the groups: "Grade/Depth1".
    ///
    /// There is one Effect instance per plugin and not per node -- that is the
    /// whole reason effects are stateless -- and for nearly everything that is
    /// right: a render's inputs, outputs and parameters all arrive in this
    /// object, so the effect has nothing to remember.
    ///
    /// A network breaks that, and not by carelessness. Its answer arrives
    /// frames after it was asked for, so something has to hold the buffer it
    /// will land in, and that something belongs to *this node* rather than to
    /// the plugin. Without a name for the node there is nothing to key it on:
    /// `keep` is shared by key across every node of the type, so two Depth
    /// nodes in one graph would write into each other's working memory --
    /// intermittently, in proportion to how far apart their cadences drifted,
    /// which is the worst way for a bug to behave.
    ///
    /// Stable across frames and unique within a document, which is exactly what
    /// a key needs. Empty from a host that does not fill it in, and an effect
    /// that needs one should refuse rather than share.
    std::string instance;

    /// The part of the output being asked for, and the whole picture it is part
    /// of. They differ when the host is rendering in tiles: an effect that
    /// treats the window's edge as the picture's edge produces a seam exactly
    /// where nobody expects one.
    Rect renderWindow;
    Rect outputRod;

    std::vector<InputPlane>  inputs;
    std::vector<OutputPlane> outputs;
    std::vector<ParamValue>  params;

    /// Never null during `process`.
    Gpu* gpu = nullptr;

    /// The project's format, in full-resolution pixels, whatever `scaleX` and
    /// `scaleY` are.
    ///
    /// A generator draws for the frame, and until this it had to work the
    /// frame out -- from a size parameter somebody set to match, or from the
    /// shape of the window the host clamps "everywhere" to. Zero only from a
    /// host too old to say.
    int projectWidth = 0;
    int projectHeight = 0;

    /// Why `process` returned false, in a sentence somebody can act on.
    ///
    /// Mutable for the reason `produced` is. The host shows it where it would
    /// otherwise say only that the node could not render: "the weights file
    /// named in Model does not exist" rather than "'org.example.effect' could
    /// not render". Ignored when `process` succeeds.
    mutable std::string complaint;

    /// Numbers to hang on the picture this render produces.
    ///
    /// Mutable, and the only mutable thing in the request, because this is how
    /// a node that works something out says so: a tracker's homography, a
    /// camera solve, an exposure it measured. Whatever is left here when
    /// `process` returns travels with the output through the cache, the crop
    /// and the wire, and arrives at a node downstream as `InputPlane::values`.
    ///
    /// Small. Eight floats is a homography and sixteen is a camera; this is not
    /// a channel for a field, and a node that wants to hand on something the
    /// size of a picture should hand on a plane instead.
    mutable std::vector<std::pair<std::string, std::vector<float>>> produced;

    /// Says `id` is worth `values` to whatever comes next.
    void attach(std::string id, std::vector<float> values) const {
        for (auto& [name, held] : produced) {
            if (name == id) {
                held = std::move(values);
                return;
            }
        }
        produced.emplace_back(std::move(id), std::move(values));
    }

    /// The sound of the picture this render produces.
    ///
    /// Mutable for the reason `produced` is. Left unset (`rate == 0`), the
    /// host applies `EffectDesc::audio`; set, it is what travels with the
    /// output -- brought to the project's format and to `audioFrames` samples
    /// first. An effect that wants silence where the rule would give sound
    /// sets a block with `kSilent` and no samples.
    mutable AudioBlock audio;

    void setAudio(AudioBlock block) const { audio = std::move(block); }

    // --- convenience, so every effect does not write these ------------------

    [[nodiscard]] const InputPlane* input(const std::string& clip,
                                          const std::string& plane = "Color") const {
        for (const InputPlane& in : inputs) {
            if (in.clip == clip && in.plane == plane) {
                return &in;
            }
        }
        return nullptr;
    }

    /// The same clip at another frame, for an effect whose `ClipDesc` asked
    /// for one. Null when it did not ask, or when the frame it asked for was
    /// clamped onto the one being rendered at the end of the clip.
    ///
    /// `input()` deliberately still answers with the frame being rendered
    /// however many others arrived, so adding a frame to a descriptor cannot
    /// change what an existing lookup returns.
    [[nodiscard]] const InputPlane* inputAt(
        const std::string& clip, double when,
        const std::string& plane = "Color") const {
        for (const InputPlane& in : inputs) {
            const double gap = in.time - when;
            if (in.clip == clip && in.plane == plane && gap < 1e-6 &&
                gap > -1e-6) {
                return &in;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const OutputPlane* output(const std::string& plane = "Color") const {
        for (const OutputPlane& out : outputs) {
            if (out.plane == plane) {
                return &out;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const ParamValue* param(const std::string& name) const {
        for (const ParamValue& value : params) {
            if (value.name == name) {
                return &value;
            }
        }
        return nullptr;
    }

    /// A parameter's value, or the fallback if the host did not send one.
    ///
    /// A missing parameter is not an error: a script written against an older
    /// version of an effect simply does not mention a control added since, and
    /// the effect should behave as though it were at its default.
    [[nodiscard]] double number(const std::string& name, double fallback,
                                size_t component = 0) const {
        const ParamValue* value = param(name);
        return value == nullptr ? fallback : value->number(component, fallback);
    }

    /// Whether an effect that only does its expensive work every `every`
    /// frames should do it on this one.
    ///
    /// On the frame number rather than on a counter the effect keeps, so that
    /// scrubbing, looping and rendering out of order all pick the same frames.
    /// A counter would make the answer depend on how many times the node
    /// happened to be asked, which is not a property of the shot -- two viewers
    /// open on the same graph would each advance it.
    ///
    /// **Staggered by node.** Without the offset every node at the same cadence
    /// runs on the same frames, and a graph with two networks in it pays for
    /// both of them on one frame out of three and for neither on the other two.
    /// That lowers the average and leaves the peak exactly where it was, and it
    /// is the peak that drops frames: measured on two model nodes at cadence 3,
    /// p90 went 16.61 -> 16.35 ms, which is nothing. Spreading them by a hash
    /// of the node's path is enough, costs nothing, and is stable across runs
    /// and across machines -- which matters, because a render host and a viewer
    /// disagreeing about which frames are cheap would show as a stutter that
    /// only happens when someone is watching.
    [[nodiscard]] bool due(int every) const {
        if (every <= 1) {
            return true;
        }
        // FNV-1a over the node's path. Any spreading hash would do; this one is
        // four lines and has no dependency.
        uint64_t hash = 1469598103934665603ULL;
        for (const char letter : instance) {
            hash ^= static_cast<unsigned char>(letter);
            hash *= 1099511628211ULL;
        }
        const auto phase = static_cast<int64_t>(hash % static_cast<uint64_t>(every));
        auto frame = static_cast<int64_t>(time);
        if (time < 0.0 && static_cast<double>(frame) != time) {
            --frame;   // a cast truncates towards zero; a frame number floors
        }
        // Not `%` alone: it is negative for negative frames in C++, and a
        // timeline is allowed to start before zero.
        const int64_t at = ((frame + phase) % every + every) % every;
        return at == 0;
    }
};

/// What an effect must implement.
///
/// One object per effect *kind*, not per node: it is asked to describe itself
/// once and then to process any number of frames, possibly from different
/// nodes. So it must hold no per-node state -- everything about a render
/// arrives in the request.
///
/// That is a real constraint and a deliberate one. A stateless effect can be
/// run for two nodes at once, cached on its inputs, and re-run for a frame the
/// host threw away, none of which the host has to ask permission for.
class Effect {
public:
    virtual ~Effect() = default;

    /// Filled in once, when the plugin is loaded.
    virtual void describe(EffectDesc& into) = 0;

    /// The compiled kernels this effect brings. Registered with the device
    /// before the first render, so `Gpu::load` can find them by name.
    [[nodiscard]] virtual std::vector<KernelDesc> kernels() const = 0;

    /// How big the output is, given how big the inputs are.
    ///
    /// Answered from rectangles alone, before any pixels exist -- deciding how
    /// much of an input to render requires knowing how big this effect's output
    /// is, and rendering the input first to find out defeats the purpose.
    ///
    /// The default is the union of the inputs, which is right for anything that
    /// does not move or spread the picture. A blur genuinely creates picture
    /// outside its input -- that is what the soft edge is -- and one that does
    /// not say so gets cropped to its input and loses it.
    /// The same question, told what scale this render is being made at.
    ///
    /// WHY THERE ARE TWO
    ///
    /// The three-argument one below is the one nearly every effect wants: its
    /// answer is a function of the input rectangles, and those arrive already
    /// at the render scale, so it is scale-correct without knowing the scale.
    ///
    /// An effect that makes its *own* picture out of size knobs is the
    /// exception, and it was silently broken: `width` and `height` are
    /// canonical pixels a person typed, and returning them unscaled made a
    /// node hand back a full-size picture when the host had asked for a proxy.
    /// This host renders reduced while somebody drags, so the effect was a
    /// scene that zoomed for exactly as long as the gesture lasted and sprang
    /// back when it ended -- with nothing in any log, because nothing failed.
    ///
    /// Additive on purpose: this defaults to the old question, so every effect
    /// that does not care is unchanged and untouched.
    [[nodiscard]] virtual Rect regionOfDefinition(
        double time, double /*scaleX*/, double /*scaleY*/,
        const std::vector<Rect>& inputRods,
        const std::vector<ParamValue>& params) const {
        return regionOfDefinition(time, inputRods, params);
    }

    /// The region question with the host in it.
    ///
    /// A source's region is the size of its file, and the file is something
    /// only the host can open -- there is no `Gpu` when regions are asked,
    /// because regions are asked before any pixels exist. So the question
    /// carries `clipInfo`: the same answer `Gpu::clipInfo` gives at render
    /// time, for a path, cached by the host so that asking is free after the
    /// first time. A source that answered the project's frame because it
    /// could not know better made every box above it wrong: a Crop cut where
    /// there was no picture, a Transform moved a rectangle that was not the
    /// image's, and the gizmo in the viewer stood on neither.
    ///
    /// Empty when the host cannot say -- a path that does not open, a build
    /// with no decoder -- and the effect falls back to whatever it answered
    /// before.
    struct RegionQuery {
        double time = 0.0;
        double scaleX = 1.0;
        double scaleY = 1.0;
        const std::vector<Rect>* inputRods = nullptr;
        const std::vector<ParamValue>* params = nullptr;
        /// The clip at `path`, or an invalid `ClipInfo`. May be empty.
        std::function<ClipInfo(const std::string& path)> clipInfo;
    };

    /// Defaults to the five-argument question, so every effect that does not
    /// open files is unchanged.
    [[nodiscard]] virtual Rect regionOfDefinition(const RegionQuery& query) const {
        static const std::vector<Rect> noRods;
        static const std::vector<ParamValue> noParams;
        return regionOfDefinition(query.time, query.scaleX, query.scaleY,
                                  query.inputRods != nullptr ? *query.inputRods : noRods,
                                  query.params != nullptr ? *query.params : noParams);
    }

    [[nodiscard]] virtual Rect regionOfDefinition(
        double /*time*/, const std::vector<Rect>& inputRods,
        const std::vector<ParamValue>& /*params*/) const {
        Rect out;
        bool first = true;
        for (const Rect& rod : inputRods) {
            if (rod.isEmpty()) {
                continue;
            }
            if (first) {
                out = rod;
                first = false;
                continue;
            }
            out.x1 = rod.x1 < out.x1 ? rod.x1 : out.x1;
            out.y1 = rod.y1 < out.y1 ? rod.y1 : out.y1;
            out.x2 = rod.x2 > out.x2 ? rod.x2 : out.x2;
            out.y2 = rod.y2 > out.y2 ? rod.y2 : out.y2;
        }
        return out;
    }

    /// How much of each input this effect needs to fill `output`.
    ///
    /// The default is "the same rectangle", which is right for anything that
    /// reads a pixel to write the pixel under it -- a grade, a merge, a keyer.
    /// It is wrong for anything that *moves* the picture, and wrong in a way
    /// that is easy to miss: the host intersects what is asked for with what
    /// the input has, so a corner pin that asks for the rectangle it is
    /// producing gets the part of its source that happens to lie under that
    /// rectangle, which for a quad somewhere else in the frame is nothing at
    /// all or, worse, part of it.
    ///
    /// A spreading effect -- a blur -- needs it too, and this comment used to
    /// say otherwise. Its grown region of definition covers a render of the
    /// *whole* picture, but a host does not always ask for the whole picture: a
    /// viewer asks for its window, a tiled render for one tile. A pixel at the
    /// edge of that rectangle is a sum over neighbours outside it, so a blur
    /// asks for the output grown by its reach, at the render's scale. A
    /// *moving* effect needs it for a different reason: there is no
    /// relationship at all between where its output is and where its input is.
    ///
    /// Return one rectangle per input, in clip order. Anything empty is read as
    /// "the same as the output", so an effect can answer for one input and
    /// leave the rest alone.
    [[nodiscard]] virtual std::vector<Rect> regionOfInterest(
        double /*time*/, const Rect& output,
        const std::vector<Rect>& inputRods,
        const std::vector<ParamValue>& /*params*/) const {
        return std::vector<Rect>(inputRods.size(), output);
    }

    /// The same question with the render's scale in it -- the rectangles
    /// arrive at that scale and a knob in canonical pixels has to be brought
    /// to it before it can say where the input is. Defaults to the question
    /// above, so every effect that does not move pixels is unchanged.
    [[nodiscard]] virtual std::vector<Rect> regionOfInterest(
        const RegionQuery& query, const Rect& output) const {
        static const std::vector<Rect> noRods;
        static const std::vector<ParamValue> noParams;
        return regionOfInterest(query.time, output,
                                query.inputRods != nullptr ? *query.inputRods : noRods,
                                query.params != nullptr ? *query.params : noParams);
    }

    /// True when this render would hand back its input unchanged.
    ///
    /// Free to answer and worth answering: a blur of zero, a grade at unity, a
    /// transform at identity. The host then passes the input straight through
    /// and never allocates an output at all, which is most of what makes
    /// scrubbing a graph of defaults cheap.
    [[nodiscard]] virtual bool isIdentity(const RenderRequest& /*request*/) const {
        return false;
    }

    /// Do the work. False if the frame could not be produced.
    ///
    /// Called on the host's GPU thread, one render at a time. Returning false
    /// fails the frame with a message the host supplies; there is no way to
    /// half-render, because a picture that is half right is worse than one that
    /// is missing.
    [[nodiscard]] virtual bool process(const RenderRequest& request) = 0;
};

}   // namespace aofx
