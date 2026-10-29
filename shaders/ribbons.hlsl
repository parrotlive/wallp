// Ribbons: silky violet and magenta bands of fine fibres that undulate and twist through a
// dark sky. Each band is a twisting tube of fibres seen from the side: the front and back
// fibres cross into a woven mesh, and where the tube pinches they bunch into bright edges.
// Every fibre is found analytically per pixel (no loop over fibres).

static const float kTau = 6.2831853;

struct Band {
    float base, amp1, freq1, speed1, amp2, freq2, speed2;  // centre line
    float radius, pinchFreq, pinchSpeed;                   // tube radius and its pinching
    float twist, twistSpeed, fibres;
    float3 tint;
};

// One band at p (screen-height units, x across). Returns its additive colour.
float3 ShadeBand(Band b, float2 p, float t, float px, float near) {
    float x = p.x;
    float centre = b.base + near * (b.amp1 * sin(x * b.freq1 + t * b.speed1) + b.amp2 * sin(x * b.freq2 - t * b.speed2 + 1.7));
    float radius = near * b.radius * (0.12 + 0.88 * (0.5 + 0.5 * sin(x * b.pinchFreq + t * b.pinchSpeed)));
    float phase = x * b.twist + t * b.twistSpeed;

    // Where on the tube this pixel lies: v = cos(angle) across the silhouette.
    float v = (p.y - centre) / radius;
    float inside = saturate((1.0 - abs(v)) / max(fwidth(v), 1e-4));  // antialiased silhouette
    float a = acos(clamp(v, -1.0, 1.0));                           // 0..pi

    // Fibre coordinates of the front (angle +a) and back (angle -a) surfaces; fibres sit at
    // whole numbers. Computed for every pixel so the derivatives stay valid.
    float uf = (a - phase) * b.fibres / kTau;
    float ub = (-a - phase) * b.fibres / kTau;
    float gf = length(float2(ddx(uf), ddy(uf)));  // fibres per pixel
    float gb = length(float2(ddx(ub), ddy(ub)));
    float df = abs(frac(uf + 0.5) - 0.5) / max(gf, 1e-4);  // distance in pixels
    float db = abs(frac(ub + 0.5) - 0.5) / max(gb, 1e-4);
    float w = 0.9 * px;
    // Crisp fibres while they are apart; their average density once they crowd a pixel.
    float lf = lerp(saturate(w + 0.5 - df), saturate(w * gf), saturate(gf * 1.5 - 0.4));
    float lb = lerp(saturate(w + 0.5 - db), saturate(w * gb), saturate(gb * 1.5 - 0.4));

    float depth = sin(a);  // 1 on the axis, 0 at the silhouette
    float3 c = b.tint * (lf * (1.0 + 0.8 * depth) + 0.7 * lb) * inside;
    c += b.tint * 0.10 * inside;                                   // a faint veil inside the tube
    float edge = abs(abs(p.y - centre) - radius) / px;             // the silhouette glows
    c += b.tint * (0.9 * exp(-edge / 1.4) + 0.22 * exp(-edge / 16.0));
    c += b.tint * 0.07 * exp(-abs(p.y - centre) / (radius + 0.08));  // and so does the band
    return c;
}

void mainImage(out float4 col, float2 fragCoord) {
    float px = 1.0 / iResolution.y;
    float2 p = fragCoord * px;
    p.x += iMonitor * 1.7;  // each monitor sees a different stretch of the bands
    float t = iTime;

    // Night sky: black at the edges, deep blue high in the middle, violet haze low down.
    float2 uv = fragCoord / iResolution.xy;
    float3 c = float3(0.012, 0.010, 0.025);
    c += float3(0.020, 0.030, 0.110) * exp(-8.0 * dot(uv - float2(0.62, 0.78), uv - float2(0.62, 0.78)));
    c += float3(0.060, 0.012, 0.080) * exp(-5.0 * dot(uv - float2(0.35, 0.15), uv - float2(0.35, 0.15)));

    Band bands[4] = {
        {0.58, 0.20, 2.1, 0.23, 0.06, 4.3, 0.31,  0.090, 1.7, 0.19,  6.5, 0.50, 52.0, float3(0.55, 0.28, 1.00)},
        {0.46, 0.17, 2.7, -0.17, 0.07, 3.7, 0.27, 0.070, 2.3, -0.23, -5.5, 0.42, 44.0, float3(1.00, 0.22, 0.80)},
        {0.34, 0.18, 1.6, 0.19, 0.08, 5.1, -0.21, 0.120, 1.3, 0.15,  4.8, -0.36, 60.0, float3(0.62, 0.20, 1.00)},
        {0.72, 0.12, 3.1, -0.13, 0.05, 4.9, 0.17, 0.055, 2.9, 0.27,  -7.0, 0.55, 36.0, float3(0.40, 0.32, 1.00)},
    };
    float near = 0.65 + 0.6 * uv.x;  // perspective: the bands come closer toward the right
    float3 light = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) light += ShadeBand(bands[i], p, t, px, near);

    // Bands fade out toward the left edge, as if lit from the right.
    light *= 0.45 + 0.55 * smoothstep(-0.1, 0.8, uv.x);
    c += light;
    c = 1.0 - exp(-c * 1.3);  // soft shoulder: overlaps glow white-pink instead of clipping
    col = float4(c, 1.0);
}
