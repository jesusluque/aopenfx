// Copyright (c) 2026 aopenfx contributors.
//
// What a node that delivers has to say, written down once.
//
// A Write delivers a file; an offline node delivers a side effect that takes a
// frame at a time -- a set of pictures on disk, a rectified copy, a training
// run that starts when the last one has landed. They are the same thing to the
// host: a target the batch walks a range for, in order, once each. So they
// share the range vocabulary, and this is where it lives. A document should
// not have to learn two vocabularies for one idea, and a plugin should not
// carry its own copy of five parameter descriptions that have to match the
// built-in Write's to the letter.
//
// The host resolves `first` and `last` before the request arrives -- on
// `Input` it walks the graph back to the files, which no plugin can do -- and
// writes the answer into those two, whatever `range` says. That is why
// `isLastDelivered` reads them without looking at the mode.
#pragma once

#include <string_view>

#include <cmath>
#include <string>
#include <tuple>

#include "aofx/Descriptor.h"
#include "aofx/Effect.h"

namespace aofx {

/// The delivery range parameters, in the same shape and words as the built-in
/// Write: `range` (Input / Project / Custom), `first`, `last`, `offset`,
/// `order`. `rangeDefault` is the index of the choice the node starts on --
/// 0 Input, 1 Project, 2 Custom.
inline void addDeliveryParams(EffectDesc& into, int rangeDefault = 0) {
    ParamDesc range;
    range.name = "range";
    range.label = "Frame Range";
    range.type = ParamType::Choice;
    range.choices = {{"input", "Input -- what reaches this node"},
                     {"project", "Project -- the whole timeline"},
                     {"custom", "Custom -- first and last below"}};
    range.defaults = {static_cast<double>(rangeDefault)};
    range.hint =
        "Which frames this node delivers. 'Input' is the length of what "
        "reaches it, followed back through the graph to the files it opens.";
    into.params.push_back(range);

    for (const auto& [name, label, hint] :
         {std::tuple{"first", "First Frame",
                     "First frame delivered, when the range is Custom."},
          std::tuple{"last", "Last Frame",
                     "Last frame delivered, when the range is Custom."},
          std::tuple{"offset", "Frame Offset",
                     "Added to the timeline frame to number the file."},
          std::tuple{"order", "Render Order",
                     "Which delivery goes first when a batch renders several. "
                     "Lower first."}}) {
        ParamDesc param;
        param.name = name;
        param.label = label;
        param.type = ParamType::Integer;
        param.defaults = {0.0};
        param.hardMin = {-1000000.0};
        param.hardMax = {1000000.0};
        param.hint = hint;
        // **First and Last belong to Custom, and say so.**
        //
        // Every delivery node showed them at zero whatever the range mode
        // was, and a zero is not read as "unused" -- it is read as "no
        // frames". A CameraTrack whose range says "Input" and whose First
        // Frame says 0 looks like a node that will deliver nothing, which is
        // exactly what somebody reported after pressing Solve again and
        // waiting.
        //
        // The hints already said "when the range is Custom". A hint is what
        // you read after you have been confused; this is what stops the
        // confusion.
        if (std::string_view(name) == "first" ||
            std::string_view(name) == "last") {
            param.shownWhen = ShownWhen{"range", "custom"};
        }
        into.params.push_back(param);
    }
}

/// The first number of a parameter, or `fallback` when the request does not
/// carry it.
[[nodiscard]] inline double deliveryNumber(const RenderRequest& request,
                                           const char*          name,
                                           double               fallback) {
    for (const ParamValue& param : request.params) {
        if (param.name == name && !param.numbers.empty()) {
            return param.numbers.front();
        }
    }
    return fallback;
}

/// The resolved range, as `{first, last}`; `{0, 0}` when nothing resolved it
/// -- a host that does not, or a frame outside any range.
[[nodiscard]] inline std::pair<double, double> deliveredRange(
    const RenderRequest& request) {
    return {deliveryNumber(request, "first", 0.0),
            deliveryNumber(request, "last", 0.0)};
}

/// True when the host has resolved a range and this request is inside it.
[[nodiscard]] inline bool isDelivered(const RenderRequest& request) {
    const auto [first, last] = deliveredRange(request);
    if (last < first || (first == 0.0 && last == 0.0)) {
        return false;
    }
    return request.time > first - 0.5 && request.time < last + 0.5;
}

/// True on the first frame of the resolved range.
[[nodiscard]] inline bool isFirstDelivered(const RenderRequest& request) {
    const auto [first, last] = deliveredRange(request);
    if (last < first || (first == 0.0 && last == 0.0)) {
        return false;
    }
    return std::abs(request.time - first) < 0.5;
}

/// True on the last frame of the resolved range: where a movie is closed and
/// where a job that needs every frame is started.
///
/// Both zero means nothing resolved them. A movie is then closed when the
/// script goes away instead, which is late but not wrong; a job simply never
/// starts, which is the right answer for a viewer.
[[nodiscard]] inline bool isLastDelivered(const RenderRequest& request) {
    const auto [first, last] = deliveredRange(request);
    if (last < first || (first == 0.0 && last == 0.0)) {
        return false;
    }
    return std::abs(request.time - last) < 0.5;
}

// --- a job that outlives the frame that started it --------------------------
//
// An offline node whose work takes longer than a render -- a training run, a
// solve -- says so by hanging `job.<instance>` on its output, and the host's
// batch asks the same frame again, every couple of seconds, until the job is
// done or failed. Only its own `process()` runs on each ask: what feeds it is
// cached. The node keeps the job's state somewhere that survives between
// asks (a static map by output directory is the usual place), and `process`
// does no more than read it and attach it.
//
// Cancel: the host presses the node's `cancel` Button and asks once more, and
// the node is expected to stop what it started -- the batch has no other road
// into a process the node spawned.

/// What `job.<instance>` says at slot 0.
enum class JobState { Idle = 0, Running = 1, Done = 2, Failed = 3 };

/// Why a job failed, at slot 4, for the host to put into words. Zero when it
/// did not fail or the reason is only in the node's own log.
enum class JobReason {
    None = 0,
    NoGpuMemory = 1,      ///< the card had less free than the node asks for
    NoInterpreter = 2,    ///< the python (or tool) it names does not exist
    ExitedBadly = 3,      ///< the process ran and returned non-zero
    NoDataset = 4,        ///< the input it was pointed at is not there
    NoOutput = 5,         ///< the process finished and left nothing behind
    CouldNotStart = 6     ///< spawning it failed
};

/// Hangs the job's state on this render: `{state, percent, current, total,
/// reason}`, and publishes the same four numbers for the status stream.
inline void attachJob(const RenderRequest& request, JobState state,
                      double percent, double current, double total,
                      JobReason reason = JobReason::None) {
    request.attach("job." + request.instance,
                   {static_cast<float>(static_cast<int>(state)),
                    static_cast<float>(percent), static_cast<float>(current),
                    static_cast<float>(total),
                    static_cast<float>(static_cast<int>(reason))});
    if (request.gpu != nullptr) {
        request.gpu->publish(request.instance, "job.state",
                             static_cast<double>(static_cast<int>(state)));
        request.gpu->publish(request.instance, "job.percent", percent);
        request.gpu->publish(request.instance, "job.current", current);
        request.gpu->publish(request.instance, "job.total", total);
    }
}

}   // namespace aofx
