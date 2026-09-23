// Copyright (c) 2026 aopenfx contributors.
//
// The small types that cross the boundary.
//
// Deliberately its own vocabulary rather than the host's. An SDK that included
// a host's own image header would drag in the host's allocator, its result
// type and its dependencies, and a plugin that linked the host would not be a plugin.
// These are plain structs with the same *meaning* as the host's, and the host
// converts at the one place they meet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "aofx/Audio.h"

namespace aofx {

/// Half-open, in pixels, y up. The same convention as everything else in this
/// application and as OpenFX: (0,0) is the bottom left.
struct Rect {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;

    [[nodiscard]] constexpr int  width() const noexcept { return x2 - x1; }
    [[nodiscard]] constexpr int  height() const noexcept { return y2 - y1; }
    [[nodiscard]] constexpr bool isEmpty() const noexcept {
        return x2 <= x1 || y2 <= y1;
    }
};

/// A picture in device memory.
///
/// Always float32 RGBA, linear, premultiplied. One internal format, so a kernel
/// never branches on depth or channel count -- and `stride` is in **pixels**,
/// not bytes, because that is the unit a kernel indexes in.
///
/// `device` is opaque on purpose. It is the host's handle to a buffer, and a
/// plugin can do exactly two things with it: name it in a dispatch, and pass it
/// back. There is no way to read or free it from here, which is what stops a
/// plugin outliving or double-freeing the host's memory.
struct Buffer {
    uint64_t device = 0;
    int      width = 0;
    int      height = 0;
    int      stride = 0;

    /// Where this buffer sits, in image coordinates.
    ///
    /// Not decoration. An effect's output is very often a different size from
    /// its input -- a blur's region of definition is bigger than what it was
    /// given, because that is what the soft edge is -- and a kernel that walks
    /// the output while indexing the input by the same coordinates reads off
    /// the end of every row and wraps onto the next. It looks like the picture
    /// repeating down the side, and at a large radius it swallows the frame.
    ///
    /// So the two are related by their rectangles, and the effect works out
    /// the offset. `width`/`height` always match `rect`; both are here because
    /// a kernel wants the extent and the host wants the position.
    Rect rect;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return device != 0 && width > 0 && height > 0;
    }
};

/// A clip the host has open. Opaque; only useful to hand back to `decode`.
using ClipId = uint32_t;
inline constexpr ClipId kInvalidClip = 0;

/// What a clip holds. All zero for one that could not be opened.
struct ClipInfo {
    int width = 0;
    int height = 0;
    /// How many frames. One for a still, which is a legitimate clip.
    int frames = 0;
    /// Numerator and denominator, so 30000/1001 is exact and 29.97 is not.
    int fpsNumerator = 0;
    int fpsDenominator = 1;
    /// A broadcast rather than a recording.
    ///
    /// `frames` is a fiction for one of these -- there is no last frame, and
    /// the host reports one so that every caller written for a file keeps
    /// working -- and `decode` hands over the newest picture whatever frame is
    /// asked for. An effect that wants to say "live" in its interface reads
    /// this; one that does not can ignore it and still renders.
    bool live = false;

    /// Whether the clip has sound, and what shape the file's own track is.
    /// The host hands sound over at the *project's* rate and channel count
    /// whatever these say; they are for an effect that wants to tell the
    /// user. A file that is only sound -- a wav, an mp3 -- has these and no
    /// picture, so `isValid()` is false for it and `hasAudio` is true.
    bool hasAudio = false;
    int  audioRate = 0;
    int  audioChannels = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return width > 0 && height > 0 && frames > 0;
    }
};

/// A movie file the host has open for **writing**. The mirror of `ClipId`.
using RecorderId = uint32_t;
inline constexpr RecorderId kInvalidRecorder = 0;

/// How to write a movie: what an effect asks the host for.
///
/// The codec is named by id -- "h265", "h264", "prores" -- and never by
/// position in a list, because the list of what a machine can write is not the
/// same on two machines: the render host encodes on an NVIDIA card and a
/// workstation through VideoToolbox. A number would mean a different codec
/// depending on where the document was opened. Ask the host what it has
/// and refuse by name when it
/// does not have it.
struct RecorderDesc {
    std::string codec = "h265";
    int  width = 0;
    int  height = 0;
    /// The rate the file declares. **The project's**, not the source clip's: a
    /// delivery is written at the rate of the timeline that produced it.
    int  fpsNumerator = 25;
    int  fpsDenominator = 1;
    /// Target, in kbit/s. Ignored by codecs that do not work that way.
    int  bitrateKbps = 20000;

    /// How many bits a channel gets, for a still or a sequence: "half",
    /// "float", "uint8" or "uint16". Ignored by a movie, where the codec
    /// decides.
    ///
    /// Half by default, and that is the right default rather than a cautious
    /// one: an EXR of halves is half the size and carries more range than any
    /// display will ever show. Float is for a pass that will be graded again
    /// -- a depth, a position, a normal -- where the rounding compounds. The
    /// integer depths are for a PNG or a TIFF that something else will read
    /// as a photograph: they get the sRGB curve, which the float ones do not.
    std::string depth = "half";

    /// The project's pixel aspect, written into the file's header.
    ///
    /// Not a property of the pixels: a 2.0 anamorphic delivery has square
    /// samples and a header that says they are not, and a file written without
    /// it opens narrow everywhere downstream.
    double pixelAspect = 1.0;

    /// The sound track, if any: "aac", "pcm", "opus", or empty for none.
    ///
    /// Named like the codec is, and for the same reason; the host refuses by
    /// name what this machine cannot write. `pcm` is exact and is what a
    /// ProRes delivery wants; `aac` is what an H.26x one does. The rate and
    /// channel count are the project's, which every block that arrives at
    /// `record` already has.
    std::string audioCodec;
    int  audioRate = 48000;
    int  audioChannels = 2;
    int  audioBitrateKbps = 192;
};

/// One plane of one input clip, at one time.
///
/// The plane id is a string because that is what the rest of the world calls
/// them -- "Color", "Backward", "MyLayer" -- and because a fixed enumeration
/// could never hold an arbitrary EXR layer, which is exactly the case that
/// matters.
struct InputPlane {
    std::string clip;
    std::string plane = "Color";
    double      time = 0.0;
    Buffer      buffer;
    /// The whole picture this buffer is a piece of, in this render's pixels
    /// -- the input's region of definition, as `RenderRequest::outputRod` is
    /// the output's. The buffer is what the effect asked for of it, which is
    /// less when the effect wanted a corner and could be more; an effect
    /// that pivots on "the middle of the picture" wants this rectangle and
    /// not the buffer's, or its pivot moves with what it happened to ask
    /// for. Empty from a host that does not fill it in.
    Rect        rod;

    /// Numbers that travelled with this picture: a track, a camera, a
    /// measurement some node upstream made. Empty is the ordinary case.
    ///
    /// A consumer should treat one as *better information than its own
    /// parameters*, and fall back to them when it is absent -- that is what
    /// lets the same node be driven by a tracker or by hand without being two
    /// nodes.
    std::vector<std::pair<std::string, std::vector<float>>> values;

    /// The numbers under `id`, or null.
    [[nodiscard]] const std::vector<float>* value(const std::string& id) const {
        for (const auto& [name, held] : values) {
            if (name == id) {
                return &held;
            }
        }
        return nullptr;
    }

    /// The sound under this picture at `time`, or null: a still, a project
    /// without sound, a node upstream that made none. On the "Color" plane
    /// only. Valid for the duration of `process`. See `aofx/Audio.h`.
    const AudioBlock* audio = nullptr;
};

/// One plane the effect is expected to write.
///
/// The first is always the picture. The rest are whatever the effect declared:
/// a matte, a depth, a motion vector field, a per-object id -- the same idea a
/// renderer calls an arbitrary output variable, and for the same reason. An
/// effect that can produce them cheaply while it is already reading the pixels
/// should not have to be run again to get them.
struct OutputPlane {
    std::string plane = "Color";
    Buffer      buffer;
};

/// What a parameter is holding at the frame being rendered.
///
/// Animation is evaluated by the host before this arrives: a plugin sees a
/// number, never a curve. That is one fewer thing for every effect to get
/// right, and it is what lets the host cache on the evaluated value.
struct ParamValue {
    std::string         name;
    std::vector<double> numbers;   ///< empty for a string parameter
    std::string         text;      ///< empty for a numeric one

    [[nodiscard]] double number(double fallback = 0.0) const noexcept {
        return numbers.empty() ? fallback : numbers.front();
    }
    [[nodiscard]] double number(size_t at, double fallback) const noexcept {
        return at < numbers.size() ? numbers[at] : fallback;
    }
};

/// Only shown while another parameter has a particular value.
///
/// Here rather than in `Descriptor.h` because a gizmo (`aofx/Gizmo.h`) obeys
/// the same rule as the row it edits, and both headers need it.
///
/// Declarative, and read by the host: AOFX has no `paramChanged` action and
/// this does not invent one. An effect says what a control is *for* and the
/// host decides when it is worth a row -- which is the same division as
/// `ParamRole`, where the effect says "this pair is a place" and never touches
/// the toolkit.
///
/// It hides the handle as well as the row. Eight draggable points on a picture,
/// six of which the node is currently ignoring, is not a panel problem.
struct ShownWhen {
    /// The controlling parameter, by name. Empty means always shown.
    std::string param;
    /// The value it must have. For a Choice this is the option's `value`
    /// string; for anything else, the number written out.
    std::string is;
};

/// How a dispatch is shaped: one thread per output pixel, normally.
struct Grid {
    uint32_t x = 1;
    uint32_t y = 1;
    uint32_t z = 1;
};

}   // namespace aofx
