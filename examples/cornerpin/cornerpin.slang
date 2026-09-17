// Copyright (c) 2026 aopenfx contributors.
//
// A picture pinned to four corners, and the mesh that stops it being flat.
//
// The kernel is the easy half and it is the same one every corner pin has: for
// each output pixel, map backwards through the homography, sample there. What
// takes explaining is why there is a mesh at all.
//
// A homography is exactly right for a plane, and a road is not a plane. It
// crowns towards the middle, it dips at the gutter, and over the twelve metres
// an advertisement occupies that is several pixels of disagreement between
// where the maths says a point is and where it is. Painted onto asphalt, those
// pixels read as the advertisement sliding under the wheels. cyclAD's answer
// was a GY x GX mesh of offsets over the quad, and this is the same: the
// homography does the perspective and the mesh does what the perspective got
// wrong.
//
// The mesh is bilinear between its knots, and its offsets are in the *source*
// picture, added after the backward map. That is the cheap direction: an
// offset applied to where we look costs one addition per pixel, where an
// offset applied to where we draw would need a scatter.

struct PinParams {
    uint  srcWidth;
    uint  srcHeight;
    uint  srcStride;
    /// Where the source's own rectangle sits. Everything in this kernel is in
    /// canonical coordinates -- the ones the corners are given in -- because
    /// the map produces them, and converting to a buffer index is the last
    /// thing that happens rather than something each expression does its own
    /// version of. Mixing the two is what put the artwork half off its quad.
    int   srcOriginX;
    int   srcOriginY;
    uint  dstWidth;
    uint  dstHeight;
    uint  dstStride;
    /// Where the output's own rectangle sits, so a pixel index can be turned
    /// into the canonical coordinate the corners are expressed in.
    int   dstOriginX;
    int   dstOriginY;
    /// The backward map, output canonical to input canonical. Row major, and
    /// the bottom right is 1 by construction so it is not carried.
    float h0, h1, h2, h3, h4, h5, h6, h7;
    /// The mesh, or 1x1 for none. Knots, not cells: a 3x3 mesh has nine
    /// offsets and four cells.
    uint  meshX;
    uint  meshY;
    /// How far outside the quad still counts, as a fraction of it. A pin with
    /// a hard edge on a moving picture crawls; a few pixels of ramp does not.
    float softness;
    uint  filter;    ///< 0 nearest, 1 bilinear, 2 supersampled (anisotropic box)
    uint  blackOutside;

    /// Motion blur: how far each corner moved since the last frame, in the
    /// output's pixels, and the shutter -- the fraction of that movement
    /// the exposure covers. Zero corners, or a shutter of zero, is off.
    float m0x; float m0y; float m1x; float m1y;
    float m2x; float m2y; float m3x; float m3y;
    float shutter;
    uint  blurOn;
};

StructuredBuffer<float4>  src;
/// Two floats per knot, in source pixels. Bound always; `meshX * meshY` of 1
/// means it is one zero and never asked for anything.
StructuredBuffer<float2>  mesh;
RWStructuredBuffer<float4> dst;
ConstantBuffer<PinParams> params;

// ---------------------------------------------------------------------------

/// A source texel, addressed in canonical coordinates.
float4 fetchSource(int cx, int cy) {
    const int x = cx - params.srcOriginX;
    const int y = cy - params.srcOriginY;
    if (x < 0 || y < 0 || x >= int(params.srcWidth) ||
        y >= int(params.srcHeight)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return src[uint(y) * params.srcStride + uint(x)];
}

float4 sampleSource(float u, float v) {
    if (params.filter == 0u) {
        return fetchSource(int(floor(u)), int(floor(v)));
    }
    const int   x0 = int(floor(u - 0.5));
    const int   y0 = int(floor(v - 0.5));
    const float fx = u - 0.5 - float(x0);
    const float fy = v - 0.5 - float(y0);
    const float4 a = fetchSource(x0, y0);
    const float4 b = fetchSource(x0 + 1, y0);
    const float4 c = fetchSource(x0, y0 + 1);
    const float4 d = fetchSource(x0 + 1, y0 + 1);
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}

/// The mesh offset at a position inside the quad, in source pixels.
///
/// `s` and `t` are where the pixel landed in the source quad's own unit
/// square, which is what the backward map produces before it is turned into
/// pixels -- so the mesh follows the advertisement rather than the frame, and
/// stays put when the camera moves.
float2 meshAt(float s, float t) {
    if (params.meshX < 2u && params.meshY < 2u) {
        return float2(0.0, 0.0);
    }
    const float x = clamp(s, 0.0, 1.0) * float(params.meshX - 1u);
    const float y = clamp(t, 0.0, 1.0) * float(params.meshY - 1u);
    const int   x0 = clamp(int(floor(x)), 0, int(params.meshX) - 1);
    const int   y0 = clamp(int(floor(y)), 0, int(params.meshY) - 1);
    const int   x1 = min(x0 + 1, int(params.meshX) - 1);
    const int   y1 = min(y0 + 1, int(params.meshY) - 1);
    const float fx = x - float(x0);
    const float fy = y - float(y0);

    const float2 a = mesh[uint(y0) * params.meshX + uint(x0)];
    const float2 b = mesh[uint(y0) * params.meshX + uint(x1)];
    const float2 c = mesh[uint(y1) * params.meshX + uint(x0)];
    const float2 d = mesh[uint(y1) * params.meshX + uint(x1)];
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void pinWarp(uint3 id : SV_DispatchThreadID) {
    if (id.x >= params.dstWidth || id.y >= params.dstHeight) {
        return;
    }
    const uint at = id.y * params.dstStride + id.x;

    // The pixel's centre, in the coordinates the corners were given in.
    const float px = float(int(id.x) + params.dstOriginX) + 0.5;
    const float py = float(int(id.y) + params.dstOriginY) + 0.5;

    const float w = params.h6 * px + params.h7 * py + 1.0;
    if (abs(w) < 1.0e-9) {
        // The horizon. A quad dragged past its own vanishing point has a line
        // where the map is undefined, and the honest answer there is nothing --
        // not a division that produces an enormous coordinate and reads the
        // source at whatever it happens to land on.
        dst[at] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    // Bounded before anything turns them into an index.
    //
    // `w` is guarded against zero above, but it is allowed to be very small,
    // and a divisor of 1e-8 produces a coordinate of 1e11. Converting a float
    // that large -- or a NaN, which the same arithmetic can reach -- to an int
    // is undefined behaviour, and the bounds check in `fetchSource` is written
    // against an int that is already meaningless. That is a read at an
    // arbitrary address on the device, which is a faulted context and a dead
    // process rather than a wrong pixel.
    //
    // A million pixels either side of the source is far outside anything real
    // and comfortably inside what an int holds. Clamping rather than
    // discarding, so a quad approaching its own vanishing point degrades into
    // its edge pixels instead of into a hole.
    const float kFar = 1.0e6;
    const float sx = clamp((params.h0 * px + params.h1 * py + params.h2) / w,
                           -kFar, kFar);
    const float sy = clamp((params.h3 * px + params.h4 * py + params.h5) / w,
                           -kFar, kFar);

    // Where that is inside the source, for the mesh: a normalisation, not a
    // second map.
    const float s = (sx - float(params.srcOriginX)) /
                    max(float(params.srcWidth), 1.0);
    const float t = (sy - float(params.srcOriginY)) /
                    max(float(params.srcHeight), 1.0);
    const float2 bend = meshAt(s, t);

    float4 out;
    if (params.filter == 2u) {
        // Supersampled: the map's Jacobian at this pixel says how many
        // source pixels one output pixel covers, each way. a,b,c,d are the
        // derivatives of (sx, sy) with respect to (px, py); the column norms
        // are the footprint along the output's own axes, so a floor seen at
        // a grazing angle gets many taps up the picture and few across it --
        // anisotropic, where a single scalar would blur the axis that did
        // not need it. Each tap goes through the whole homography, which is
        // exact under perspective where a linear step would not be.
        const float a = (params.h0 - sx * params.h6) / w;
        const float b = (params.h1 - sx * params.h7) / w;
        const float c = (params.h3 - sy * params.h6) / w;
        const float d = (params.h4 - sy * params.h7) / w;
        const int nx = clamp(int(ceil(sqrt(a * a + c * c))), 1, 8);
        const int ny = clamp(int(ceil(sqrt(b * b + d * d))), 1, 8);
        if (nx == 1 && ny == 1) {
            out = sampleSource(sx + bend.x, sy + bend.y);
        } else {
            float4 sum = float4(0.0, 0.0, 0.0, 0.0);
            int taps = 0;
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    const float ox = px + (float(i) + 0.5) / float(nx) - 0.5;
                    const float oy = py + (float(j) + 0.5) / float(ny) - 0.5;
                    const float wt = params.h6 * ox + params.h7 * oy + 1.0;
                    if (abs(wt) < 1.0e-9) {
                        continue;
                    }
                    const float tx = clamp((params.h0 * ox + params.h1 * oy + params.h2) / wt,
                                           -kFar, kFar);
                    const float ty = clamp((params.h3 * ox + params.h4 * oy + params.h5) / wt,
                                           -kFar, kFar);
                    sum += sampleSource(tx + bend.x, ty + bend.y);
                    ++taps;
                }
            }
            out = taps > 0 ? sum / float(taps) : float4(0.0, 0.0, 0.0, 0.0);
        }
    } else {
        out = sampleSource(sx + bend.x, sy + bend.y);
    }

    if (params.blackOutside != 0u) {
        // Outside the source, transparent -- with a ramp, because a hard edge
        // on a pin that moves a fraction of a pixel a frame crawls, and the
        // crawl is what makes a composite look stuck on.
        const float soft = max(params.softness, 1.0e-4);
        const float left = sx - float(params.srcOriginX);
        const float bottom = sy - float(params.srcOriginY);
        const float2 inside =
            float2(min(left, float(params.srcWidth) - left),
                   min(bottom, float(params.srcHeight) - bottom));
        const float edge = saturate(min(inside.x, inside.y) / soft);
        out *= edge;
    }
    dst[at] = out;
}


/// The pinned picture, blurred along the motion the track measured.
///
/// The corners moved between this frame and the last; a real camera would
/// have smeared the paint on the road by the shutter's share of that. The
/// motion at a pixel is the corners' motion interpolated across the quad --
/// through the same backward map the warp uses, so it is the quad's own
/// parametrisation and not the frame's -- and the picture is averaged along
/// it. `src` here is the warped picture, not the source.
[numthreads(16, 16, 1)]
void pinBlur(uint3 id : SV_DispatchThreadID) {
    if (id.x >= params.dstWidth || id.y >= params.dstHeight) {
        return;
    }
    const uint at = id.y * params.dstStride + id.x;
    const float4 here = src[at];
    if (params.blurOn == 0u) {
        dst[at] = here;
        return;
    }
    const float px = float(int(id.x) + params.dstOriginX) + 0.5;
    const float py = float(int(id.y) + params.dstOriginY) + 0.5;
    const float w = params.h6 * px + params.h7 * py + 1.0;
    if (abs(w) < 1.0e-9) {
        dst[at] = here;
        return;
    }
    const float kFar = 1.0e6;
    const float sx = clamp((params.h0 * px + params.h1 * py + params.h2) / w, -kFar, kFar);
    const float sy = clamp((params.h3 * px + params.h4 * py + params.h5) / w, -kFar, kFar);
    const float s = saturate((sx - float(params.srcOriginX)) / max(float(params.srcWidth), 1.0));
    const float t = saturate((sy - float(params.srcOriginY)) / max(float(params.srcHeight), 1.0));
    // Corners anticlockwise from bottom-left: 0 (0,0), 1 (1,0), 2 (1,1), 3 (0,1).
    const float2 m = (float2(params.m0x, params.m0y) * (1.0 - s) * (1.0 - t) +
                      float2(params.m1x, params.m1y) * s * (1.0 - t) +
                      float2(params.m2x, params.m2y) * s * t +
                      float2(params.m3x, params.m3y) * (1.0 - s) * t) * params.shutter;
    const float length = sqrt(m.x * m.x + m.y * m.y);
    if (length < 0.75) {
        dst[at] = here;
        return;
    }
    // One tap a pixel of travel, capped: a frame that moved a hundred
    // pixels is a smear, and a smear does not need a hundred reads.
    const int taps = clamp(int(ceil(length)), 2, 32);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    for (int k = 0; k < taps; ++k) {
        const float f = (float(k) + 0.5) / float(taps) - 0.5;
        const int qx = int(id.x) + int(round(m.x * f));
        const int qy = int(id.y) + int(round(m.y * f));
        if (qx < 0 || qy < 0 || qx >= int(params.dstWidth) || qy >= int(params.dstHeight)) {
            continue;   // off the picture: transparent, which is what is there
        }
        sum += src[uint(qy) * params.dstStride + uint(qx)];
    }
    dst[at] = sum / float(taps);
}
