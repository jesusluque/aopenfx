// Copyright (c) 2026 aopenfx contributors.
//
// Which channels a node is applied to.
//
// Every node in a compositor has this, and in every one of them it means the
// same thing: the operation changes the channels you chose and passes the rest
// through untouched. Blur the colour and keep the matte crisp; grade the red
// alone; key on alpha without disturbing anything else.
//
// THE HOST OWNS IT, NOT THE PLUGIN
//
// An effect fills all four channels and is not told which ones mattered. That
// is the whole point: a kernel that had to branch on which channels it was
// allowed to write is a kernel every author gets wrong in a different way, and
// gets wrong silently -- an effect that ignored the control would look like an
// effect that worked.
//
// So the host adds the control to every effect's description, and puts the
// unselected channels back afterwards with a kernel of its own. A plugin
// cannot fail to support it and cannot support it incorrectly, because it
// never sees it.
#pragma once

#include <cstdint>

#include "aofx/Descriptor.h"

namespace aofx::host {

/// One switch a channel, rather than a list of the combinations somebody
/// thought of.
///
/// It started as a choice -- RGBA, RGB, alpha, and the three singles -- and
/// that is a list that is always missing the one you want: red and blue,
/// green and alpha. Four switches say all sixteen without enumerating any of
/// them, and it is how every compositor asks the question.
///
/// Prefixed so an effect cannot collide with one by naming a parameter, and
/// labelled with a single letter, which is what makes a properties panel lay
/// the four of them across one row instead of down four.
inline constexpr const char* kChannelRedParam = "aofx.channel.r";
inline constexpr const char* kChannelGreenParam = "aofx.channel.g";
inline constexpr const char* kChannelBlueParam = "aofx.channel.b";
inline constexpr const char* kChannelAlphaParam = "aofx.channel.a";

/// The host's kernel that puts the unselected channels back. Registered with
/// the device when the registry scans, under a name no plugin can take: the
/// build's own table is consulted first, and this is not in it, so a plugin
/// naming a kernel this would shadow it -- hence a name nobody would choose by
/// accident.
inline constexpr const char* kChannelsKernel = "aofx.host.channels";
inline constexpr const char* kChannelsEntry = "channelsMain";

/// One bit a channel, red in bit 0. A set bit is a channel the effect changes.
enum : uint32_t {
    kChannelRed = 1u,
    kChannelGreen = 2u,
    kChannelBlue = 4u,
    kChannelAlpha = 8u,
    kChannelsAll = kChannelRed | kChannelGreen | kChannelBlue | kChannelAlpha,
};

/// The mask from four switches.
///
/// All four off is every channel, not none. A node applied to nothing is a node
/// that does nothing, which is indistinguishable from a broken one -- and it is
/// what a document written before these existed would otherwise mean.
[[nodiscard]] uint32_t channelMaskFrom(bool red, bool green, bool blue,
                                       bool alpha) noexcept;

/// Adds the four switches to a description, at the front.
///
/// First because it is the same control on every node -- it is not part of what
/// makes this effect this effect, and burying it under the parameters that do
/// puts the node's own controls further from the top on every node in the
/// graph.
void describeChannels(aofx::EffectDesc& into);

/// The uniform block of `kChannelsKernel`, matching `ChannelParams` in
/// channels.slang field for field. Four-byte members in declaration order, so
/// no padding rule has to agree across two compilers. Dispatch it over
/// `{input, output}` with a grid of the output's size.
struct ChannelUniforms {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dstStride = 0;
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
    uint32_t srcStride = 0;
    int32_t  srcOffsetX = 0;
    int32_t  srcOffsetY = 0;
    uint32_t keepMask = kChannelsAll;
    uint32_t pad0 = 0;
};
static_assert(sizeof(ChannelUniforms) == 40, "no padding, on any compiler");

}   // namespace aofx::host
