// Copyright (c) 2026 aopenfx contributors.
//
// The grade every compositor has, per channel.
//
// Two points in and two points out, then a multiply, an offset and a gamma:
//
//     A = multiply * (white - black) / (whitePoint - blackPoint)
//     B = offset + black - A * blackPoint
//     out = A * in + B
//     out = pow(out, 1/gamma)      where out is positive
//
// Collapsing the first three into one multiply-add is not an optimisation, it
// is the definition -- a straight line through two named points -- and writing
// it as four separate steps is how the reverse form ends up not being the exact
// inverse of the forward one.
//
// Negative values skip the gamma. There is no real answer for a negative number
// raised to a fractional power, and scene-linear pictures carry negatives
// routinely: a legal value out of a matrix, a ringing filter, the underside of
// a highlight roll-off. Clamping them here would quietly repair pixels that a
// downstream node might have made sense of.

struct GradeParams {
    uint  width;
    uint  height;
    uint  srcStride;
    uint  dstStride;
    // One per channel, alpha included: a grade that could not touch alpha
    // would be a grade that cannot pull a matte back.
    float4 blackPoint;
    float4 whitePoint;
    float4 black;
    float4 white;
    float4 multiply;
    float4 offset;
    float4 gamma;
    uint  clampBlack;
    uint  clampWhite;
    uint  reverse;
    /// Whether anybody asked for the clipping matte.
    ///
    /// When nothing did, the host binds the picture's own buffer in the
    /// matte's place rather than allocating one. That is safe only because
    /// this flag stops the write: an unwritten alias is just an extra name for
    /// a buffer, and a written one would be the picture destroyed by its own
    /// side effect.
    uint  wantClip;
    /// Where the input sits relative to the output, and how big it is.
    /// See fetchSource: a kernel must not assume the two line up.
    int   srcOffsetX;
    int   srcOffsetY;
    uint  srcWidth;
    uint  srcHeight;
};

StructuredBuffer<float4>    src;
RWStructuredBuffer<float4>  dst;
/// The clipping matte: where this grade pushed a value out of [0, 1].
///
/// An arbitrary output variable, in the sense a renderer means it. It costs one
/// comparison per channel on values the kernel already has in registers, which
/// is the whole argument for producing it here rather than by running a second
/// node over the result -- by then the information is gone, because the clamp
/// has already happened or the value has already been carried away.
RWStructuredBuffer<float4>  clipMatte;
ConstantBuffer<GradeParams> params;


/// One source texel, related to the output rather than assumed to be under it.
///
/// The plain `src[y * stride + x]` this replaces trusted the host completely:
/// it assumed the input covers the output and shares its origin. That held
/// until a crop handed an effect an output larger than its input, and a kernel
/// walking its output read past the end of the buffer -- which reached the
/// delivered frame as infinities rather than as a crash or a black frame.
///
/// Outside the input is transparent black. The engine is fixed and should never
/// send this, but a kernel that reads wild memory when it does is a kernel that
/// turns a host defect into garbage on screen instead of an obvious hole.
float4 fetchSource(uint2 xy) {
    const int sx = int(xy.x) + params.srcOffsetX;
    const int sy = int(xy.y) + params.srcOffsetY;
    if (sx < 0 || sy < 0 || sx >= int(params.srcWidth) ||
        sy >= int(params.srcHeight)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return src[uint(sy) * params.srcStride + uint(sx)];
}

float4 safePow(float4 v, float4 exponent) {
    return float4(v.x > 0.0 ? pow(v.x, exponent.x) : v.x,
                  v.y > 0.0 ? pow(v.y, exponent.y) : v.y,
                  v.z > 0.0 ? pow(v.z, exponent.z) : v.z,
                  v.w > 0.0 ? pow(v.w, exponent.w) : v.w);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void gradeMain(uint3 tid: SV_DispatchThreadID) {
    const uint2 xy = tid.xy;
    if (xy.x >= params.width || xy.y >= params.height) {
        return;
    }

    // The denominator is whatever the user typed, and they can type the same
    // number twice. A white point equal to the black point is a line of
    // infinite slope, so it becomes a line of slope one instead of a frame full
    // of infinities.
    const float4 span = params.whitePoint - params.blackPoint;
    const float4 safeSpan = float4(abs(span.x) < 1e-8 ? 1.0 : span.x,
                                   abs(span.y) < 1e-8 ? 1.0 : span.y,
                                   abs(span.z) < 1e-8 ? 1.0 : span.z,
                                   abs(span.w) < 1e-8 ? 1.0 : span.w);
    const float4 a = params.multiply * (params.white - params.black) / safeSpan;
    const float4 b = params.offset + params.black - a * params.blackPoint;

    float4 v = fetchSource(xy);

    if (params.reverse != 0u) {
        // The exact inverse, in the opposite order: gamma first, then the line.
        // Doing it as a second forward grade with reciprocal numbers is close
        // and not the same, and "close" in an inverse means a round trip that
        // does not come back.
        v = safePow(v, params.gamma);
        const float4 safeA = float4(abs(a.x) < 1e-8 ? 1.0 : a.x,
                                    abs(a.y) < 1e-8 ? 1.0 : a.y,
                                    abs(a.z) < 1e-8 ? 1.0 : a.z,
                                    abs(a.w) < 1e-8 ? 1.0 : a.w);
        v = (v - b) / safeA;
    } else {
        v = a * v + b;
        v = safePow(v, 1.0 / params.gamma);
    }

    // Measured before the clamps, on purpose. After them every value is inside
    // the range by construction and the matte would be empty in exactly the
    // case somebody most wants it -- a graded shot with Clamp White on, where
    // the question is which highlights got flattened.
    // Strictly outside, not on the edge. `step(v, 0)` lights when v <= 0, and
    // a value of exactly zero has not been clipped by anything -- it is the
    // bottom of the range, not past it. The first column of a ramp from zero is
    // exactly that value, so the test caught it immediately: 32 pixels wrong,
    // which was one column of a 32-row plate.
    const float4 below = float4(1.0, 1.0, 1.0, 1.0) -
                         step(float4(0.0, 0.0, 0.0, 0.0), v);
    const float4 above = float4(1.0, 1.0, 1.0, 1.0) -
                         step(v, float4(1.0, 1.0, 1.0, 1.0));
    const float4 outside = min(float4(1.0, 1.0, 1.0, 1.0), below + above);

    if (params.clampBlack != 0u) {
        v = max(v, float4(0.0, 0.0, 0.0, 0.0));
    }
    if (params.clampWhite != 0u) {
        v = min(v, float4(1.0, 1.0, 1.0, 1.0));
    }
    dst[xy.y * params.dstStride + xy.x] = v;

    // Per channel in RGB, and alpha carries "any of them" so that the matte is
    // usable as a matte without a channel shuffle in front of it.
    if (params.wantClip != 0u) {
        clipMatte[xy.y * params.dstStride + xy.x] =
            float4(outside.x, outside.y, outside.z,
                   max(outside.x, max(outside.y, outside.z)));
    }
}
