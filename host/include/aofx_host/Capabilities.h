// Copyright (c) 2026 aopenfx contributors.
//
// What a program brings to the host, and what it does not.
//
// The host answers every verb of `aofx::Gpu`. The device verbs -- kernels,
// scratch, keep, borrow, read, publish -- it answers itself, on gpe, and they
// are the same in every program. The media verbs (a clip the host opened, a
// movie it holds open, the sound under a frame) and the model verbs (a network
// the host compiled) are a decoder and an inference runtime, and those are the
// program's business: a player brings a media stack and an inference stack, a
// renderer brings neither.
//
// So the program declares what it has. A verb it declared nothing for answers
// "not available in this host" and says why -- from this one host, not from a
// second copy of it with the verb cut out. Two copies of a host are two hosts,
// and a bundle that loads in one and not the other is the bug nobody finds.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "aofx/Effect.h"
#include "aofx/Types.h"

namespace aofx::host {

/// Where the host's own lines go.
///
/// Three levels and no formatting: every message is a sentence the host has
/// already written. Left unset, they go to stderr, which is the right answer
/// for a command line and the wrong one for a window -- so a window sets one.
class Logger {
public:
    virtual ~Logger() = default;
    virtual void info(const std::string& line);
    virtual void warn(const std::string& line);
    virtual void error(const std::string& line);
};

/// Playing and writing media: the host side of `Gpu::clip`, `decode`,
/// `decodeNext`, `decodeAudio`, `recorder`, `record` and `closeRecorder`.
///
/// The contract is the SDK's, verb for verb; what a provider adds is the
/// decoder behind it. Ids are the provider's to mint and to keep valid for the
/// life of the process, which is what an effect that holds one expects.
class MediaProvider {
public:
    virtual ~MediaProvider() = default;

    [[nodiscard]] virtual ClipId   clip(const std::string& path) = 0;
    [[nodiscard]] virtual ClipInfo clipInfo(ClipId) const = 0;
    virtual bool decode(ClipId, int frame, const Buffer& out) = 0;
    virtual bool decodeNext(ClipId, int direction, const Buffer& out, bool& atStart,
                            bool& atEnd) = 0;
    virtual bool decodeAudio(ClipId, int64_t firstSample, int64_t sourceSamples,
                             const AudioFormat&, int64_t outFrames, AudioBlock& out) = 0;

    [[nodiscard]] virtual RecorderId recorder(const std::string& path, const RecorderDesc&) = 0;
    virtual bool record(RecorderId, const Buffer& picture, double frame,
                        const AudioBlock* audio) = 0;
    virtual void closeRecorder(RecorderId) = 0;
};

/// Running a network the host compiled: the host side of `Gpu::model`,
/// `modelInput`, `modelOutput`, `infer`, `inferShaped` and `inferred`.
///
/// `askedBy` names the effect instance asking, so a provider that lets go of
/// models can let go of the ones a given node used. Ids are the provider's,
/// valid for the life of the process; a model it has let go of answers false
/// to everything and is asked for by name again.
class ModelProvider {
public:
    virtual ~ModelProvider() = default;

    [[nodiscard]] virtual ModelId model(const std::string& name,
                                        const std::string& askedBy) = 0;
    [[nodiscard]] virtual ModelIo modelInput(ModelId, int index) const = 0;
    [[nodiscard]] virtual ModelIo modelOutput(ModelId, int index) const = 0;
    virtual bool infer(ModelId, const std::vector<Buffer>& inputs,
                       const std::vector<Buffer>& outputs) = 0;
    virtual bool inferShaped(ModelId, const std::vector<Buffer>& inputs,
                             const std::vector<ModelIo>& inputShapes,
                             const std::vector<Buffer>& outputs,
                             const std::vector<ModelIo>& outputShapes) = 0;
    [[nodiscard]] virtual bool inferred(ModelId) = 0;
};

/// What this program brings. Null means "not available", and the runner says
/// so by name, once per verb per render at most.
///
/// The object must outlive every runner and registry made from it: a static
/// in the program is the natural home, since there is one host per process.
struct Capabilities {
    Logger*        log = nullptr;
    MediaProvider* media = nullptr;
    ModelProvider* models = nullptr;
    /// An identifier in an older spelling, mapped to the one the bundle
    /// declares, so a document that has not caught up still finds its effect.
    /// Identity when unset.
    std::function<std::string(const std::string&)> canonicalId;

    /// What is here, for a listing: "kernels", "keep", "borrow", "media",
    /// "models" -- the last two only when a provider is set.
    [[nodiscard]] std::vector<std::string> names() const;
};

}   // namespace aofx::host
