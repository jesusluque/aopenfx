// Copyright (c) 2026 aopenfx contributors.
//
// A gaussian blur, one axis at a time.
//
// Separable, because a gaussian is: blurring along x and then along y gives the
// same picture as one two-dimensional pass and costs 2r taps a pixel instead of
// r squared. At size 45 that is 112 samples per pixel rather than 3136, which
// is the difference between a frame and a slideshow.
//
// One kernel run twice rather than two kernels, with `stepX`/`stepY` saying
// which way to walk. The arithmetic is symmetric by definition, and two copies
// of it is two places for it to drift.
//
// WHY NOT A RECURSIVE FILTER
//
// CImg's blur is IIR and costs the same at any radius, which is the right
// design on a CPU and the wrong one here. An IIR pass is a sequential walk
// along each row, and not walking is the whole of a GPU's advantage. A direct
// convolution is embarrassingly parallel, and at these radii the arithmetic is
// free next to the memory traffic either way.
//
// Nothing is imported. A plugin's kernel cannot reach the host's own Slang
// modules, and should not want to: the four lines it would save are four lines
// it now cannot have taken away by a host it does not control.

/// Matched byte for byte by `BlurUniforms` in Blur.cpp. Four-byte members in
/// declaration order: no padding for the two compilers to disagree about.
struct BlurParams {
    /// The destination's extent: what this dispatch walks.
    uint  width;
    uint  height;
    /// The source's, which is not the same thing. A blur's output is bigger
    /// than its input by what the filter reaches, so indexing the source by the
    /// destination's coordinates reads off the end of every row and wraps onto
    /// the next -- the picture repeating down the side.
    uint  srcWidth;
    uint  srcHeight;
    /// Destination pixel (0,0) sits at this source coordinate. Negative when
    /// the output is the larger of the two, which is the usual case here.
    int   srcOffsetX;
    int   srcOffsetY;
    uint  srcStride;   // in pixels
    uint  dstStride;   // in pixels
    /// Taps either side of the centre. Zero is a copy, which is what a blur of
    /// nothing should be rather than a special case at the call site.
    uint  radius;
    /// Which way this pass walks. One of the two is always zero.
    int   stepX;
    int   stepY;
    /// Standard deviation in pixels. Carried rather than derived from the
    /// radius, so the host owns the relationship between the two: the radius is
    /// where the tail is cut off, and how many sigmas that is is a decision.
    float sigma;
};

StructuredBuffer<float4>   src;
RWStructuredBuffer<float4> dst;
ConstantBuffer<BlurParams> params;

[shader("compute")]
[numthreads(16, 16, 1)]
void blurMain(uint3 tid: SV_DispatchThreadID) {
    const uint2 xy = tid.xy;
    // A dispatch is rounded up to whole thread groups, so the last group runs
    // off the edge of the picture.
    if (xy.x >= params.width || xy.y >= params.height) {
        return;
    }

    // Where this destination pixel lands in the source.
    const int sourceX = int(xy.x) + params.srcOffsetX;
    const int sourceY = int(xy.y) + params.srcOffsetY;

    const uint at = xy.y * params.dstStride + xy.x;
    if (params.radius == 0u || params.sigma <= 0.0) {
        // A straight copy, with the rule the blur below follows: outside the
        // input there is nothing. It used to repeat the edge pixel here, so a
        // zero-size blur over a region larger than its input grew a smear the
        // real blur never would.
        if (sourceX < 0 || sourceY < 0 || sourceX >= int(params.srcWidth) ||
            sourceY >= int(params.srcHeight)) {
            dst[at] = float4(0.0, 0.0, 0.0, 0.0);
        } else {
            dst[at] = src[uint(sourceY) * params.srcStride + uint(sourceX)];
        }
        return;
    }

    // Outside the input there is nothing, and the sum is over the whole kernel
    // whether or not a tap landed on picture.
    //
    // This started the other way round -- taps outside were clamped to the edge
    // pixel and the sum counted only what was used, so the border kept its full
    // brightness. That is wrong, and comparing against CImgBlur is what showed
    // it: the two filters agree to a single code value in the interior and
    // differed by 140 at the plate's corner. A hard-edged plate blurred that
    // way becomes a larger hard-edged plate with a soft ramp inside it, when
    // what a blur does is let the picture *fade out* past its edge -- which is
    // the entire reason its region of definition grows.
    //
    // On a premultiplied image the alpha falls off with the colour, so the
    // result is a picture that ends softly rather than one that ends.
    const float twoSigmaSquared = 2.0 * params.sigma * params.sigma;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float  total = 0.0;
    const int radius = int(params.radius);
    for (int t = -radius; t <= radius; ++t) {
        const float weight = exp(-float(t * t) / twoSigmaSquared);
        // Counted whether or not it lands on picture: that is what makes the
        // edge fall off instead of holding its brightness.
        total = total + weight;
        const int sx = sourceX + t * params.stepX;
        const int sy = sourceY + t * params.stepY;
        if (sx < 0 || sy < 0 || sx >= int(params.srcWidth) ||
            sy >= int(params.srcHeight)) {
            continue;
        }
        sum = sum + src[uint(sy) * params.srcStride + uint(sx)] * weight;
    }
    // Premultiplied throughout, so alpha is filtered with the colour. Filtering
    // only RGB would unpremultiply the image wherever the blur crossed an edge,
    // and it shows up three operations later as a dark fringe.
    dst[at] = sum / total;
}
