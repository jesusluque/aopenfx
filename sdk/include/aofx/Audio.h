// Copyright (c) 2026 aopenfx contributors.
//
// Sound, as it crosses the boundary.
//
// AUDIO RIDES ON THE PICTURE
//
// This host has no audio timeline. The samples under a frame are a property
// of that frame, the way a matte or a tracked corner is: they hang off the
// picture and go wherever it goes -- through the cache, a crop, a sub-pass at
// another time, the prefetcher, the remote wire. A picture that is delayed
// carries its sound with it and a picture that is dropped drops it, which is
// the whole of the synchronisation argument, and the reason an effect never
// sees a stream: it sees the sound of the frame it is rendering, and hands
// back the sound of the frame it made.
//
// An effect that says nothing about sound gets the host's rule: the sound of
// its inputs, summed (`EffectDesc::audio`). One that changes it -- a gain, a
// mix, a node that reads a file -- fills `RenderRequest::audio`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aofx {

/// The project's sound: a rate and a channel count. Zero channels is a
/// project with no sound.
struct AudioFormat {
    int rate = 48000;
    int channels = 2;   ///< 0..8
};

/// The samples under one frame: interleaved float, `frames() * channels`.
///
/// The same shape as the host's own block, so it crosses by copy of a few
/// kilobytes and nothing else.
struct AudioBlock {
    enum Flags : uint32_t {
        /// The first sample does not follow the last of the previous frame's:
        /// a loop wrapped, a source was cut to.
        kCut = 1u,
        /// Every sample is zero.
        kSilent = 2u,
    };

    int                rate = 0;   ///< 0 = not set
    int                channels = 0;
    uint32_t           flags = 0;
    /// The host's identity for the block; an effect leaves it alone and the
    /// host mints one for whatever comes back changed.
    uint64_t           serial = 0;
    std::vector<float> samples;

    [[nodiscard]] size_t frames() const noexcept {
        return channels > 0 ? samples.size() / static_cast<size_t>(channels) : 0;
    }
    [[nodiscard]] bool isSet() const noexcept { return rate > 0 && channels > 0; }
};

// --- the span rule ----------------------------------------------------------------
//
// Frame k at a rate of num/den covers samples [start(k), start(k+1)) at
// `rate`, with start(k) = round(k * rate * den / num). The host uses the
// same rule, so an effect that asks for a clip's frame k asks for exactly
// the samples the host would put under its picture. At 30000/1001 and
// 48 kHz the lengths are 1601 and 1602, and every pair abuts.

/// The first sample of frame `k`. Sixty-four bits: good for a hundred days
/// of 96 kHz at 24000/1001.
[[nodiscard]] inline int64_t sampleStart(int64_t k, int rate, int64_t fpsNum,
                                         int64_t fpsDen) noexcept {
    if (fpsNum <= 0 || fpsDen <= 0 || rate <= 0) {
        return 0;
    }
    const int64_t numerator = k * rate * fpsDen * 2 + fpsNum;
    const int64_t divisor = fpsNum * 2;
    int64_t quotient = numerator / divisor;
    if (numerator % divisor != 0 && numerator < 0) {
        quotient -= 1;
    }
    return quotient;
}

/// How many samples frame `k` holds.
[[nodiscard]] inline int64_t sliceLength(int64_t k, int rate, int64_t fpsNum,
                                         int64_t fpsDen) noexcept {
    return sampleStart(k + 1, rate, fpsNum, fpsDen) - sampleStart(k, rate, fpsNum, fpsDen);
}

}   // namespace aofx
