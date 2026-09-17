// Copyright (c) 2026 aopenfx contributors.
//
// The sixty-four-slot box format, written down once.
//
// A detector hands boxes to another node as **pixels**, because that is the
// only channel between two effects that costs nothing: `Gpu` has no verb for
// getting a buffer back off the device cheaply, and a plane is frame-sized
// whatever is in it. So the boxes live in the first two rows of a buffer,
// sixty-four texels wide:
//
//     row 0, texel s:  (x1, y1, x2, y2)          canonical pixels
//     row 1, texel s:  (sure, score, id, 7734)   filled, confidence, marker
//
// 7734 is there so that a *picture* wired into a Boxes port is not read as
// boxes. `meta.z <= 0` is an empty slot.
//
// THE IDENTITY IN `meta.z`, WHICH USED TO BE THE NUMBER ONE
//
// A detector finds a person; a tracker finds the SAME person again next
// frame, and that is the whole of what it adds. The number saying which
// person had nowhere to go, so it went where the "this slot is filled" flag
// was: anything above nought still means filled, so every reader written
// against the old layout reads the new one correctly and without knowing.
// A producer with no identity to give -- Detect, Locate, PersonMatte -- writes
// 1, which is what it always wrote.
//
// This file exists because that layout was spelled out by hand in five
// different `.slang` files and one comment, and a format with six copies is a
// format that is about to have two versions.
//
// WHAT `attachedBoxes` IS FOR, AND WHY IT IS WORTH A READBACK
//
// Pixels are the right channel for another *kernel* -- Sam, SamMt, David and
// Vggtslam all read these on the device and must not pay to bring them back.
// They are the wrong channel for anything that is not a kernel: an expression
// asking where a face is, a caption following it, a panel showing how many
// there are. Those need numbers on the host.
//
// So the producer reads its own box buffer back -- 512 bytes, once a frame,
// behind a network that costs tens of milliseconds -- and attaches them. The
// pixels still go out exactly as they did; this is an addition, not a
// replacement.
#pragma once

#include <cstddef>
#include <vector>

#include "aofx/Types.h"

namespace aofx {

/// How many boxes a buffer of these holds.
// Sixty-four, from sixteen on 2026-09-10: Locate finds twenty-two things in
// a frame it is asked about and had to drop six. Every reader loops to the
// capacity it is handed in its uniforms; the writers that keep a table of
// candidates in a single thread size it from here.
inline constexpr uint32_t kBoxCapacity = 64;

/// The value in `w` of the second row that says "these really are boxes".
inline constexpr float kBoxMarker = 7734.0F;

/// Floats in one box buffer: two rows of `kBoxCapacity` texels of four.
inline constexpr size_t kBoxBufferFloats = size_t{kBoxCapacity} * 2 * 4;

/// Numbers per box in the attached form.
// Seven from six on 2026-09-11, when McByte gave boxes identities. Every
// reader in the tree indexes by this constant, which is why the change is one
// line here and none anywhere else -- and why it is an ABI bump: a bundle
// built against six attaches rows of six that the host would read as rows of
// seven, and every box after the first would be numbers from the next one.
inline constexpr size_t kAttachedBoxStride = 7;

/// The attachment id a node publishes its boxes under.
///
/// Namespaced by the instance, like every other measured attachment, because
/// two detectors in one graph are two answers and a bare name would be
/// last-writer-wins down the chain.
[[nodiscard]] inline std::string boxAttachmentId(const std::string& instance) {
    return "boxes." + instance;
}

/// Turns a box buffer, read back off the device, into what travels attached.
///
/// The attached form is not the buffer's form and deliberately so:
///
///     [ count, then count * (x1, y1, x2, y2, sure, score, id) ]
///
/// Count first because the question anybody asks first is how many, and empty
/// slots left out because an expression should not have to know that sixteen
/// is the capacity. Canonical pixels either way -- the same numbers the
/// kernels read, so a box means one thing in this system.
[[nodiscard]] inline std::vector<float> attachedBoxes(
    const std::vector<float>& readBack) {
    std::vector<float> out{0.0F};
    if (readBack.size() < kBoxBufferFloats) {
        return out;
    }
    const size_t metaAt = size_t{kBoxCapacity} * 4;
    uint32_t     count = 0;
    for (uint32_t slot = 0; slot < kBoxCapacity; ++slot) {
        const float* corners = &readBack[size_t{slot} * 4];
        const float* meta = &readBack[metaAt + size_t{slot} * 4];
        // An empty slot, and a buffer that is not boxes at all. The marker is
        // checked here as well as in the kernels: a host-side reader that
        // trusted whatever was in the buffer would publish a picture's top-left
        // pixels as coordinates.
        if (meta[2] <= 0.0F || meta[3] < kBoxMarker - 1.0F ||
            meta[3] > kBoxMarker + 1.0F) {
            continue;
        }
        out.push_back(corners[0]);
        out.push_back(corners[1]);
        out.push_back(corners[2]);
        out.push_back(corners[3]);
        out.push_back(meta[0]);
        out.push_back(meta[1]);
        // The identity, and the flag, which are the same number: a producer
        // that does not track writes 1 and every box it makes is "the first
        // one", which is true -- it has no second frame to be the same in.
        out.push_back(meta[2]);
        ++count;
    }
    out[0] = static_cast<float>(count);
    return out;
}

}   // namespace aofx
