// Topo neon: glowing contour lines, blue on the left turning violet and magenta on the right,
// on a deep indigo ground. Brightness waves run uphill through the levels while the terrain
// morphs.

#include "_topo.hlsli"

void mainImage(out float4 col, float2 fragCoord) {
    float px = iResolution.y / 1080.0;
    float2 uv = fragCoord / iResolution.xy;
    float2 p = fragCoord / iResolution.y * 1.35 + float2(23.0 + iMonitor * 3.7, iMonitor * 1.3);
    float h = TopoHeight(p + float2(0.008, -0.006) * iTime, iTime * 0.028 + 40.0);

    // Major lines every fourth level with a wide glow; dim minor lines in between.
    Contour c = ContourAt(h, 15.0);
    bool major = IsMajor(c.level, 4.0);
    float pulse = 0.65 + 0.35 * sin(c.level * 0.9 - iTime * 0.7);
    float core = LineMask(c.dist, (major ? 2.0 : 1.1) * px);
    float glow = exp(-c.dist / ((major ? 7.0 : 3.0) * px));
    float energy = major ? (core + 0.55 * glow) * (0.65 + 0.35 * pulse) : (0.8 * core + 0.18 * glow) * pulse;

    // Hue: across the screen and, a little, with height, drifting slowly.
    float hue = saturate(uv.x * 0.85 + 0.12 * h + 0.10 * sin(iTime * 0.05 + iMonitor));
    float3 blue = float3(0.25, 0.32, 1.00), violet = float3(0.62, 0.22, 1.00), magenta = float3(0.95, 0.30, 0.95);
    float3 tint = hue < 0.5 ? lerp(blue, violet, hue * 2.0) : lerp(violet, magenta, hue * 2.0 - 1.0);

    float3 ground = lerp(float3(0.030, 0.025, 0.130), float3(0.100, 0.020, 0.170), uv.x);
    ground *= 0.8 + 0.25 * smoothstep(-0.6, 0.8, h);  // lowlands darker
    float3 c3 = ground + tint * energy;
    c3 += float3(0.9, 0.85, 1.0) * core * (major ? 0.35 : 0.0) * pulse;  // white-hot cores
    col = float4(c3, 1.0);
}
