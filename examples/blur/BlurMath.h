// Copyright (c) 2026 aopenfx contributors.
//
// The blur's arithmetic, apart from the effect.
//
// Everything a host asks a blur before any pixel exists -- how far the picture
// grows, how much input a region needs, whether there is anything to do -- is
// a function of the size knob and the render scale, and nothing else. Kept
// here, free of the effect class and the kernel header, so the same numbers the
// render uses can be checked on a CPU, and so the regions and the render
// cannot drift apart: they call the same functions.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "aofx/Types.h"

namespace aofx_examples::blur {

inline constexpr const char* kParamSize = "size";

/// Standard deviations covered before the kernel is cut off.
///
/// Three covers 99.7% of the curve, which is below one code value at eight bits
/// and is where every other implementation stops. Named because it is a
/// decision rather than a constant: it trades a wider kernel against a visible
/// step at the edge of the blur.
inline constexpr double kRadiusInSigmas = 3.0;

/// CImgBlur's, so the two are interchangeable: `sigma = size / 2.4`.
inline constexpr double kSigmaPerSize = 1.0 / 2.4;

/// The size knob, in canonical pixels, as a standard deviation.
[[nodiscard]] inline double sigmaFrom(double size) {
    return std::max(size, 0.0) * kSigmaPerSize;
}

/// How many pixels either side the kernel reaches, for a sigma in *render*
/// pixels.
///
/// The small allowance below the ceiling is for floating point, not for the
/// filter: 8 / 2.4 * 3 is ten, and a product that comes out a hair above ten
/// must not become a radius of eleven.
[[nodiscard]] inline int radiusFrom(double sigma) {
    if (sigma <= 0.0) {
        return 0;
    }
    return static_cast<int>(std::ceil(sigma * kRadiusInSigmas - 1e-9));
}

/// The two axes of a blur at one render scale, in render pixels.
struct Reach {
    double sigmaX = 0.0;
    double sigmaY = 0.0;
    int    radiusX = 0;
    int    radiusY = 0;

    [[nodiscard]] bool none() const { return radiusX == 0 && radiusY == 0; }
};

/// The size knob is canonical pixels a person typed; the render happens at a
/// scale. A blur of ten pixels at full size is a blur of five at half, or the
/// half-size picture looks twice as soft as the full one and a proxy stops
/// being a preview of anything.
[[nodiscard]] inline Reach reachAt(double sizeX, double sizeY, double scaleX,
                                   double scaleY) {
    Reach reach;
    reach.sigmaX = sigmaFrom(sizeX) * std::max(scaleX, 0.0);
    reach.sigmaY = sigmaFrom(sizeY) * std::max(scaleY, 0.0);
    reach.radiusX = radiusFrom(reach.sigmaX);
    reach.radiusY = radiusFrom(reach.sigmaY);
    return reach;
}

/// `size` from the parameter list a region question carries. A single number
/// means both axes, which is what a one-dimensional value is.
inline void sizeFrom(const std::vector<aofx::ParamValue>& params, double& sizeX,
                     double& sizeY) {
    sizeX = 0.0;
    sizeY = 0.0;
    for (const aofx::ParamValue& value : params) {
        if (value.name == kParamSize) {
            sizeX = value.number(0, 0.0);
            sizeY = value.number(1, sizeX);
        }
    }
}

/// `rect` grown by the reach on each side. Empty stays empty.
[[nodiscard]] inline aofx::Rect grown(aofx::Rect rect, const Reach& reach) {
    if (rect.isEmpty()) {
        return rect;
    }
    rect.x1 -= reach.radiusX;
    rect.x2 += reach.radiusX;
    rect.y1 -= reach.radiusY;
    rect.y2 += reach.radiusY;
    return rect;
}

}   // namespace aofx_examples::blur
