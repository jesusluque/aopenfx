// Copyright (c) 2026 aopenfx contributors.
//
// What a matching pipeline puts in a plane, and where.
//
// A tracker split into nodes -- a detector, a matcher, a fitter -- has to
// hand keypoints and correspondences from one node to the next, and the SDK
// offers exactly two ways to do that (`Effect.h`): an attachment, which is
// small by contract, and a plane, which is a frame-sized float4 buffer. A
// thousand descriptors of sixty-four floats is not small, so it goes in a
// plane, and then somebody has to say *where in the plane* -- which is this
// file.
//
// WHY A HEADER PIXEL AND A VERSION
//
// The producer and the consumer are different plugins, built at different
// times, and on a render host they may be different builds again. A layout
// agreed by comment is a layout that drifts, and a reader that trusts a
// buffer's shape reads garbage as data: a thousand keypoints at the wrong
// stride is not an error, it is a track that wanders. So every block starts
// with its magic and its version, and a reader that does not know the
// version **reads nothing** rather than the wrong offsets. `Tracks.h` learnt
// this for the document; a plane needs it for the same reason.
//
// WHY POSITIONS AND NOT INDICES IN `matches`
//
// A sparse matcher (XFeat and LightGlue) pairs indices into two keypoint
// sets. A dense one (LoFTR) has no indices to give -- it answers with
// positions. Carrying indices would need a second block shape for the dense
// path and a second fitter to read it, so `matches` carries the two
// positions and one fitter serves both. The cost is four floats a match
// instead of two; the saving is a node.
//
// WHY OFFSETS IN PIXELS, AND WHICH PIXELS
//
// A plane is addressed as float4 texels on the device (`aofx::Buffer`), so
// everything here counts in texels and a descriptor is a whole number of
// them. Sixty-four floats is sixteen; the offset of keypoint N is
// arithmetic, in a kernel, with no search and no indirection.
//
// **Linear from the start of the buffer, not (x, y).** A buffer's `stride`
// can exceed its width, so texel `i` here is *not* the pixel at
// `(i % width, i / width)` -- it is simply the i'th float4 of the
// allocation. Nothing addresses these blocks as a picture, so the padding
// never matters, and both sides counting the same way is all that is
// required. GeometricTrack's mesh plane already works exactly like this, and
// a reader that assumed a row length instead would find its data shifted by
// the padding on the one machine whose allocator pads.
//
// THESE NUMBERS LIVE TWICE
//
// Here, and in the `.slang` that reads them. Nothing checks that the two
// agree -- the same hazard as a kernel's uniform block -- so a change here
// is a change in both, and the Slang side says so at the top of its file.
#pragma once

#include <cstdint>

namespace aofx {

// The magics are small numbers and not four packed characters, which is what
// a file format would use and what this cannot.
//
// A plane holds float32. A float32 carries integers exactly only up to
// 2^24 -- sixteen million and change -- and any four-character magic is far
// above that: 'O','F','E','A' is 1 095 650 630, which lands on the nearest
// representable float and comes back out as a different number. The check
// would then pass or fail on the rounding, which is the worst kind of check:
// one that works on the machine it was written on. Small distinct integers
// are exact everywhere, and exactness is the whole job of a magic.
inline constexpr float kFeaturesMagic = 7801.0F;
inline constexpr float kMatchesMagic = 7802.0F;
inline constexpr float kKeyframesMagic = 7803.0F;

/// Bumped when a layout below changes. One number for all three: they are
/// written by one pipeline and read by one pipeline, and a version each
/// would be three things to forget instead of one.
inline constexpr float kFeatureVersion = 1.0F;

// --- `features`: what a detector produces ----------------------------------
//
//   pixel 0          magic, version, count, descriptorDim
//   pixel 1          width, height, 0, 0        -- the space the keypoints are in
//   pixel 2 + i      x, y, score, 0             -- keypoint i, rows from the top
//   pixel D + i*S    the descriptor of keypoint i, S pixels of it
//
// `width` and `height` are the picture the keypoints were found in, which is
// not always the picture the node was handed: a detector runs at its own
// size and a consumer scales by its own width over this one. The same reason
// `Tracks` records the frame it was traced in.

inline constexpr uint32_t kFeaturesHeaderPixels = 2;

/// Where keypoint `index` sits, in pixels from the start of the plane.
[[nodiscard]] inline constexpr uint32_t featuresKeypointAt(uint32_t index) {
    return kFeaturesHeaderPixels + index;
}

/// How many pixels one descriptor of `dim` floats occupies.
[[nodiscard]] inline constexpr uint32_t featuresDescriptorPixels(uint32_t dim) {
    return (dim + 3U) / 4U;
}

/// Where the descriptor of keypoint `index` starts.
[[nodiscard]] inline constexpr uint32_t featuresDescriptorAt(uint32_t index,
                                                             uint32_t count,
                                                             uint32_t dim) {
    return kFeaturesHeaderPixels + count + index * featuresDescriptorPixels(dim);
}

/// The pixels a whole block needs, so a producer can refuse before it writes
/// past the end of a plane rather than into whatever follows it.
[[nodiscard]] inline constexpr uint32_t featuresPixels(uint32_t count, uint32_t dim) {
    return kFeaturesHeaderPixels + count + count * featuresDescriptorPixels(dim);
}

// --- `matches`: what a matcher produces ------------------------------------
//
//   pixel 0          magic, version, count, 0
//   pixel 1          width0, height0, width1, height1
//   pixel 2 + i      x0, y0, x1, y1             -- match i, both rows from the top
//
// The two sizes are the two spaces the two halves are in, and they differ
// whenever the two sides were looked at differently -- a frame at the fast
// size against a mosaic at the canvas size, which is the ordinary case here.

inline constexpr uint32_t kMatchesHeaderPixels = 2;

[[nodiscard]] inline constexpr uint32_t matchAt(uint32_t index) {
    return kMatchesHeaderPixels + index;
}

[[nodiscard]] inline constexpr uint32_t matchesPixels(uint32_t count) {
    return kMatchesHeaderPixels + count;
}

// --- `keyframes`: the table a relocaliser walks ----------------------------
//
//   pixel 0          magic, version, count, descriptorDim
//   pixel 1          width, height, 0, 0
//   pixel H + i*K    frame, valid, 0, 0
//     + 1 .. 3       H_ref_from_kf, row major, nine floats in three pixels
//     + 4 ...        the global descriptor, `descriptorDim/4` pixels
//
// A keyframe is a homography and a smell: where the plane was when this
// frame was seen, and a vector that says whether a later frame looks like
// this one at all. Both belong to the same row because a relocaliser needs
// the second to choose and the first to answer.

inline constexpr uint32_t kKeyframesHeaderPixels = 2;
/// The fixed part of a row: the frame, then the homography.
inline constexpr uint32_t kKeyframeFixedPixels = 4;

[[nodiscard]] inline constexpr uint32_t keyframeStride(uint32_t descriptorDim) {
    return kKeyframeFixedPixels + featuresDescriptorPixels(descriptorDim);
}

[[nodiscard]] inline constexpr uint32_t keyframeAt(uint32_t index, uint32_t descriptorDim) {
    return kKeyframesHeaderPixels + index * keyframeStride(descriptorDim);
}

[[nodiscard]] inline constexpr uint32_t keyframesPixels(uint32_t count, uint32_t descriptorDim) {
    return kKeyframesHeaderPixels + count * keyframeStride(descriptorDim);
}

// --- the mosaic --------------------------------------------------------------
//
// The mosaic has **no header pixel**, on purpose: it is a picture, and a
// header written into its first texel is a corrupt pixel of it that some
// viewer will one day show. What a reader needs to know about it -- the map
// from the reference plane into the canvas, and how big the canvas is -- is
// eleven floats, which is what an attachment is for. It travels beside the
// plane under this name.
inline constexpr const char* kMosaicSpace = "mosaic_space";
/// Nine floats of `canvas_from_reference`, row major, then the canvas' width
/// and height. Eleven in all.
inline constexpr uint32_t kMosaicSpaceFloats = 11;

// --- the attachments the pipeline agrees on ----------------------------------
//
// Small enough for the channel the SDK reserves for them, and named here so
// that eight nodes spell them the same way.
inline constexpr const char* kHomography = "homography";   ///< 9, row major, h22 = 1, document pixels
inline constexpr const char* kInliers = "inliers";         ///< 1
inline constexpr const char* kResidual = "residual";       ///< 1
inline constexpr const char* kPath = "path";               ///< 1: 0 lost, 1 fast, 2 slow, 3 relocalised

}   // namespace aofx
