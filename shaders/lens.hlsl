// Lens: a black hole follows the cursor, wrapped in a swirling violet accretion disc, and
// bends the starlight and nebula behind it into an Einstein ring. Clicking makes it pulse;
// holding the button feeds it and it grows. When the cursor is on another monitor it waits
// at the nearest edge.

#include "_noise.hlsli"
#include "_mouse.hlsli"

static const float kPi = 3.14159265;

float Stars(float2 q, float cells, float pixel, int seed) {
    float2 g = q * cells;
    int2 cell = int2(floor(g)) + int2(seed, 5 * seed);
    if (Hash(cell) > 0.10) return 0.0;
    float2 at = float2(Hash(cell + 101), Hash(cell + 211)) * 0.8 + 0.1;
    float d = length(frac(g) - at) / cells;
    float size = max(0.0008, 0.8 * pixel);
    float m = Hash(cell + 503);
    float twinkle = 0.7 + 0.3 * sin(iTime * (0.5 + 2.0 * Hash(cell + 307)) + 6.0 * m);
    return exp(-d * d / (size * size)) * (0.15 + 0.85 * m * m) * twinkle;
}

// The unlensed sky at q (screen heights): drifting teal and violet nebula, two star layers.
float3 Sky(float2 q, float pixel, float t, int seed) {
    float2 drift = float2(t * 0.004, t * 0.0015);
    float n = Fbm(q * 2.2 + drift + float2(seed * 3.1, 0.0));
    float m = Fbm(q * 3.1 - drift * 1.3 + 9.0);
    float3 c = float3(0.014, 0.010, 0.030);
    c += float3(0.06, 0.13, 0.19) * smoothstep(0.30, 0.85, n);
    c += float3(0.24, 0.07, 0.30) * smoothstep(0.35, 0.90, m);
    c += float3(0.95, 0.95, 1.00) * Stars(q + drift, 70.0, pixel, seed);
    c += float3(0.80, 0.85, 1.00) * 0.6 * Stars(q + drift * 0.5 + 0.31, 140.0, pixel, seed + 3);
    return c;
}

void mainImage(out float4 col, float2 fragCoord) {
    float H = iResolution.y;
    float pixel = 1.0 / H;
    float t = iTime;
    int seed = int(iMonitor) * 17;
    float2 p = fragCoord * pixel;

    // The hole trails the cursor a little and waits at the edge when the cursor is elsewhere.
    float2 hole = clamp(iMouse.xy - iMouseVel * 0.10, 0.0, iResolution.xy) * pixel;

    // Size: a pulse after each click, growth while the button is held.
    float pulse = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        float age = iClick[i].z;
        pulse += exp(-age * 3.0) * (0.5 + 0.5 * cos(age * 14.0));
    }
    float feed = iClick[0].w > 0.5 ? saturate(iClick[0].z * 0.5) : 0.0;
    float einstein = 0.12 * (1.0 + 0.35 * pulse + 0.9 * feed);

    // Point-mass lens: a pixel at offset d shows the sky at d * (1 - E^2 / |d|^2), with a
    // slight swirl as if the hole were spinning.
    float2 d = p - hole;
    float r2 = max(dot(d, d), 1e-6);
    float r = sqrt(r2);
    float2 src = d * (1.0 - einstein * einstein / r2);
    float swirl = 1.4 * einstein * einstein / r2 + t * 0.02;
    src = float2(cos(swirl) * src.x - sin(swirl) * src.y, sin(swirl) * src.x + cos(swirl) * src.y);
    float3 c = Sky(hole + src, pixel, t, seed);

    // Photon ring at the Einstein radius, brighter on the side turning toward us (a hot spot
    // that orbits), and a glow around it; the shadow inside.
    float angle = atan2(d.y, d.x);
    float beam = 0.35 + 0.65 * pow(0.5 + 0.5 * cos(angle - t * 0.6), 3.0);
    beam *= 0.8 + 0.4 * Noise(float2(angle * 4.0 + t * 0.9, t * 0.3));
    float ring = exp(-abs(r - einstein) / (0.0022 + 0.03 * einstein * pulse));
    c += float3(1.00, 0.70, 0.85) * ring * (0.35 + 0.9 * beam);
    c += float3(0.55, 0.28, 0.65) * exp(-max(r - einstein, 0.0) / 0.05) * (0.12 + 0.25 * beam);
    float shadow = 0.72 * einstein;
    c *= saturate((r - shadow) / pixel * 0.7);

    // Accretion disc, tilted toward us: spiral arms of hot gas that turn and drift inward.
    // Its near half crosses in front of the shadow, the far half is hidden behind it.
    float2 e = float2(d.x, d.y / 0.30);
    float er = length(e);
    float inner = 1.05 * einstein, outer = 3.2 * einstein;
    float band = smoothstep(inner, inner * 1.25, er) * (1.0 - smoothstep(outer * 0.6, outer, er));
    if (band > 0.0) {
        float ea = atan2(e.y, e.x);
        // Spiral coordinates: following an arm, the angle shifts with log radius. Time turns
        // the arms and moves the gas inward without winding the pattern up.
        // Two samples with their angle seams on opposite sides, blended so neither shows.
        float lr = log(er / einstein);
        float spin = 1.6 * lr - t * 0.35, inflow = lr * 4.0 + t * 0.5;
        float ea2 = ea > 0.0 ? ea - kPi : ea + kPi;
        float gasA = Fbm(float2((ea + spin) * 1.9, inflow));
        float gasB = Fbm(float2((ea2 + spin) * 1.9, inflow) + 31.0);
        float w = cos(0.5 * ea);
        float gas = lerp(gasB, gasA, w * w);
        float streaks = 0.30 + 1.5 * gas * gas;
        float heat = saturate(1.6 - er / einstein * 0.55);  // hotter toward the hole
        float3 hot = lerp(float3(0.45, 0.12, 0.55), float3(1.00, 0.62, 0.85), heat);
        // In front of the shadow there is no glow behind the gas, so lift it a little.
        float front = d.y < 0.0 ? (r < shadow ? 1.5 : 1.0) : saturate((r - shadow) / pixel * 0.7);
        float doppler = 0.7 + 0.3 * cos(ea);  // the side coming toward us is brighter
        c += hot * band * streaks * doppler * front * (0.45 + 0.75 * heat);
    }
    col = float4(1.0 - exp(-c * 1.3), 1.0);
}
