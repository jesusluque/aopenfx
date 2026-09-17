// Copyright (c) 2026 aopenfx contributors.
//
// The blur example's regions and reach, on a CPU.
//
// The questions a host asks before any pixel exists -- how far the picture
// grows, how much input a window needs, whether there is anything to do -- are
// answered by examples/blur/BlurMath.h, and the effect answers them by calling
// exactly those functions. So they can be checked here without a GPU, a kernel
// or a host.
#include <cstdio>

#include "BlurMath.h"

namespace {

using namespace aofx_examples::blur;

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

bool same(const aofx::Rect& a, const aofx::Rect& b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

}   // namespace

int main() {
    const aofx::Rect window{20, 20, 40, 40};

    {
        const Reach reach = reachAt(8.0, 8.0, 1.0, 1.0);
        expect(reach.radiusX == 10 && reach.radiusY == 10,
               "size 8 at scale 1 reaches 10 pixels (sigma 8/2.4, three sigmas)");
        expect(same(grown(window, reach), aofx::Rect{10, 10, 50, 50}),
               "size 8 at scale 1: the window [20,20,40,40] needs [10,10,50,50]");
    }
    {
        const Reach reach = reachAt(8.0, 8.0, 0.5, 0.5);
        expect(reach.radiusX == 5 && reach.radiusY == 5,
               "size 8 at scale 0.5 reaches 5 pixels, not 10");
        expect(same(grown(window, reach), aofx::Rect{15, 15, 45, 45}),
               "size 8 at scale 0.5: the window grows by 5");
    }
    {
        const Reach reach = reachAt(0.0, 0.0, 1.0, 1.0);
        expect(reach.none(), "size 0 is an identity");
        expect(same(grown(window, reach), window), "size 0 grows nothing");
    }
    {
        const Reach reach = reachAt(8.0, 0.0, 1.0, 1.0);
        expect(reach.radiusX == 10 && reach.radiusY == 0 && !reach.none(),
               "size (8, 0) reaches along x only");
        expect(same(grown(window, reach), aofx::Rect{10, 20, 50, 40}),
               "size (8, 0) grows the window along x only");
    }
    {
        // A window that does not start at the origin -- the ordinary case for a
        // tile or a viewer's region -- and one partly left of and below zero.
        const Reach reach = reachAt(8.0, 8.0, 1.0, 1.0);
        expect(same(grown(aofx::Rect{-30, -15, 10, 5}, reach),
                    aofx::Rect{-40, -25, 20, 15}),
               "a window with a non-zero, negative origin grows around itself");
        expect(same(grown(aofx::Rect{1000, 500, 1920, 1080}, reach),
                    aofx::Rect{990, 490, 1930, 1090}),
               "a window far from the origin grows around itself");
    }
    {
        // What the parameter list carries: a two-dimensional value, and a
        // single number meaning both axes.
        double x = -1.0;
        double y = -1.0;
        aofx::ParamValue size;
        size.name = kParamSize;
        size.numbers = {8.0, 24.0};
        sizeFrom({size}, x, y);
        expect(x == 8.0 && y == 24.0, "sizeFrom reads both axes");
        size.numbers = {12.0};
        sizeFrom({size}, x, y);
        expect(x == 12.0 && y == 12.0, "a single size is both axes");
        sizeFrom({}, x, y);
        expect(x == 0.0 && y == 0.0, "no size is no blur");
    }
    {
        expect(same(grown(aofx::Rect{}, reachAt(8.0, 8.0, 1.0, 1.0)), aofx::Rect{}),
               "an empty region stays empty");
        expect(reachAt(24.0, 24.0, 1.0, 1.0).radiusX == 30 &&
                   reachAt(12.0, 12.0, 1.0, 1.0).radiusX == 15,
               "no floating-point pixel of extra reach at sizes 12 and 24");
    }

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
