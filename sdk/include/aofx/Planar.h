// Copyright (c) 2026 aopenfx contributors.
//
// A solved plane, and how it gets into a document.
//
// WHY IT LIVES IN THE DOCUMENT
//
// The same argument `Tracks.h` makes, and it is worth making once more
// because the numbers here are smaller and the saving is larger. Solving a
// plane needs two networks, a GPU and a render host; *using* the answer
// needs a matrix multiply. Keeping the two apart is what lets a comp open on
// a laptop that could not have solved anything, and lets somebody rewire
// everything downstream of the tracker without waking the render host.
//
// Nine floats a frame and a byte that says whether the frame was solved at
// all. Two thousand nine hundred frames of that is a hundred kilobytes,
// a hundred and forty in base64 -- the same order as an analysis this format
// already carries, so the document is not the thing to economise on.
//
// WHAT IS NOT HERE
//
// The features and the matches. They are megabytes a frame, they are worth a
// millisecond to make again, and a document is not a disk cache. What is
// stored is the *answer*: where the plane went.
//
// LITTLE-ENDIAN, AND SAID -- for the reason `Tracks.h` says it.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// For `detail::toBase64` and its opposite. One base64 in this SDK: a second
// one would be a second place for a padding bug to live.
#include "aofx/Tracks.h"

namespace aofx {

/// 'O','P','L','N'. In the first four bytes of the block, so something that
/// is not one is refused before anything is sized from its contents. Bytes
/// here rather than a float, unlike `Features.h`: this block is bytes.
inline constexpr uint32_t kPlanarMagic = 0x4E4C504FU;

/// Bumped when the layout changes. A reader refuses a version it does not
/// know rather than reading the wrong offsets.
inline constexpr uint16_t kPlanarVersion = 1;

/// Where the plane was on one frame.
struct PlanarSample {
    /// `H_reference_from_frame`, row major, normalised so the last is one.
    /// The map that takes a point of the reference plane to this frame.
    float h[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    /// 0 or 1. A frame the tracker lost is **not** the identity -- it has no
    /// answer at all, and a consumer that treated the identity as an answer
    /// would paint the sign on the frame's own corner.
    uint8_t valid = 0;
};

/// A whole solve: one homography per frame over a range.
struct Planar {
    int32_t first = 0;
    int32_t last = -1;
    /// The frame the reference plane was taken from: the frame whose
    /// homography is the identity, and the one the corners were drawn on.
    int32_t reference = 0;
    /// The picture the homographies are in, so a solve made at proxy scale
    /// still means something at full: a consumer scales by its own width
    /// over this one. The same reason `Tracks` records it.
    uint16_t width = 0;
    uint16_t height = 0;
    /// One per frame of the range, in order.
    std::vector<PlanarSample> at;

    [[nodiscard]] int32_t frames() const noexcept {
        return last >= first ? last - first + 1 : 0;
    }
    [[nodiscard]] bool empty() const noexcept { return frames() == 0 || at.empty(); }

    /// The solve on one frame, or nothing outside the range -- and nothing,
    /// too, for a frame inside it that was never solved. Both are honest
    /// answers and neither is the identity.
    [[nodiscard]] const PlanarSample* sample(int32_t frame) const noexcept {
        if (frame < first || frame > last) {
            return nullptr;
        }
        const size_t index = static_cast<size_t>(frame - first);
        if (index >= at.size() || at[index].valid == 0) {
            return nullptr;
        }
        return &at[index];
    }
};

/// The text a document stores. Empty for a solve with nothing in it.
[[nodiscard]] inline std::string writePlanar(const Planar& planar) {
    if (planar.empty()) {
        return {};
    }
    std::vector<uint8_t> raw;
    raw.reserve(24 + planar.at.size() * 37);
    detail::put(raw, kPlanarMagic);
    detail::put(raw, kPlanarVersion);
    detail::put(raw, planar.first);
    detail::put(raw, planar.last);
    detail::put(raw, planar.reference);
    detail::put(raw, planar.width);
    detail::put(raw, planar.height);
    for (const PlanarSample& one : planar.at) {
        for (const float value : one.h) {
            detail::put(raw, value);
        }
        raw.push_back(one.valid != 0 ? uint8_t{1} : uint8_t{0});
    }
    return detail::toBase64(raw);
}

/// What a document stored, or nothing.
///
/// Nothing for anything it does not recognise -- a different version, a
/// truncated block, a value somebody typed. Never a partial answer: half a
/// solve read as a whole one is a shot that drifts for a reason nobody can
/// find.
[[nodiscard]] inline Planar readPlanar(const std::string& text) {
    Planar out;
    if (text.empty()) {
        return out;
    }
    const std::vector<uint8_t> raw = detail::fromBase64(text);
    size_t at = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    if (!detail::take(raw, at, magic) || magic != kPlanarMagic) {
        return {};
    }
    if (!detail::take(raw, at, version) || version != kPlanarVersion) {
        return {};
    }
    if (!detail::take(raw, at, out.first) || !detail::take(raw, at, out.last) ||
        !detail::take(raw, at, out.reference) ||
        !detail::take(raw, at, out.width) || !detail::take(raw, at, out.height)) {
        return {};
    }
    if (out.last < out.first) {
        return {};
    }
    const size_t wanted = static_cast<size_t>(out.frames());
    // Sized from the header, then checked against what is there. Resizing to
    // a number a corrupt header claimed would be a gigabyte allocated from
    // four bad bytes, so the bytes have to be present before the room is
    // made -- `Tracks.h` learnt this the same way.
    if (raw.size() - at != wanted * 37) {
        return {};
    }
    out.at.resize(wanted);
    for (PlanarSample& one : out.at) {
        for (float& value : one.h) {
            if (!detail::take(raw, at, value)) {
                return {};
            }
        }
        one.valid = raw[at++];
    }
    return out;
}

}   // namespace aofx
