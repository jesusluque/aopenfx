// Copyright (c) 2026 aopenfx contributors.
//
// One, minus the picture.
//
// Deliberately the same node as openfx-misc's InvertOFX, control for control,
// so that swapping one for the other in a script changes nothing but the
// speed: the same (un)premult pair, the same mask with the same inversion,
// the same mix. What it does not repeat are that plugin's four process
// switches -- this host gives every AOFX effect R, G, B and A of its own, it
// applies them itself, and a second set beside them would be two answers to
// one question.
//
// WHY (UN)PREMULT IS A SWITCH AND NOT A RULE
//
// `1 - rgb` on a premultiplied pixel is not the inverse of anything. A pixel
// half covered by mid grey is stored as 0.25; inverting that gives 0.75, which
// read back as half-covered is a colour of 1.5 -- so a soft edge inverts
// brighter than the middle of the same shape and the matte grows a halo.
//
// Off by default all the same, because that is InvertOFX's default and this
// node exists to be interchangeable with it. Somebody inverting a premultiplied
// plate wants it on; somebody inverting a depth pass, a normal map or a matte
// does not, and for those alpha is one anyway.

struct InvertParams {
    uint  srcWidth;
    uint  srcHeight;
    uint  srcStride;
    int   srcOffsetX;
    int   srcOffsetY;
    uint  dstWidth;
    uint  dstHeight;
    uint  dstStride;
    /// The mask, and where it sits relative to what is being written.
    uint  hasMask;
    uint  maskWidth;
    uint  maskHeight;
    uint  maskStride;
    int   maskOffsetX;
    int   maskOffsetY;
    /// 1 when the effect belongs where the mask is *dark*.
    uint  maskInvert;
    /// 1 to divide by a channel before inverting and multiply back after.
    uint  premult;
    /// Which channel that is: 0 R, 1 G, 2 B, 3 A.
    uint  premultChannel;
    float mix;
};

StructuredBuffer<float4>     src;
StructuredBuffer<float4>     mask;
RWStructuredBuffer<float4>   dst;
ConstantBuffer<InvertParams> params;

/// The source pixel under destination pixel `xy`. Outside the source is
/// transparent.
///
/// Offset first, then bounds-checked. The destination and the source are two
/// rectangles that need not share an origin or a size -- the offset is
/// `target.rect.x1 - source.rect.x1`, and it is not zero whenever the host
/// hands over more (or less) of the input than it asks to be written. The
/// first version clamped the *destination* coordinate against the *source's*
/// size and added the offset afterwards, which reads past the end of `src` as
/// soon as the offset is positive: on the device that is not a wrong pixel but
/// a read at an arbitrary address. Transparent rather than the edge pixel
/// repeated, the convention every other example uses (see grade.slang).
float4 fetchSource(uint2 xy) {
    const int sx = int(xy.x) + params.srcOffsetX;
    const int sy = int(xy.y) + params.srcOffsetY;
    if (sx < 0 || sy < 0 || sx >= int(params.srcWidth) ||
        sy >= int(params.srcHeight)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return src[uint(sy) * params.srcStride + uint(sx)];
}

/// How much of the effect this pixel gets, from the mask and the mix.
float amountAt(uint2 xy) {
    if (params.hasMask == 0u) {
        // No mask is no opinion, and `Invert Mask` has nothing to invert. A
        // node that read it anyway would apply nothing at all the moment the
        // switch was ticked with the port empty.
        return params.mix;
    }
    float covered = 0.0f;   // outside the mask is outside the effect
    const int mx = int(xy.x) + params.maskOffsetX;
    const int my = int(xy.y) + params.maskOffsetY;
    if (mx >= 0 && my >= 0 && mx < int(params.maskWidth) &&
        my < int(params.maskHeight)) {
        covered = mask[uint(my) * params.maskStride + uint(mx)].w;
    }
    if (params.maskInvert != 0u) {
        covered = 1.0f - covered;
    }
    return params.mix * covered;
}

float channelOf(float4 rgba, uint which) {
    return which == 0u ? rgba.x : which == 1u ? rgba.y
                               : which == 2u ? rgba.z
                                             : rgba.w;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void invertMain(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= params.dstWidth || tid.y >= params.dstHeight) {
        return;
    }
    const float4 rgba = fetchSource(tid.xy);

    // Divided by the chosen channel, inverted, multiplied back by the *same*
    // number. The same one and not the inverted one: this is "divide the image
    // before processing and re-multiply it afterwards", and the divisor is a
    // property of the pixel that came in.
    float4 work = rgba;
    const float by = params.premult != 0u
                         ? channelOf(rgba, params.premultChannel) : 1.0f;
    const bool dividing = params.premult != 0u && by > 1.1920929e-7f;
    if (dividing) {
        work /= by;
    }

    // All four inverted, always. Which of them survive is the host's question,
    // and it puts the others back after this kernel has run -- so a kernel
    // that branched on the channel switches would be answering it twice.
    float4 inverted = 1.0f - work;
    if (dividing) {
        inverted *= by;
    }

    const float amount = amountAt(tid.xy);
    dst[tid.y * params.dstStride + tid.x] = lerp(rgba, inverted, amount);
}
