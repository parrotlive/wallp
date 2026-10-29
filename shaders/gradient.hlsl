// Gradient: four soft color blobs drifting on slow Lissajous paths.
// gradient.json caps it at 30 fps; it moves far too slowly to need more.

static const float3 kColors[4] = {
    float3(0.36, 0.19, 0.60),  // violet
    float3(0.07, 0.42, 0.60),  // teal
    float3(0.85, 0.38, 0.42),  // coral
    float3(0.08, 0.10, 0.26),  // navy
};
static const float4 kMotion[4] = {  // xy = angular speed, zw = phase
    float4(1.0, 1.3, 0.0, 1.0),
    float4(0.7, 0.9, 2.0, 0.3),
    float4(1.1, 0.6, 4.0, 2.2),
    float4(0.5, 1.2, 5.1, 3.7),
};

void mainImage(out float4 col, float2 fragCoord) {
    float aspect = iResolution.x / iResolution.y;
    float2 uv = fragCoord / iResolution.y;  // x: 0..aspect, y: 0..1
    float t = iTime * 0.08;

    float3 sum = 0.0;
    float weights = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        float4 m = kMotion[i];
        float2 center = (0.5 + 0.38 * float2(sin(t * m.x + m.z), cos(t * m.y + m.w))) * float2(aspect, 1.0);
        float2 d = uv - center;
        float w = 1.0 / (dot(d, d) + 0.02);
        w *= w;
        sum += kColors[i] * w;
        weights += w;
    }
    float3 c = sum / weights;

    float dither = frac(52.9829189 * frac(dot(fragCoord, float2(0.06711056, 0.00583715)))) - 0.5;
    col = float4(c + dither / 255.0, 1.0);
}
