// Copyright (c) 2026 aopenfx contributors.
//
// Four patterns, one file, one blob.
//
// They share a parameter struct and a single output buffer because that is
// what entry points in one Slang file are for: the layout is declared once and
// cannot drift between them. Every one of them writes a pixel from its
// coordinate and reads nothing, which is the definition of a generator and the
// reason none of them takes a source.
//
// COORDINATES ARE THE IMAGE'S, NOT THE TILE'S
//
// `origin` is the output rectangle's own corner, so a checker drawn into a
// region that starts at x=512 has its squares where a checker drawn into the
// whole frame would. Without it, every render at a different region would put
// the pattern somewhere else -- which is invisible until the day a crop moves.
//
// And `frame` is the whole picture this tile is part of -- `outputRod`, not the
// output buffer. The two patterns that are laid out as a *proportion* of the
// picture, the bars and the wheel, must be: measured against the tile, a render
// split into four would draw four sets of bars and four wheels, each complete
// and each wrong, with the seams in the obvious places. Nothing about a small
// picture makes that happen, which is why it would have shipped.

struct GenerateParams {
    uint  dstWidth;
    uint  dstHeight;
    uint  dstStride;
    int   originX;

    int   originY;
    int   frameX1;    ///< the whole picture this tile belongs to
    int   frameY1;
    int   frameX2;

    int   frameY2;
    uint  pad0;
    uint  pad1;
    uint  pad2;

    float4 colour;        ///< Constant

    float4 checker0;      ///< CheckerBoard: the 2x2 cell, reading rows upward
    float4 checker1;
    float4 checker2;
    float4 checker3;
    float4 lineColour;

    float2 boxSize;
    float  lineWidth;
    float  barLevel;      ///< ColorBars: 0.75 or 1.0

    float centreSaturation;   ///< ColorWheel
    float edgeSaturation;
    float centreValue;
    float edgeValue;

    float wheelGamma;
    float rotate;         ///< degrees
    float pad3;
    float pad4;
};

RWStructuredBuffer<float4>    dst;
ConstantBuffer<GenerateParams> params;

bool outside(uint2 xy) {
    return xy.x >= params.dstWidth || xy.y >= params.dstHeight;
}

/// Where this pixel is in the picture, not in the tile.
int2 imagePoint(uint2 xy) {
    return int2(params.originX + int(xy.x), params.originY + int(xy.y));
}

/// Inside the picture the pattern is laid out over.
///
/// Only ever false where the host padded the render out past the region this
/// node said it covered -- a Constant with a Size inside a larger format. There
/// the answer is transparent black and not the pattern's edge colour, because
/// a generator asked for a box should produce a box and not a tint over
/// everything.
bool inFrame(int2 p) {
    return p.x >= params.frameX1 && p.x < params.frameX2 &&
           p.y >= params.frameY1 && p.y < params.frameY2;
}

static const float4 kNothing = float4(0.0, 0.0, 0.0, 0.0);

// ---------------------------------------------------------------------------

[shader("compute")]
[numthreads(16, 16, 1)]
void constantMain(uint3 tid: SV_DispatchThreadID) {
    const uint2 xy = tid.xy;
    if (outside(xy)) {
        return;
    }
    dst[xy.y * params.dstStride + xy.x] =
        inFrame(imagePoint(xy)) ? params.colour : kNothing;
}

// ---------------------------------------------------------------------------

[shader("compute")]
[numthreads(16, 16, 1)]
void checkerBoardMain(uint3 tid: SV_DispatchThreadID) {
    const uint2 xy = tid.xy;
    if (outside(xy)) {
        return;
    }
    const int2 at = imagePoint(xy);
    if (!inFrame(at)) {
        dst[xy.y * params.dstStride + xy.x] = kNothing;
        return;
    }
    const float px = float(at.x) + 0.5;
    const float py = float(at.y) + 0.5;

    const float bw = max(params.boxSize.x, 1.0);
    const float bh = max(params.boxSize.y, 1.0);
    const float u = px / bw;
    const float v = py / bh;
    const int   cx = int(floor(u));
    const int   cy = int(floor(v));

    // The cell of a 2x2 supercell, so four colours tile without a seam. `&1`
    // on a negative would fold the wrong way, so the parity is taken from a
    // sum that cannot be negative in any picture anybody makes.
    const uint  parity = uint(cx & 1) + 2u * uint(cy & 1);
    float4 out = params.checker0;
    if (parity == 1u) { out = params.checker1; }
    else if (parity == 2u) { out = params.checker2; }
    else if (parity == 3u) { out = params.checker3; }

    // The grid itself: distance to the nearest cell border, in pixels.
    if (params.lineWidth > 0.0) {
        const float dx = min(frac(u), 1.0 - frac(u)) * bw;
        const float dy = min(frac(v), 1.0 - frac(v)) * bh;
        if (min(dx, dy) < params.lineWidth * 0.5) {
            out = params.lineColour;
        }
    }
    dst[xy.y * params.dstStride + xy.x] = out;
}

// ---------------------------------------------------------------------------
//
// SMPTE ECR 1-1978, the bars everybody means when they say colour bars: seven
// bars over two thirds, a reversed blue strip, and a bottom quarter with the
// -I / +Q patches and the three-step PLUGE. `barLevel` is 0.75 or 1.0 -- the
// difference between bars a legaliser passes and bars that clip.

float3 barAt(uint index, float level) {
    // White, yellow, cyan, green, magenta, red, blue.
    if (index == 0u) { return float3(level, level, level); }
    if (index == 1u) { return float3(level, level, 0.0); }
    if (index == 2u) { return float3(0.0, level, level); }
    if (index == 3u) { return float3(0.0, level, 0.0); }
    if (index == 4u) { return float3(level, 0.0, level); }
    if (index == 5u) { return float3(level, 0.0, 0.0); }
    return float3(0.0, 0.0, level);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void colorBarsMain(uint3 tid: SV_DispatchThreadID) {
    const uint2 xy = tid.xy;
    if (outside(xy)) {
        return;
    }
    const int2 at = imagePoint(xy);
    if (!inFrame(at)) {
        dst[xy.y * params.dstStride + xy.x] = kNothing;
        return;
    }
    // u across, and v from the TOP, because the pattern is described from the
    // top and pictures here have y going up.
    const float fw = float(max(params.frameX2 - params.frameX1, 1));
    const float fh = float(max(params.frameY2 - params.frameY1, 1));
    const float u = (float(at.x - params.frameX1) + 0.5) / fw;
    const float v = 1.0 - (float(at.y - params.frameY1) + 0.5) / fh;

    const float level = params.barLevel;
    float3 out;

    if (v < 2.0 / 3.0) {
        out = barAt(min(uint(u * 7.0), 6u), level);
    } else if (v < 0.75) {
        // The reverse blue strip, aligned to the seven bars above it.
        const uint bar = min(uint(u * 7.0), 6u);
        if (bar == 0u) { out = float3(0.0, 0.0, level); }
        else if (bar == 1u) { out = float3(0.0, 0.0, 0.0); }
        else if (bar == 2u) { out = float3(level, 0.0, level); }
        else if (bar == 3u) { out = float3(0.0, 0.0, 0.0); }
        else if (bar == 4u) { out = float3(0.0, level, level); }
        else if (bar == 5u) { out = float3(0.0, 0.0, 0.0); }
        else { out = float3(level, level, level); }
    } else {
        // -I, white, +Q, black, then PLUGE at -4%, 0 and +4%, then black.
        const float t = u * 28.0;
        if (t < 5.0) { out = float3(0.0, 0.129, 0.298); }
        else if (t < 10.0) { out = float3(1.0, 1.0, 1.0); }
        else if (t < 15.0) { out = float3(0.176, 0.0, 0.353); }
        else if (t < 20.0) { out = float3(0.0, 0.0, 0.0); }
        else if (t < 22.0) { out = float3(-0.04, -0.04, -0.04); }
        else if (t < 24.0) { out = float3(0.0, 0.0, 0.0); }
        else if (t < 26.0) { out = float3(0.04, 0.04, 0.04); }
        else { out = float3(0.0, 0.0, 0.0); }
    }
    dst[xy.y * params.dstStride + xy.x] = float4(out, 1.0);
}

// ---------------------------------------------------------------------------

float3 fromHsv(float h, float s, float v) {
    const float hue = frac(h);
    const float sector = hue * 6.0;
    const float f = frac(sector);
    const float p = v * (1.0 - s);
    const float q = v * (1.0 - s * f);
    const float t = v * (1.0 - s * (1.0 - f));
    const uint  which = uint(sector) % 6u;
    if (which == 0u) { return float3(v, t, p); }
    if (which == 1u) { return float3(q, v, p); }
    if (which == 2u) { return float3(p, v, t); }
    if (which == 3u) { return float3(p, q, v); }
    if (which == 4u) { return float3(t, p, v); }
    return float3(v, p, q);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void colorWheelMain(uint3 tid: SV_DispatchThreadID) {
    const uint2 xy = tid.xy;
    if (outside(xy)) {
        return;
    }
    const int2 at = imagePoint(xy);
    if (!inFrame(at)) {
        dst[xy.y * params.dstStride + xy.x] = kNothing;
        return;
    }
    const float w = float(max(params.frameX2 - params.frameX1, 1));
    const float h = float(max(params.frameY2 - params.frameY1, 1));
    // The largest circle that fits, centred: a wheel that stretched with the
    // format would not be a wheel.
    const float half = min(w, h) * 0.5;
    const float dx = (float(at.x - params.frameX1) + 0.5) - w * 0.5;
    const float dy = (float(at.y - params.frameY1) + 0.5) - h * 0.5;
    const float r = sqrt(dx * dx + dy * dy) / max(half, 1.0);
    if (r > 1.0) {
        dst[xy.y * params.dstStride + xy.x] = kNothing;
        return;
    }
    const float turn = params.rotate / 360.0;
    const float hue = atan2(dy, dx) / (2.0 * 3.14159265358979) + turn;
    const float sat = lerp(params.centreSaturation, params.edgeSaturation, r);
    const float val = lerp(params.centreValue, params.edgeValue, r);
    float3 rgb = fromHsv(hue, saturate(sat), max(val, 0.0));
    const float g = params.wheelGamma;
    if (g > 0.0 && abs(g - 1.0) > 1e-6) {
        rgb = pow(max(rgb, 0.0), float3(1.0 / g, 1.0 / g, 1.0 / g));
    }
    dst[xy.y * params.dstStride + xy.x] = float4(rgb, 1.0);
}
