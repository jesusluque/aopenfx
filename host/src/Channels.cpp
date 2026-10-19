// Copyright (c) 2026 aopenfx contributors.
#include "aofx_host/Channels.h"

#include <array>
#include <utility>

namespace aofx::host {

uint32_t channelMaskFrom(bool red, bool green, bool blue, bool alpha) noexcept {
    const uint32_t mask = (red ? kChannelRed : 0u) | (green ? kChannelGreen : 0u) |
                          (blue ? kChannelBlue : 0u) | (alpha ? kChannelAlpha : 0u);
    // None selected means all. A node applied to nothing is a node that does
    // nothing, which reads as broken -- and it is what a document saved before
    // these switches existed says, since a missing boolean is false.
    return mask == 0u ? kChannelsAll : mask;
}

void describeChannels(aofx::EffectDesc& into) {
    const std::array<std::pair<const char*, const char*>, 4> kSwitches{{
        {kChannelRedParam, "R"},
        {kChannelGreenParam, "G"},
        {kChannelBlueParam, "B"},
        {kChannelAlphaParam, "A"},
    }};
    std::vector<aofx::ParamDesc> channels;
    for (const auto& [name, label] : kSwitches) {
        aofx::ParamDesc param;
        param.name = name;
        param.label = label;
        param.hint = "Which channels this node changes. The rest come through from the "
                     "input untouched -- blur the colour and the matte stays crisp.";
        param.type = aofx::ParamType::Boolean;
        param.defaults = {1.0};
        channels.push_back(std::move(param));
    }
    // At the front. The same control appears on every node, so it is not part
    // of what makes this effect this effect; putting it after the parameters
    // that are pushes them all down by a row on every node in the graph.
    into.params.insert(into.params.begin(), channels.begin(), channels.end());
}

}   // namespace aofx::host
