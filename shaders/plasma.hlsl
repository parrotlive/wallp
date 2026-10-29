// Plasma: a few sine waves through a cosine palette. About as cheap as a shader gets.

float3 Palette(float t) {
    // a + b * cos(2pi * (t + d)), after Inigo Quilez
    // Kept fairly dark so desktop icon labels stay readable.
    return float3(0.30, 0.20, 0.42) + float3(0.28, 0.20, 0.30) * cos(6.2831853 * (t + float3(0.00, 0.10, 0.22)));
}

void mainImage(out float4 col, float2 fragCoord) {
    float2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float t = iTime * 0.2;
    float v = sin(uv.x * 2.7 + t * 1.3)
            + sin(uv.y * 3.1 - t)
            + sin((uv.x + uv.y) * 2.2 + t * 0.7)
            + sin(length(uv * 3.0 + float2(sin(t * 0.6), cos(t * 0.5))) * 3.0 - t * 1.5);
    float3 c = Palette(v * 0.16 + t * 0.05 + iMonitor * 0.25);
    c *= 1.0 - 0.4 * dot(uv, uv);  // soft vignette

    // Interleaved gradient noise: one 8-bit step of dither hides banding in smooth gradients.
    float dither = frac(52.9829189 * frac(dot(fragCoord, float2(0.06711056, 0.00583715)))) - 0.5;
    col = float4(c + dither / 255.0, 1.0);
}
