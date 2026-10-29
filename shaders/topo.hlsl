// Topo: thin white contour lines on black, every fifth one bold, over terrain that slowly
// rises, merges and sinks.

#include "_topo.hlsli"

void mainImage(out float4 col, float2 fragCoord) {
    float px = iResolution.y / 1080.0;  // line widths follow the display size
    float2 p = fragCoord / iResolution.y * 1.7 + float2(iMonitor * 3.7, iMonitor * 1.3);
    float h = TopoHeight(p + float2(0.010, 0.004) * iTime, iTime * 0.03);
    Contour c = ContourAt(h, 13.0);

    bool major = IsMajor(c.level, 5.0);
    float ink = LineMask(c.dist, (major ? 2.6 : 1.25) * px) * (major ? 1.0 : 0.66);
    col = float4((0.010 + ink).xxx, 1.0);
}
