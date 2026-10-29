// Clouds: domain-warped fBm over a sunset sky. The heaviest sample, so clouds.json renders it
// at half resolution (soft content upscales well) and caps it at 30 fps (it drifts slowly).

#include "_noise.hlsli"

void mainImage(out float4 col, float2 fragCoord) {
    float2 uv = fragCoord / iResolution.xy;
    float2 p = fragCoord / iResolution.y;  // square units, 1 = screen height
    float aspect = iResolution.x / iResolution.y;
    float t = iTime * 0.02;

    // Warm horizon, violet middle, deep blue top, and a low sun.
    float3 sky = lerp(float3(0.98, 0.58, 0.36), float3(0.56, 0.36, 0.52), smoothstep(0.0, 0.5, uv.y));
    sky = lerp(sky, float3(0.10, 0.12, 0.30), smoothstep(0.35, 1.0, uv.y));
    float2 sun = (p - float2(0.72 * aspect, 0.16)) * float2(1.0, 1.8);
    sky += float3(1.0, 0.75, 0.45) * (0.6 * exp(-4.0 * length(sun)));

    // Flattened clouds drifting right, thinning out toward the top.
    float2 q = p * float2(1.6, 3.2) + float2(-2.0 * t, 0.0);
    float warp = Fbm(q * 0.5 + float2(0.0, t));
    float density = Fbm(q + warp * 1.5);
    float cover = smoothstep(0.40, 0.62, density) * smoothstep(1.05, 0.35, uv.y);
    float3 rim = float3(1.00, 0.72, 0.58), core = float3(0.34, 0.24, 0.36);  // backlit edges, shaded centers
    float3 c = lerp(sky, lerp(rim, core, smoothstep(0.48, 0.72, density)), cover);

    float dither = frac(52.9829189 * frac(dot(fragCoord, float2(0.06711056, 0.00583715)))) - 0.5;
    col = float4(c + dither / 255.0, 1.0);
}
