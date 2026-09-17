// Copyright (c) 2026 aopenfx contributors.
//
// The compositing operations, in one kernel.
//
// Thirty-nine of them, at the same indices the Merge plugin uses, so a script
// can name either node and mean the same operation. The indices are therefore
// not negotiable: reordering this list changes what every saved script does.
//
// A and B, not "foreground" and "background": A is what arrives, B is what it
// arrives onto, and `a` and `b` are their alphas. Every formula below is
// written in those terms because that is how they are written everywhere else,
// and renaming them here would be a translation nobody asked for on every line.
//
// The four HSL operations -- hue, saturation, colour, luminosity -- are not
// per-component. They take one part of A's colour and the rest of B's, which
// needs the whole pixel at once, so they are implemented separately below.

struct MergeParams {
    uint width;
    uint height;
    uint aStride;
    uint bStride;
    uint dstStride;
    /// A and B each sit somewhere, and so does the output: a merge is the one
    /// operation whose two inputs routinely have different regions. Offsets
    /// from the output's own origin, in pixels.
    int  aOffsetX;
    int  aOffsetY;
    int  bOffsetX;
    int  bOffsetY;
    uint aWidth;
    uint aHeight;
    uint bWidth;
    uint bHeight;
    uint operation;
    /// Output alpha as a + b - a*b rather than whatever the operation gives.
    /// Always on for the HSL modes, which have no opinion about alpha.
    uint screenAlpha;
    uint pad0;
};

StructuredBuffer<float4>    srcA;
StructuredBuffer<float4>    srcB;
RWStructuredBuffer<float4>  dst;
ConstantBuffer<MergeParams> params;

// Indices, kept in step with Merge.cpp and with the plugin.
static const uint kAtop = 0u;        static const uint kAverage = 1u;
static const uint kColor = 2u;       static const uint kColorBurn = 3u;
static const uint kColorDodge = 4u;  static const uint kConjointOver = 5u;
static const uint kCopy = 6u;        static const uint kDifference = 7u;
static const uint kDisjointOver = 8u; static const uint kDivide = 9u;
static const uint kExclusion = 10u;  static const uint kFreeze = 11u;
static const uint kFrom = 12u;       static const uint kGeometric = 13u;
static const uint kGrainExtract = 14u; static const uint kGrainMerge = 15u;
static const uint kHardLight = 16u;  static const uint kHue = 17u;
static const uint kHypot = 18u;      static const uint kIn = 19u;
static const uint kLuminosity = 20u; static const uint kMask = 21u;
static const uint kMatte = 22u;      static const uint kMax = 23u;
static const uint kMin = 24u;        static const uint kMinus = 25u;
static const uint kMultiply = 26u;   static const uint kOut = 27u;
static const uint kOver = 28u;       static const uint kOverlay = 29u;
static const uint kPinLight = 30u;   static const uint kPlus = 31u;
static const uint kReflect = 32u;    static const uint kSaturation = 33u;
static const uint kScreen = 34u;     static const uint kSoftLight = 35u;
static const uint kStencil = 36u;    static const uint kUnder = 37u;
static const uint kXor = 38u;

/// One component, given the two values and the two alphas.
float mergeOne(uint op, float A, float B, float a, float b) {
    if (op == kAtop)        { return A * b + B * (1.0 - a); }
    if (op == kAverage)     { return (A + B) * 0.5; }
    if (op == kColorBurn)   { return A <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - B) / A); }
    if (op == kColorDodge)  { return A >= 1.0 ? 1.0 : min(1.0, B / (1.0 - A)); }
    if (op == kConjointOver) {
        // Over for sources that overlap completely: where A covers less than
        // B, B is scaled by the ratio rather than by (1 - a).
        return a >= b ? A : A + B * (1.0 - a / max(b, 1e-8));
    }
    if (op == kCopy)        { return A; }
    if (op == kDifference)  { return abs(A - B); }
    if (op == kDisjointOver) {
        // Over for sources that do not overlap at all: while the two alphas
        // together fit inside one pixel, neither occludes the other.
        return a + b < 1.0 ? A + B : A + B * (1.0 - a) / max(b, 1e-8);
    }
    if (op == kDivide)      { return B <= 0.0 ? 0.0 : A / B; }
    if (op == kExclusion)   { return A + B - 2.0 * A * B; }
    // freeze and reflect take A and B the other way round from every other
    // operation here, and both clamp. Not deduced -- measured. Two ramps at
    // alpha 1, A along x and B along y, rendered through the plugin: the
    // surface that came back was the exact transpose of mine, cell for cell,
    // with a floor at 0 and a ceiling at 1. A surface probe in the host's
    // equivalence tests (not part of this repository) settled it after
    // two wrong guesses -- including one where I removed these clamps on the
    // theory that they were mine, and moved the error by not one code value.
    if (op == kFreeze)      { return B <= 0.0 ? 0.0 : max(0.0, 1.0 - sqrt(max(0.0, 1.0 - A)) / B); }
    if (op == kFrom)        { return B - A; }
    if (op == kGeometric)   { return A + B <= 0.0 ? 0.0 : 2.0 * A * B / (A + B); }
    if (op == kGrainExtract) { return B - A + 0.5; }
    if (op == kGrainMerge)  { return B + A - 0.5; }
    if (op == kHardLight)   { return A < 0.5 ? 2.0 * A * B : 1.0 - 2.0 * (1.0 - A) * (1.0 - B); }
    if (op == kHypot)       { return sqrt(A * A + B * B); }
    if (op == kIn)          { return A * b; }
    if (op == kMask)        { return B * a; }
    if (op == kMatte)       { return A * a + B * (1.0 - a); }
    if (op == kMax)         { return max(A, B); }
    if (op == kMin)         { return min(A, B); }
    if (op == kMinus)       { return A - B; }
    if (op == kMultiply)    { return A * B; }
    if (op == kOut)         { return A * (1.0 - b); }
    if (op == kOver)        { return A + B * (1.0 - a); }
    if (op == kOverlay)     { return B < 0.5 ? 2.0 * A * B : 1.0 - 2.0 * (1.0 - A) * (1.0 - B); }
    if (op == kPinLight)    { return B >= 2.0 * A ? 2.0 * A : (B > 2.0 * A - 1.0 ? B : 2.0 * A - 1.0); }
    if (op == kPlus)        { return A + B; }
    if (op == kReflect)     { return B >= 1.0 ? 1.0 : min(1.0, A * A / (1.0 - B)); }
    if (op == kScreen)      { return A + B - A * B; }
    if (op == kSoftLight) {
        // The photographic soft light, with the knee at 0.5 -- the same one
        // openfx-misc uses, and not the simpler A*B + A*(1-A*B) variant that
        // several other applications call by the same name.
        // The knee is on A, and the upper branch's D(B) is a cubic below a
        // quarter rather than a square root. That second knee is the whole
        // difference, and it is worth saying how it was found because the
        // obvious probe missed it: the alpha-1 ramp agreed to three decimals
        // everywhere, since sqrt(B) and the cubic differ by less than a
        // thousandth over most of their range -- 0.17580 against 0.17649 at
        // B=0.143. What gave it away was that on the plate the green channel
        // agreed exactly while red and blue did not: green was the channel
        // whose background sat above a quarter.
        //
        // A probe that agrees is not a probe that confirms. This one was short
        // three decimal places of saying anything at all.
        const float d = B <= 0.25 ? ((16.0 * B - 12.0) * B + 4.0) * B
                                  : sqrt(max(B, 0.0));
        return A < 0.5 ? 2.0 * A * B + B * B * (1.0 - 2.0 * A)
                       : B + (2.0 * A - 1.0) * (d - B);
    }
    if (op == kStencil)     { return B * (1.0 - a); }
    if (op == kUnder)       { return A * (1.0 - b) + B; }
    if (op == kXor)         { return A * (1.0 - b) + B * (1.0 - a); }
    return A;
}

// --- the four that are not per-component -----------------------------------
//
// Hue, saturation, colour and luminosity take one attribute from A and the rest
// from B, which needs all three components at once. These are the definitions
// from the PDF blend-mode specification, which is where every implementation of
// them comes from.

float lum(float3 c) { return 0.3 * c.x + 0.59 * c.y + 0.11 * c.z; }

float3 clipColour(float3 c) {
    const float l = lum(c);
    const float low = min(c.x, min(c.y, c.z));
    const float high = max(c.x, max(c.y, c.z));
    if (low < 0.0) {
        c = l + (c - l) * l / max(l - low, 1e-8);
    }
    if (high > 1.0) {
        c = l + (c - l) * (1.0 - l) / max(high - l, 1e-8);
    }
    return c;
}

float3 setLum(float3 c, float l) { return clipColour(c + (l - lum(c))); }

float sat(float3 c) {
    return max(c.x, max(c.y, c.z)) - min(c.x, min(c.y, c.z));
}

float3 setSat(float3 c, float s) {
    const float high = max(c.x, max(c.y, c.z));
    const float low = min(c.x, min(c.y, c.z));
    if (high <= low) {
        return float3(0.0, 0.0, 0.0);
    }
    return (c - low) * s / (high - low);
}

float3 mergeHsl(uint op, float3 A, float3 B) {
    if (op == kHue)        { return setLum(setSat(A, sat(B)), lum(B)); }
    if (op == kSaturation) { return setLum(setSat(B, sat(A)), lum(B)); }
    if (op == kColor)      { return setLum(A, lum(B)); }
    if (op == kLuminosity) { return setLum(B, lum(A)); }
    return A;
}

bool isHsl(uint op) {
    return op == kHue || op == kSaturation || op == kColor || op == kLuminosity;
}

float4 sample(StructuredBuffer<float4> buffer, int x, int y, uint w, uint h,
              uint stride) {
    // Outside an input there is nothing, which for a merge is the whole point:
    // where B does not reach, A is composited onto transparent black, and the
    // union of the two regions is a picture rather than an edge.
    if (x < 0 || y < 0 || x >= int(w) || y >= int(h)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return buffer[uint(y) * stride + uint(x)];
}

[shader("compute")]
[numthreads(16, 16, 1)]
void mergeMain(uint3 tid: SV_DispatchThreadID) {
    const uint2 xy = tid.xy;
    if (xy.x >= params.width || xy.y >= params.height) {
        return;
    }

    const float4 A = sample(srcA, int(xy.x) + params.aOffsetX,
                            int(xy.y) + params.aOffsetY, params.aWidth,
                            params.aHeight, params.aStride);
    const float4 B = sample(srcB, int(xy.x) + params.bOffsetX,
                            int(xy.y) + params.bOffsetY, params.bWidth,
                            params.bHeight, params.bStride);

    float4 out;
    if (isHsl(params.operation)) {
        // Weighted by A's alpha, which the other thirty-five carry inside their
        // own formulas and these four do not.
        //
        // A blend mode is a statement about two colours and says nothing about
        // coverage; `over` has the (1 - a) in it, `setLum` has nowhere to put
        // one. Without this, everywhere A is transparent came out as B blended
        // against transparent black -- so the region around a small element
        // went dark instead of being left alone, and the plugin was right.
        //
        // The colour is unpremultiplied first: these operate on colour, and a
        // premultiplied value is a colour already multiplied by its coverage,
        // so blending it treats a half-covered red as a dark red.
        const float3 aColour = A.w > 1e-6 ? A.xyz / A.w : A.xyz;
        const float3 bColour = B.w > 1e-6 ? B.xyz / B.w : B.xyz;
        const float3 blended = mergeHsl(params.operation, aColour, bColour);
        const float3 colour = lerp(bColour, blended, A.w);
        const float alpha = A.w + B.w - A.w * B.w;
        out = float4(colour * alpha, alpha);
    } else {
        out = float4(mergeOne(params.operation, A.x, B.x, A.w, B.w),
                     mergeOne(params.operation, A.y, B.y, A.w, B.w),
                     mergeOne(params.operation, A.z, B.z, A.w, B.w),
                     mergeOne(params.operation, A.w, B.w, A.w, B.w));
        if (params.screenAlpha != 0u) {
            out.w = A.w + B.w - A.w * B.w;
        }
    }
    dst[xy.y * params.dstStride + xy.x] = out;
}
