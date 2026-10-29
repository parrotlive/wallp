// Topo light: fine black contour lines on white paper, over slowly morphing terrain.

#include "_topo.hlsli"

void mainImage(out float4 col, float2 fragCoord) {
    float px = iResolution.y / 1080.0;
    float2 p = fragCoord / iResolution.y * 1.7 + float2(11.0 + iMonitor * 3.7, 4.0 - iMonitor * 1.3);
    float h = TopoHeight(p - float2(0.009, 0.005) * iTime, iTime * 0.03 + 20.0);
    Contour c = ContourAt(h, 12.0);

    float ink = LineMask(c.dist, 1.35 * px);
    float2 uv = fragCoord / iResolution.xy - 0.5;
    float paper = 0.955 - 0.05 * dot(uv, uv);  // a little vignette, like light falling on paper
    col = float4(lerp(paper.xxx, float3(0.06, 0.06, 0.07), ink), 1.0);
}
