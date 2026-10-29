// Clock: an analog clock driven by iDate (local time). clock.json caps it at 30 fps.

float SdSegment(float2 p, float2 a, float2 b) {
    float2 pa = p - a, ba = b - a;
    float h = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h);
}

// Direction at `turns` (0 = twelve o'clock, clockwise; y points up).
float2 Dir(float turns) {
    float a = turns * 6.2831853;
    return float2(sin(a), cos(a));
}

// Coverage of a stroke with half-width `w` at distance `d`, with an edge `soft` wide.
float Stroke(float d, float w, float soft) { return saturate((w - d) / soft + 0.5); }

void mainImage(out float4 col, float2 fragCoord) {
    float px = 1.0 / iResolution.y;
    float2 p = (fragCoord - 0.5 * iResolution.xy) * px;  // origin at the center, 1 = screen height
    float r = length(p);
    const float R = 0.32;

    float3 c = lerp(float3(0.060, 0.068, 0.090), float3(0.018, 0.020, 0.028), saturate(r * 1.3));
    c = lerp(c, float3(0.075, 0.084, 0.108), 0.9 * Stroke(r, R, px));      // face
    c = lerp(c, float3(0.20, 0.22, 0.27), Stroke(abs(r - R), 0.0025, px));  // rim

    // Ticks: only the nearest of the 60 is evaluated.
    float tick = round(atan2(p.x, p.y) / 6.2831853 * 60.0);
    bool hourTick = fmod(abs(tick), 5.0) < 0.5;
    float2 td = Dir(tick / 60.0);
    float dt = SdSegment(p, td * (hourTick ? R * 0.82 : R * 0.90), td * (R * 0.95));
    c = lerp(c, hourTick ? float3(0.85, 0.87, 0.92) : float3(0.42, 0.45, 0.52),
             Stroke(dt, hourTick ? 0.0042 : 0.0016, px));

    float secs = iDate.w;
    float2 hd = Dir(fmod(secs / 3600.0, 12.0) / 12.0);
    float2 md = Dir(fmod(secs / 60.0, 60.0) / 60.0);
    float2 sd = Dir(fmod(secs, 60.0) / 60.0);
    float2 hA = -hd * 0.03, hB = hd * (R * 0.52);
    float2 mA = -md * 0.04, mB = md * (R * 0.78);

    // Soft shadows under the hour and minute hands (light from the top left).
    float2 so = p - float2(0.004, -0.006);
    float shadow = max(Stroke(SdSegment(so, hA, hB), 0.008, px * 8.0), Stroke(SdSegment(so, mA, mB), 0.006, px * 8.0));
    c *= 1.0 - 0.35 * shadow;

    const float3 hand = float3(0.92, 0.93, 0.96), accent = float3(1.0, 0.42, 0.30);
    c = lerp(c, hand, Stroke(SdSegment(p, hA, hB), 0.0075, px));
    c = lerp(c, hand, Stroke(SdSegment(p, mA, mB), 0.0050, px));
    c = lerp(c, accent, Stroke(SdSegment(p, -sd * 0.06, sd * (R * 0.86)), 0.0018, px));
    c = lerp(c, accent, Stroke(r, 0.011, px));                  // center cap
    c = lerp(c, float3(0.10, 0.10, 0.12), Stroke(r, 0.004, px));  // pin
    col = float4(c, 1.0);
}
