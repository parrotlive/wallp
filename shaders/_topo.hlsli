// Shared by the topo* wallpapers: a slowly morphing terrain and its contour lines.

#ifndef WALLP_TOPO_HLSLI
#define WALLP_TOPO_HLSLI

#include "_noise.hlsli"

// Terrain height at p (in screen heights), roughly -1..1. `t` moves through the noise's third
// dimension, so hills rise, merge and sink instead of just sliding across the screen.
float TopoHeight(float2 p, float t) {
    float2 warp = GNoise2(float3(p * 0.55, t * 0.6 + 3.1));
    float3 q = float3(p + 0.9 * warp, t);
    return GNoise(q) + 0.42 * GNoise(q * 2.07 + 5.2) + 0.18 * GNoise(q * 4.31 + 1.7);
}

struct Contour {
    float level;  // index of the nearest contour line
    float dist;   // distance to that line, in pixels
};

// Contour lines `1 / levels` apart in height. The distance comes from the screen-space
// gradient, so lines keep their width however steep the terrain gets.
Contour ContourAt(float h, float levels) {
    float v = h * levels;
    float grad = length(float2(ddx(v), ddy(v)));
    Contour c;
    c.level = round(v);
    c.dist = abs(v - c.level) / max(grad, 1e-5);
    return c;
}

// Coverage of an antialiased line `width` pixels wide.
float LineMask(float dist, float width) {
    return saturate(0.5 * width + 0.5 - dist);
}

bool IsMajor(float level, float every) {
    return abs(level - every * round(level / every)) < 0.5;
}

#endif
