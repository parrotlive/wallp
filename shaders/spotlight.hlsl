// Spotlight: a dot grid lit by the mouse cursor. spotlight.json sets "mouse": true, which makes
// wallp read the cursor each frame (iMouse.xy, in pixels, origin at the bottom left).

void mainImage(out float4 col, float2 fragCoord) {
    float2 res = iResolution.xy;
    float cell = max(14.0, res.y / 54.0);  // grid spacing in pixels
    float2 id = floor(fragCoord / cell);
    float2 f = fragCoord / cell - id - 0.5;
    float2 center = (id + 0.5) * cell;     // a whole dot shares one light value

    // The cursor light, plus a dim one that wanders so the screen is never static.
    float2 wander = res * (0.5 + float2(0.35 * sin(iTime * 0.21), 0.30 * sin(iTime * 0.17 + 1.0)));
    float dm = length(center - iMouse.xy) / res.y;
    float dw = length(center - wander) / res.y;
    float light = saturate(exp(-14.0 * dm * dm) + 0.35 * exp(-6.0 * dw * dw));
    float ripple = 0.5 + 0.5 * sin(dm * 45.0 - iTime * 2.5);

    float radius = lerp(0.07, 0.30, light);                          // in cells
    float coverage = saturate((radius - length(f)) * cell + 0.5);   // ~1 px antialiased edge
    float3 dotColor = lerp(float3(0.12, 0.14, 0.20), float3(0.35, 0.80, 1.00), light);
    dotColor *= 0.75 + 0.25 * ripple * light;
    float3 background = float3(0.035, 0.040, 0.060) + float3(0.04, 0.07, 0.12) * light;
    col = float4(lerp(background, dotColor, coverage), 1.0);
}
