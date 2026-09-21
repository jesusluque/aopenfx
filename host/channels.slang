// Copyright (c) 2026 aopenfx contributors.
//
// Puts back the channels an effect was not asked to touch.
//
// Every AOFX effect fills all four channels -- there is one internal format and
// a kernel that had to branch on which channels it was allowed to write would
// be a kernel every author gets wrong in a different way. So the effect renders
// everything and this restores the rest from the input, which is what a node
// applied to `rgb` means: the alpha that comes out is the alpha that went in.
//
// The host's own kernel, compiled by the SDK's own build rule. That is
// deliberate: if the way a plugin builds a kernel is not good enough for the
// host to use, it is not good enough.
//
// The two pictures need not be the same size -- an effect's region of
// definition is often larger than its input -- so they are related by their
// rectangles, exactly as an effect relates its own. Where the input does not
// reach there is nothing to put back, and the effect's own value stands: it is
// picture the effect created, and taking it away would leave a hole in the
// channels nobody asked to change.

struct ChannelParams {
    /// The destination, which is the effect's output.
    uint  width;
    uint  height;
    uint  dstStride;
    /// The input, which may be smaller and elsewhere.
    uint  srcWidth;
    uint  srcHeight;
    uint  srcStride;
    int   srcOffsetX;
    int   srcOffsetY;
    /// One bit a channel, red in bit 0. A set bit is a channel the effect was
    /// asked to change, so a set bit is one this leaves alone.
    uint  keepMask;
    uint  pad0;
};

StructuredBuffer<float4>      src;
RWStructuredBuffer<float4>    dst;
ConstantBuffer<ChannelParams> params;

[shader("compute")]
[numthreads(16, 16, 1)]
void channelsMain(uint3 tid: SV_DispatchThreadID) {
    const uint2 xy = tid.xy;
    if (xy.x >= params.width || xy.y >= params.height) {
        return;
    }

    const int sx = int(xy.x) + params.srcOffsetX;
    const int sy = int(xy.y) + params.srcOffsetY;
    // Outside the input entirely: nothing to put back. What the effect made
    // there is picture it created, and blanking it would punch a hole in the
    // channels nobody asked to change.
    if (sx < 0 || sy < 0 || sx >= int(params.srcWidth) ||
        sy >= int(params.srcHeight)) {
        return;
    }

    const uint at = xy.y * params.dstStride + xy.x;
    const float4 was = src[uint(sy) * params.srcStride + uint(sx)];
    const float4 now = dst[at];

    dst[at] = float4((params.keepMask & 1u) != 0u ? now.x : was.x,
                     (params.keepMask & 2u) != 0u ? now.y : was.y,
                     (params.keepMask & 4u) != 0u ? now.z : was.z,
                     (params.keepMask & 8u) != 0u ? now.w : was.w);
}
