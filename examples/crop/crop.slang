// Copyright (c) 2026 aopenfx contributors.
//
// A rectangle, and what is outside it.
//
// ONE KERNEL, BECAUSE CUT AND BLACK ARE THE SAME ARITHMETIC
//
// Whether the picture is really cut down to the box or merely blacked outside
// it is decided before this runs, by `regionOfDefinition`: with `cut` on, the
// output rectangle *is* the box, and this kernel is walking a smaller frame.
// Either way the work per pixel is the same -- read the source, ask how far
// inside the box this pixel is, multiply -- so there is one kernel and not two,
// and the tick cannot make them disagree.
//
// SOFTNESS IS A DISTANCE, NOT A BLUR
//
// A soft crop edge is asked for far more often than it is thought about: what
// people want is the picture fading out over a few pixels, not the picture
// blurred. So this is a ramp on the distance to the box's edge, which costs one
// subtraction per side and no neighbourhood at all.

struct CropParams {
    uint  srcWidth;
    uint  srcHeight;
    uint  srcStride;
    int   srcOffsetX;
    int   srcOffsetY;
    uint  dstWidth;
    uint  dstHeight;
    uint  dstStride;
    /// Where the output's own rectangle sits, so the box -- which is in the
    /// picture's coordinates and does not move when the output is cut down --
    /// can be compared against it.
    int   dstOriginX;
    int   dstOriginY;
    /// The box, in the picture's coordinates, already ordered.
    float x1;
    float y1;
    float x2;
    float y2;
    float softness;
    /// 1 to keep what is outside instead of blacking it. With `cut` on there is
    /// nothing outside to keep, which is why the two ticks are not the same.
    uint  keepOutside;
};

StructuredBuffer<float4>   src;
RWStructuredBuffer<float4> dst;
ConstantBuffer<CropParams> params;

float4 fetchSource(int x, int y) {
    // Offset first, then bounds-checked: the source can be larger or smaller
    // than the output, and a clamp before the offset reads past either end.
    // Outside the source is transparent, not the edge pixel repeated -- a crop
    // handed less of its input than it writes has nothing to show there, and
    // smearing the last row across the gap would be inventing a picture.
    const int cx = x + params.srcOffsetX;
    const int cy = y + params.srcOffsetY;
    if (cx < 0 || cy < 0 || cx >= int(params.srcWidth) ||
        cy >= int(params.srcHeight)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return src[uint(cy) * params.srcStride + uint(cx)];
}

/// How much of this pixel is inside the box: one well within, zero well
/// outside, and a ramp `softness` wide across the edge.
float insideness(float x, float y) {
    if (params.softness <= 0.0) {
        return (x >= params.x1 && x <= params.x2 && y >= params.y1 &&
                y <= params.y2)
                   ? 1.0
                   : 0.0;
    }
    // The distance to each side, ramped. Multiplied rather than taken as a
    // minimum, so a corner is rounded the way a soft edge on two sides at once
    // actually looks -- a minimum leaves a square corner on a soft edge, which
    // reads as a mistake.
    const float s = params.softness;
    const float left = saturate((x - params.x1) / s);
    const float right = saturate((params.x2 - x) / s);
    const float bottom = saturate((y - params.y1) / s);
    const float top = saturate((params.y2 - y) / s);
    return left * right * bottom * top;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void cropMain(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= params.dstWidth || tid.y >= params.dstHeight) {
        return;
    }
    const float4 rgba = fetchSource(int(tid.x), int(tid.y));

    // In the picture's coordinates, which is where the box is. With `cut` on
    // the output starts somewhere else, and forgetting this is a crop that
    // works until somebody cuts and then slides.
    const float x = float(int(tid.x) + params.dstOriginX) + 0.5;
    const float y = float(int(tid.y) + params.dstOriginY) + 0.5;

    const float inside = params.keepOutside != 0u ? 1.0 : insideness(x, y);
    dst[tid.y * params.dstStride + tid.x] = rgba * inside;
}
