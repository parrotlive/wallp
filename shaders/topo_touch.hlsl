// Topo touch: the black contour map, but the cursor raises a hill that the lines flow
// around, and the contours near it glow violet. Clicking sends a wave through the terrain.

#include "_topo.hlsli"
#include "_mouse.hlsli"

void mainImage(out float4 col, float2 fragCoord) {
    float H = iResolution.y;
    float px = H / 1080.0;
    float2 p = fragCoord / H * 1.7 + float2(31.0 + iMonitor * 3.7, iMonitor * 1.3);
    float h = TopoHeight(p + float2(0.010, 0.004) * iTime, iTime * 0.03 + 60.0);

    // The hill follows the cursor a little behind it.
    float2 m = iMouse.xy - iMouseVel * 0.12;
    float2 v = (fragCoord - m) / H;
    float hill = exp(-dot(v, v) / 0.012);
    h += 0.55 * hill;

    // Waves from clicks: a raised ring spreading outward.
    [unroll] for (int i = 0; i < 4; ++i) {
        float age = iClick[i].z;
        float rw = length(fragCoord - iClick[i].xy) / H;
        float x = (rw - age * 0.45) / 0.06;
        h += 0.35 * exp(-x * x) * exp(-age * 0.9);
    }

    Contour c = ContourAt(h, 13.0);
    bool major = IsMajor(c.level, 5.0);
    float ink = LineMask(c.dist, (major ? 2.6 : 1.25) * px) * (major ? 1.0 : 0.62);
    float glow = exp(-c.dist / (4.0 * px)) * hill;  // lines near the cursor glow
    float3 lineColour = lerp(float3(1.0, 1.0, 1.0), float3(0.80, 0.55, 1.00), saturate(hill * 1.4));
    float3 c3 = 0.010 + lineColour * ink * (1.0 + 0.6 * hill) + float3(0.45, 0.20, 0.85) * glow * 0.45;
    col = float4(c3, 1.0);
}
