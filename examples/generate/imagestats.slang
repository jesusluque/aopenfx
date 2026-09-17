// Copyright (c) 2026 aopenfx contributors.
//
// Min, max, mean and standard deviation of a picture, per channel.
//
// HOW THE REDUCTION IS SHAPED
//
// One thread per row of the region, each folding its own row into its own slot,
// and then four threads folding the slots. No groupshared memory and no
// `SV_GroupID` anywhere -- deliberately: the group size is the backend's choice
// (16x16 on Metal, a flat 256 on CUDA), so a kernel that declares its own and
// indexes groupshared by the thread's place in the group silently loses
// fifteen sixteenths of its lanes and still returns a plausible number. That
// bug cost a measured six per cent of a box once. It is not being written a
// second time.
//
// A row per thread is a real parallel reduction on anything with more than a
// few hundred rows, which is every picture this will ever see, and its cost is
// one pass over the pixels either way.

struct StatsParams {
    uint srcWidth;
    uint srcHeight;
    uint srcStride;
    uint rows;        ///< how many rows of the region there are

    uint x1;          ///< the region, already in the source buffer's own
    uint y1;          ///< coordinates -- the host does that arithmetic
    uint x2;
    uint y2;

    uint dstWidth;
    uint dstHeight;
    uint dstStride;
    int  srcOffsetX;

    int  srcOffsetY;
    uint pad0;
    uint pad1;
    uint pad2;
};

StructuredBuffer<float4>       src;
RWStructuredBuffer<float4>     sums;   ///< 0..3 the answer; 4+r*4 the rows
RWStructuredBuffer<float4>     dst;
ConstantBuffer<StatsParams>    params;

static const float kBig = 3.402823e+38;

[shader("compute")]
[numthreads(16, 16, 1)]
void statsRow(uint3 tid: SV_DispatchThreadID) {
    const uint row = tid.x;
    if (row >= params.rows) {
        return;
    }
    float4 lo = float4(kBig, kBig, kBig, kBig);
    float4 hi = float4(-kBig, -kBig, -kBig, -kBig);
    float4 total = float4(0, 0, 0, 0);
    float4 squares = float4(0, 0, 0, 0);

    const uint y = params.y1 + row;
    for (uint x = params.x1; x < params.x2; ++x) {
        const float4 v = src[y * params.srcStride + x];
        lo = min(lo, v);
        hi = max(hi, v);
        total += v;
        squares += v * v;
    }
    const uint at = 4u + row * 4u;
    sums[at + 0u] = lo;
    sums[at + 1u] = hi;
    sums[at + 2u] = total;
    sums[at + 3u] = squares;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void statsFold(uint3 tid: SV_DispatchThreadID) {
    const uint which = tid.x;
    if (which >= 4u) {
        return;
    }
    // Each of the four statistics folds the same way it accumulated, which is
    // why they can share one loop: two of them by min/max and two by adding.
    float4 out = which == 0u   ? float4(kBig, kBig, kBig, kBig)
                 : which == 1u ? float4(-kBig, -kBig, -kBig, -kBig)
                               : float4(0, 0, 0, 0);
    for (uint row = 0u; row < params.rows; ++row) {
        const float4 one = sums[4u + row * 4u + which];
        if (which == 0u) {
            out = min(out, one);
        } else if (which == 1u) {
            out = max(out, one);
        } else {
            out += one;
        }
    }
    sums[which] = out;
}

/// The picture through, untouched. A probe you can leave in a graph.
[shader("compute")]
[numthreads(16, 16, 1)]
void statsCopy(uint3 tid: SV_DispatchThreadID) {
    const uint2 xy = tid.xy;
    if (xy.x >= params.dstWidth || xy.y >= params.dstHeight) {
        return;
    }
    const int sx = int(xy.x) + params.srcOffsetX;
    const int sy = int(xy.y) + params.srcOffsetY;
    float4 v = float4(0, 0, 0, 0);
    if (sx >= 0 && sy >= 0 && uint(sx) < params.srcWidth &&
        uint(sy) < params.srcHeight) {
        v = src[uint(sy) * params.srcStride + uint(sx)];
    }
    dst[xy.y * params.dstStride + xy.x] = v;
}
