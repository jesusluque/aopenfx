// Copyright (c) 2026 aopenfx contributors.
//
// Translate, rotate, scale and skew, with a resampling filter.
//
// Every pixel of the output is mapped *backwards* into the source and the
// source is sampled there. That direction is not a preference: mapping forwards
// leaves holes wherever the transform stretches, and writing to a destination
// pixel twice is a race. Backwards, every output pixel is written exactly once
// by exactly one thread, and the filter decides what it reads.
//
// The matrix arrives already inverted. Inverting a 3x3 once on the CPU beats
// inverting it in every one of two million threads, and it is the same answer.
//
// Coordinates: the output pixel at index (x, y) has its centre at
// (dstX1 + x + 0.5, dstY1 + y + 0.5) in the shared pixel space that both
// rectangles are expressed in. Going the other way, a source position `s` lands
// at index-space u = s - srcX1 - 0.5, where an integer u means "exactly on a
// pixel centre". Getting that half-pixel wrong is invisible on a translation by
// a whole number of pixels and shows up as a soft image everywhere else, which
// is the kind of bug that survives a long time.

struct TransformParams {
    uint  width;
    uint  height;
    uint  srcStride;
    uint  dstStride;
    int   srcX1;
    int   srcY1;
    uint  srcWidth;
    uint  srcHeight;
    int   dstX1;
    int   dstY1;
    /// The inverse, row major and affine:
    ///     sx = m0*x + m1*y + m2
    ///     sy = m3*x + m4*y + m5
    float m0;
    float m1;
    float m2;
    float m3;
    float m4;
    float m5;
    uint  filter;
    /// Outside the source is transparent black, or the edge pixel repeated.
    uint  blackOutside;
    uint  doClamp;
    uint  pad0;
};

StructuredBuffer<float4>        src;
RWStructuredBuffer<float4>      dst;
ConstantBuffer<TransformParams> params;

static const uint kImpulse = 0u;   static const uint kBox = 1u;
static const uint kBilinear = 2u;  static const uint kCubic = 3u;
static const uint kKeys = 4u;      static const uint kSimon = 5u;
static const uint kRifman = 6u;    static const uint kMitchell = 7u;
static const uint kParzen = 8u;

/// One texel, in index space. Outside the source is either nothing or the edge
/// repeated -- the plugin's `black_outside`, which is on by default.
float4 fetch(int x, int y) {
    if (params.blackOutside != 0u) {
        if (x < 0 || y < 0 || x >= int(params.srcWidth) ||
            y >= int(params.srcHeight)) {
            return float4(0.0, 0.0, 0.0, 0.0);
        }
    } else {
        x = clamp(x, 0, int(params.srcWidth) - 1);
        y = clamp(y, 0, int(params.srcHeight) - 1);
        if (params.srcWidth == 0u || params.srcHeight == 0u) {
            return float4(0.0, 0.0, 0.0, 0.0);
        }
    }
    return src[uint(y) * params.srcStride + uint(x)];
}

/// The Mitchell-Netravali family, which is every cubic filter here at once.
///
/// One kernel with two constants covers Cubic, Keys, Simon, Rifman, Mitchell
/// and Parzen -- six entries in the menu that are one function evaluated at six
/// pairs of numbers. Writing them out separately would be six chances to get a
/// coefficient wrong and no way to tell which one.
float cubicWeight(float t, float B, float C) {
    t = abs(t);
    const float t2 = t * t;
    const float t3 = t2 * t;
    if (t < 1.0) {
        return ((12.0 - 9.0 * B - 6.0 * C) * t3 +
                (-18.0 + 12.0 * B + 6.0 * C) * t2 + (6.0 - 2.0 * B)) / 6.0;
    }
    if (t < 2.0) {
        return ((-B - 6.0 * C) * t3 + (6.0 * B + 30.0 * C) * t2 +
                (-12.0 * B - 48.0 * C) * t + (8.0 * B + 24.0 * C)) / 6.0;
    }
    return 0.0;
}

void familyOf(uint filter, out float B, out float C) {
    // B=0 with a rising C is the classic one-parameter family: C is the
    // sharpening. Mitchell and Parzen leave it.
    B = 0.0;
    C = 0.0;
    if (filter == kKeys)     { C = 0.5; }
    if (filter == kSimon)    { C = 0.75; }
    if (filter == kRifman)   { C = 1.0; }
    if (filter == kMitchell) { B = 1.0 / 3.0; C = 1.0 / 3.0; }
    if (filter == kParzen)   { B = 1.0; C = 0.0; }
}

float4 sampleSource(float u, float v) {
    if (params.filter == kImpulse) {
        return fetch(int(floor(u + 0.5)), int(floor(v + 0.5)));
    }

    if (params.filter == kBilinear) {
        const float fu = floor(u);
        const float fv = floor(v);
        const float du = u - fu;
        const float dv = v - fv;
        const int   iu = int(fu);
        const int   iv = int(fv);
        const float4 top = lerp(fetch(iu, iv), fetch(iu + 1, iv), du);
        const float4 bot = lerp(fetch(iu, iv + 1), fetch(iu + 1, iv + 1), du);
        return lerp(top, bot, dv);
    }

    if (params.filter == kBox) {
        // The footprint of one output pixel, back in the source. Its width is
        // how far the inverse map carries a step of one along each axis, so it
        // comes straight out of the matrix rather than being guessed.
        const float wu = max(1.0, abs(params.m0) + abs(params.m1));
        const float wv = max(1.0, abs(params.m3) + abs(params.m4));
        const int   ru = int(ceil(wu * 0.5));
        const int   rv = int(ceil(wv * 0.5));
        const int   iu = int(floor(u + 0.5));
        const int   iv = int(floor(v + 0.5));
        float4 total = float4(0.0, 0.0, 0.0, 0.0);
        float  weight = 0.0;
        // Bounded. A transform can scale by ten thousand, and an unbounded
        // integration there would hang the GPU rather than be slow on it.
        for (int dy = -rv; dy <= rv && dy <= 32; ++dy) {
            for (int dx = -ru; dx <= ru && dx <= 32; ++dx) {
                total += fetch(iu + dx, iv + dy);
                weight += 1.0;
            }
        }
        return weight > 0.0 ? total / weight : float4(0.0, 0.0, 0.0, 0.0);
    }

    float B = 0.0;
    float C = 0.0;
    familyOf(params.filter, B, C);
    const float fu = floor(u);
    const float fv = floor(v);
    const float du = u - fu;
    const float dv = v - fv;
    const int   iu = int(fu);
    const int   iv = int(fv);

    float4 total = float4(0.0, 0.0, 0.0, 0.0);
    float  weight = 0.0;
    for (int j = -1; j <= 2; ++j) {
        const float wj = cubicWeight(float(j) - dv, B, C);
        for (int i = -1; i <= 2; ++i) {
            const float w = wj * cubicWeight(float(i) - du, B, C);
            total += fetch(iu + i, iv + j) * w;
            weight += w;
        }
    }
    // Normalised by the weights actually used. For these kernels the sum is one
    // analytically, and in floating point it is one to within a rounding error
    // -- dividing costs a divide and removes a slow drift across a large image.
    return weight != 0.0 ? total / weight : float4(0.0, 0.0, 0.0, 0.0);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void transformMain(uint3 tid: SV_DispatchThreadID) {
    const uint2 xy = tid.xy;
    if (xy.x >= params.width || xy.y >= params.height) {
        return;
    }

    const float ox = float(params.dstX1) + float(xy.x) + 0.5;
    const float oy = float(params.dstY1) + float(xy.y) + 0.5;

    const float sx = params.m0 * ox + params.m1 * oy + params.m2;
    const float sy = params.m3 * ox + params.m4 * oy + params.m5;

    const float u = sx - float(params.srcX1) - 0.5;
    const float v = sy - float(params.srcY1) - 0.5;

    float4 out = sampleSource(u, v);
    if (params.doClamp != 0u) {
        // The sharpening filters overshoot on purpose -- the plugin marks them
        // with a (*) -- so this is the switch that says whether the overshoot
        // is wanted.
        out = clamp(out, float4(0.0, 0.0, 0.0, 0.0), float4(1.0, 1.0, 1.0, 1.0));
    }
    dst[xy.y * params.dstStride + xy.x] = out;
}
