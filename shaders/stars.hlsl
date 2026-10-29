// Stars: three parallax layers of grid-hashed stars drifting slowly over a dark sky.

float Hash12(float2 p) {
    float3 p3 = frac(p.xyx * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float2 Hash22(float2 p) {
    float3 p3 = frac(p.xyx * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.xx + p3.yz) * p3.zy);
}

// One layer with at most one star per grid cell. `scale` is cells per screen height and
// `pixel` the size of a pixel in the same units as uv.
float3 StarLayer(float2 uv, float scale, float seed, float pixel) {
    float2 g = uv * scale;
    float2 cell = floor(g);
    if (Hash12(cell + seed) > 0.22) return 0.0;  // about one cell in five holds a star
    float2 h = Hash22(cell + seed * 1.37);
    float2 f = frac(g) - 0.5;
    float d = length(f - (h - 0.5) * 0.6);

    float size = lerp(0.02, 0.06, h.x * h.x);  // radius, in cells
    float energy = size * size;
    size = max(size, 0.7 * pixel * scale);     // never thinner than ~0.7 px (no shimmer)...
    energy /= size * size;                     // ...and no brighter for it

    float twinkle = 0.7 + 0.3 * sin(iTime * (0.8 + 2.5 * h.y) + h.x * 6.2831853);
    float3 tint = lerp(float3(0.75, 0.85, 1.0), float3(1.0, 0.88, 0.75), h.y);
    return tint * (exp(-2.0 * d * d / (size * size)) * energy * twinkle);
}

void mainImage(out float4 col, float2 fragCoord) {
    float2 uv = fragCoord / iResolution.y;
    float pixel = 1.0 / iResolution.y;

    // Deep blue at the bottom fading to near black, with a faint violet haze.
    float3 c = lerp(float3(0.020, 0.030, 0.070), float3(0.004, 0.006, 0.016), uv.y);
    float2 q = fragCoord / iResolution.xy - float2(0.7, 0.35);
    c += float3(0.05, 0.02, 0.07) * exp(-6.0 * dot(q, q));

    float seed = 11.0 + iMonitor * 101.0;  // a different sky on each monitor
    float2 drift = float2(1.0, 0.35) * iTime;
    c += StarLayer(uv + drift * 0.0020, 60.0, seed, pixel) * 0.55;
    c += StarLayer(uv + drift * 0.0045, 30.0, seed + 12.0, pixel) * 0.8;
    c += StarLayer(uv + drift * 0.0090, 14.0, seed + 26.0, pixel);
    col = float4(c, 1.0);
}
