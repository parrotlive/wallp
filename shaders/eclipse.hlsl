// Eclipse: a black disc in a violet nebula, its rim burning pink. A flame-like plume curls
// out of the upper-left rim and a fainter tail out of the lower right; their material flows
// slowly inward, the rim flickers, a lens flare pulses and the dust drifts.

#include "_noise.hlsli"

static const float kPi = 3.14159265;

float WrapAngle(float a) { return a - 2.0 * kPi * round(a / (2.0 * kPi)); }

// Sparse stars on a grid, one at most per cell, at least about a pixel wide.
float Stars(float2 p, float cells, float pixel, int seed) {
    float2 g = p * cells;
    int2 cell = int2(floor(g)) + int2(seed, 7 * seed);
    if (Hash(cell) > 0.06) return 0.0;
    float2 at = float2(Hash(cell + 101), Hash(cell + 211)) * 0.8 + 0.1;
    float d = length(frac(g) - at) / cells;
    float size = max(0.0009, 0.8 * pixel);
    float twinkle = 0.65 + 0.35 * sin(iTime * (0.6 + 2.0 * Hash(cell + 307)) + 6.0 * Hash(cell + 401));
    float m = Hash(cell + 503);
    return exp(-d * d / (size * size)) * twinkle * (0.15 + 0.85 * m * m);
}

// One spiral arm leaving the rim at angle `start`, curling as it goes out. Returns density.
float Arm(float r, float theta, float R, float start, float t, float seed) {
    float s = log(max(r, 1e-4) / R);  // distance along the arm (log radius)
    float d = WrapAngle(theta - (start - 1.1 * s));
    d += 0.30 * (Noise(float2(s * 2.5 - t * 0.08, seed)) - 0.5);  // the arm wavers
    float across = d * r;
    float width = 0.030 + 0.20 * max(r - R, 0.0);
    float envelope = exp(-across * across / (width * width)) * exp(-max(r - R, 0.0) * 2.6);
    if (envelope < 0.004) return 0.0;  // most of the screen: skip the noise

    // Smoke streaming along the arm and inward (toward smaller s), with bright filaments.
    float2 q = float2(s * 3.0 + t * 0.05, across / width * 1.6 + seed);
    float smoke = Fbm(q * float2(1.0, 1.4) + float2(0.0, 0.3 * sin(t * 0.07)));
    float ridge = 1.0 - abs(2.0 * Fbm(q * float2(1.6, 3.2) + 23.0 - float2(t * 0.03, 0.0)) - 1.0);
    float filaments = ridge * ridge * ridge * ridge;
    return envelope * (0.25 + 1.6 * smoke * smoke + 1.4 * filaments * envelope);
}

void mainImage(out float4 col, float2 fragCoord) {
    float H = iResolution.y;
    float pixel = 1.0 / H;
    float2 p = (fragCoord - 0.5 * iResolution.xy) * pixel;
    if (fmod(iMonitor, 2.0) > 0.5) p.x = -p.x;  // odd monitors show it mirrored
    float t = iTime;
    float seed = iMonitor * 13.0;

    float R = 0.17;
    float r = length(p);
    float theta = atan2(p.y, p.x);
    float start = 2.36 + 0.05 * sin(t * 0.03);  // upper left, swaying slightly

    // Dust: violet clouds turning very slowly around the disc.
    float spin = t * 0.004;
    float2 dp = float2(cos(spin) * p.x - sin(spin) * p.y, sin(spin) * p.x + cos(spin) * p.y);
    float dust = Fbm(dp * 3.2 + float2(seed, 3.0));
    float clouds = smoothstep(0.30, 0.80, dust);
    float3 c = float3(0.028, 0.016, 0.040);
    c += float3(0.16, 0.07, 0.20) * clouds * (0.5 + 0.9 * exp(-r * 1.8));
    c += 0.75 * Stars(p, 90.0, pixel, int(seed)) + 0.5 * Stars(p + 0.37, 160.0, pixel, int(seed) + 3);

    // The plume and the tail.
    float upper = Arm(r, theta, R, start, t, seed + 1.0);
    float lower = Arm(r, theta, R, start + kPi, t, seed + 9.0) * 0.32;
    float gas = upper + lower;
    float heat = saturate(gas * 1.2) * exp(-max(r - R, 0.0) * 3.0);  // hottest near the rim
    float3 gasColour = lerp(float3(0.45, 0.13, 0.45), float3(1.00, 0.36, 0.52), saturate(gas * 1.3));
    gasColour = lerp(gasColour, float3(1.00, 0.80, 0.74), heat * heat);
    c += gasColour * gas * (1.4 + 1.6 * exp(-max(r - R, 0.0) * 9.0));

    // Rim: thin and bright where the arms leave it, faint elsewhere, flickering.
    float side = 0.18 + 0.82 * exp(-pow(WrapAngle(theta - start) / 0.85, 2.0))
                      + 0.45 * exp(-pow(WrapAngle(theta - start - kPi) / 0.7, 2.0));
    float flicker = 0.75 + 0.5 * Noise(float2(theta * 5.0 + seed, t * 0.6));
    float outside = max(r - R, 0.0);
    float rim = exp(-outside / 0.003) + 0.45 * exp(-outside / 0.022) + 0.2 * exp(-outside / 0.08);
    c += float3(1.00, 0.52, 0.62) * rim * side * flicker;

    // Lens flare where the plume leaves the rim: a point and a horizontal streak.
    float2 f = p - R * 1.02 * float2(cos(start + 0.08), sin(start + 0.08));
    float pulse = 0.8 + 0.2 * sin(t * 1.1);
    float flare = 0.00002 / (dot(f, f) + 0.00002) + 0.6 * exp(-abs(f.y) / 0.0018 - abs(f.x) / 0.07);
    c += float3(1.00, 0.85, 0.85) * flare * pulse;

    // The disc itself hides everything behind it, with an antialiased edge.
    float disc = saturate((R - r) / pixel + 0.5);
    c = lerp(c, float3(0.004, 0.002, 0.006), disc);
    col = float4(1.0 - exp(-c * 1.2), 1.0);
}
