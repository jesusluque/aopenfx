// Copyright (c) 2026 aopenfx contributors.
//
// Tracked points, and how they get into a document.
//
// WHY THEY LIVE IN THE DOCUMENT AND NOT IN A FILE BESIDE IT
//
// A track is expensive to make and free to read, so it is made once and read
// for ever after. Where it is kept decides what that is worth. A file beside
// the project is a file to lose, a path to fix when the job moves, and a
// second thing to hand somebody -- while a track inside the document opens on
// a machine that cannot make one at all. That is the whole point of separating
// the two: the analysis needs a GPU and a model, and the *use* needs neither.
//
// So the numbers go in a String parameter, base64 of the block below, and they
// travel with the comp like every other value in it.
//
// WHY NOT QUANTISED
//
// Sixteen-bit fixed point over the frame would halve this. It would also cost
// a quarter of a pixel at HD, and sub-pixel accuracy is the entire reason to
// run a tracker rather than eyeball a keyframe. The size to control is the
// number of points, which the node caps and says the cost of; the precision is
// not a place to save.
//
// LITTLE-ENDIAN, AND SAID
//
// The block is written and read little-endian. Every platform this host builds
// for is little-endian, and a format that pretended otherwise would carry
// byte-swapping nobody could test.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace aofx {

/// 'O','T','R','K'. In the first four bytes, so a block that is not one is
/// refused before anything is sized from its contents.
inline constexpr uint32_t kTrackMagic = 0x4B52544FU;

/// Bumped when the layout changes. A reader refuses a version it does not
/// know rather than reading the wrong offsets -- the failure a document
/// outliving its reader is guaranteed to produce eventually.
inline constexpr uint16_t kTrackVersion = 1;

/// Where one point is on one frame.
struct TrackSample {
    float x = 0.0F;
    float y = 0.0F;
    /// 0 or 1. A point that went behind something is *not* at (0,0) -- it has
    /// no position at all this frame, and a consumer that averaged it in would
    /// drag the answer to the corner.
    uint8_t visible = 0;
};

/// A whole analysis: N points over a range of frames.
struct Tracks {
    int32_t  first = 0;
    int32_t  last = -1;
    uint16_t points = 0;
    /// The frame the points were traced in, so a track made at proxy scale
    /// still means something at full: a consumer scales by its own width over
    /// this one rather than assuming they match.
    uint16_t width = 0;
    uint16_t height = 0;
    /// Which group each point belongs to, one per point. Groups come from the
    /// boxes the points were seeded in; with no boxes every point is in group
    /// zero.
    std::vector<uint16_t> cluster;
    /// `frames * points` samples, frame-major: sample `f * points + p`.
    std::vector<TrackSample> at;

    [[nodiscard]] int32_t frames() const noexcept {
        return last >= first ? last - first + 1 : 0;
    }
    [[nodiscard]] bool empty() const noexcept {
        return points == 0 || frames() == 0;
    }
    /// One point on one frame, or nothing outside the range. Frames outside
    /// hold nothing rather than the nearest: a consumer asking about a frame
    /// this was not traced over deserves to know, not to be handed the last
    /// one and told it is current.
    [[nodiscard]] const TrackSample* sample(int32_t frame,
                                            uint16_t point) const noexcept {
        if (frame < first || frame > last || point >= points) {
            return nullptr;
        }
        const size_t at_ = static_cast<size_t>(frame - first) * points + point;
        return at_ < at.size() ? &at[at_] : nullptr;
    }
};

namespace detail {

inline constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string toBase64(const std::vector<uint8_t>& raw) {
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);
    for (size_t at = 0; at < raw.size(); at += 3) {
        const uint32_t a = raw[at];
        const uint32_t b = at + 1 < raw.size() ? raw[at + 1] : 0U;
        const uint32_t c = at + 2 < raw.size() ? raw[at + 2] : 0U;
        const uint32_t three = (a << 16) | (b << 8) | c;
        out.push_back(kBase64[(three >> 18) & 0x3FU]);
        out.push_back(kBase64[(three >> 12) & 0x3FU]);
        out.push_back(at + 1 < raw.size() ? kBase64[(three >> 6) & 0x3FU] : '=');
        out.push_back(at + 2 < raw.size() ? kBase64[three & 0x3FU] : '=');
    }
    return out;
}

inline std::vector<uint8_t> fromBase64(const std::string& text) {
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    uint32_t held = 0;
    int have = 0;
    for (const char letter : text) {
        // Whitespace is skipped so a block can be wrapped in a document
        // without becoming unreadable; anything else that is not an alphabet
        // character ends the block, which is how a truncated or corrupted
        // value refuses instead of decoding to noise.
        if (letter == '\n' || letter == '\r' || letter == ' ' ||
            letter == '\t') {
            continue;
        }
        if (letter == '=') {
            break;
        }
        const char* found = std::strchr(kBase64, letter);
        if (found == nullptr || letter == '\0') {
            return {};
        }
        held = (held << 6) | static_cast<uint32_t>(found - kBase64);
        have += 6;
        if (have >= 8) {
            have -= 8;
            out.push_back(static_cast<uint8_t>((held >> have) & 0xFFU));
        }
    }
    return out;
}

template <typename T>
void put(std::vector<uint8_t>& into, T value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    into.insert(into.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool take(const std::vector<uint8_t>& from, size_t& at, T& value) {
    if (at + sizeof(T) > from.size()) {
        return false;
    }
    std::memcpy(&value, from.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}

}   // namespace detail

/// The text a document stores. Empty for an analysis with nothing in it.
[[nodiscard]] inline std::string writeTracks(const Tracks& tracks) {
    if (tracks.empty()) {
        return {};
    }
    std::vector<uint8_t> raw;
    raw.reserve(32 + tracks.at.size() * 9);
    detail::put(raw, kTrackMagic);
    detail::put(raw, kTrackVersion);
    detail::put(raw, tracks.points);
    detail::put(raw, tracks.first);
    detail::put(raw, tracks.last);
    detail::put(raw, tracks.width);
    detail::put(raw, tracks.height);
    for (uint16_t point = 0; point < tracks.points; ++point) {
        detail::put(raw, point < tracks.cluster.size() ? tracks.cluster[point]
                                                       : uint16_t{0});
    }
    for (const TrackSample& one : tracks.at) {
        detail::put(raw, one.x);
        detail::put(raw, one.y);
        raw.push_back(one.visible != 0 ? uint8_t{1} : uint8_t{0});
    }
    return detail::toBase64(raw);
}

/// What a document stored, or nothing.
///
/// Nothing for anything it does not recognise -- a different version, a
/// truncated block, a value somebody typed. Never a partial answer: half a
/// track read as a whole one is a shot that drifts for a reason nobody can
/// find.
[[nodiscard]] inline Tracks readTracks(const std::string& text) {
    Tracks out;
    if (text.empty()) {
        return out;
    }
    const std::vector<uint8_t> raw = detail::fromBase64(text);
    size_t at = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    if (!detail::take(raw, at, magic) || magic != kTrackMagic) {
        return {};
    }
    if (!detail::take(raw, at, version) || version != kTrackVersion) {
        return {};
    }
    if (!detail::take(raw, at, out.points) ||
        !detail::take(raw, at, out.first) || !detail::take(raw, at, out.last) ||
        !detail::take(raw, at, out.width) || !detail::take(raw, at, out.height)) {
        return {};
    }
    if (out.points == 0 || out.last < out.first) {
        return {};
    }
    out.cluster.resize(out.points);
    for (uint16_t point = 0; point < out.points; ++point) {
        if (!detail::take(raw, at, out.cluster[point])) {
            return {};
        }
    }
    const size_t wanted =
        static_cast<size_t>(out.frames()) * static_cast<size_t>(out.points);
    // Sized from the header, then checked against what is there. Resizing to a
    // number a corrupt header claimed would be a gigabyte allocated from four
    // bad bytes, so the bytes have to be present before the room is made.
    if (raw.size() - at != wanted * 9) {
        return {};
    }
    out.at.resize(wanted);
    for (TrackSample& one : out.at) {
        if (!detail::take(raw, at, one.x) || !detail::take(raw, at, one.y)) {
            return {};
        }
        one.visible = raw[at++];
    }
    return out;
}

// ---------------------------------------------------------------------------
// What travels attached to a frame
// ---------------------------------------------------------------------------

/// Numbers per point in the attached form.
inline constexpr size_t kAttachedTrackStride = 6;

/// The attachment id a node publishes its tracks under.
///
/// Namespaced by the instance, like every other measured attachment, because
/// two trackers in one graph are two answers and a bare name would be
/// last-writer-wins down the chain.
[[nodiscard]] inline std::string trackAttachmentId(const std::string& instance) {
    return "tracks." + instance;
}

/// One frame of an analysis, in the form that travels down the graph.
///
///     [ points, clusters,
///       then points * (x, y, visible, cluster, startX, startY) ]
///
/// The *start* position of each point rides along with its current one, and
/// that is the whole reason a cluster works: how far a group has moved, grown
/// and turned is measured against where it began, and a consumer holding only
/// this frame would otherwise have nothing to measure from. Six floats a point
/// against two is a rounding error next to a picture.
///
/// Empty when the frame is outside the analysed range -- which is not the same
/// as a track of no points, and a consumer must be able to tell those apart.
[[nodiscard]] inline std::vector<float> attachedTracks(const Tracks& tracks,
                                                       int32_t frame) {
    if (tracks.empty() || frame < tracks.first || frame > tracks.last) {
        return {};
    }
    uint16_t clusters = 0;
    for (const uint16_t one : tracks.cluster) {
        clusters = std::max<uint16_t>(clusters, static_cast<uint16_t>(one + 1));
    }
    std::vector<float> out;
    out.reserve(2 + static_cast<size_t>(tracks.points) * kAttachedTrackStride);
    out.push_back(static_cast<float>(tracks.points));
    out.push_back(static_cast<float>(clusters));
    for (uint16_t point = 0; point < tracks.points; ++point) {
        const TrackSample* now = tracks.sample(frame, point);
        const TrackSample* began = tracks.sample(tracks.first, point);
        if (now == nullptr || began == nullptr) {
            return {};
        }
        out.push_back(now->x);
        out.push_back(now->y);
        out.push_back(now->visible != 0 ? 1.0F : 0.0F);
        out.push_back(point < tracks.cluster.size()
                          ? static_cast<float>(tracks.cluster[point])
                          : 0.0F);
        out.push_back(began->x);
        out.push_back(began->y);
    }
    return out;
}

/// What a Track node attaches when a walk finishes, turned into a `Tracks`.
///
///     [ first, last, points, width, height,
///       points * cluster,
///       frames * points * (x, y, visible) ]
///
/// The effect packs it and the *host* stores it, because an effect does not
/// own the document. Both ends read this one description, which is why it lives
/// here beside the block rather than in either of them.
///
/// Empty for anything that is not one: a short array, a header that disagrees
/// with the length, a count of nothing. Never a partial answer.
[[nodiscard]] inline Tracks tracksFromAnalysis(const std::vector<float>& packed) {
    Tracks out;
    if (packed.size() < 5) {
        return {};
    }
    out.first = static_cast<int32_t>(packed[0]);
    out.last = static_cast<int32_t>(packed[1]);
    out.points = static_cast<uint16_t>(std::max(0.0F, packed[2]));
    out.width = static_cast<uint16_t>(std::max(0.0F, packed[3]));
    out.height = static_cast<uint16_t>(std::max(0.0F, packed[4]));
    if (out.points == 0 || out.last < out.first) {
        return {};
    }
    const size_t frames = static_cast<size_t>(out.frames());
    const size_t wanted = 5 + out.points + frames * out.points * 3;
    if (packed.size() != wanted) {
        return {};
    }
    out.cluster.reserve(out.points);
    for (uint16_t point = 0; point < out.points; ++point) {
        out.cluster.push_back(
            static_cast<uint16_t>(std::max(0.0F, packed[5 + point])));
    }
    out.at.resize(frames * out.points);
    const size_t from = 5 + out.points;
    for (size_t at = 0; at < out.at.size(); ++at) {
        out.at[at].x = packed[from + at * 3];
        out.at[at].y = packed[from + at * 3 + 1];
        out.at[at].visible = packed[from + at * 3 + 2] >= 0.5F ? 1 : 0;
    }
    return out;
}

}   // namespace aofx
