// Star trails: long-exposure arcs of white and pale pink light circling a dark void. Rings
// crowd toward the void as if its gravity bent the light, and inner rings turn faster, so
// the whole vortex slowly swirls.

static const float kTau = 6.2831853;

float Hash11(float n) {
    uint h = uint(int(n)) * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;  // PCG output permutation
    return float((h >> 22u) ^ h) * (1.0 / 4294967296.0);
}

void mainImage(out float4 col, float2 fragCoord) {
    float H = iResolution.y;
    float px = H / 1080.0;
    float side = fmod(iMonitor, 2.0) < 0.5 ? 0.80 : 0.20;  // alternate monitors mirror the vortex
    float2 centre = float2(iResolution.x * side, H * 0.44);

    // Slightly tilted disc: the rings are ellipses, as if seen a little from above.
    float2 q = fragCoord - centre;
    q = float2(0.966 * q.x + 0.259 * q.y, -0.259 * q.x + 0.966 * q.y);  // rotate 15 degrees
    q.y *= 1.22;
    float r = length(q);
    float voidR = 0.15 * H;

    // Ring coordinate with the light bent toward the void: rings at r - R^2 / r.
    float spacing = 3.3 * px;
    float u = max(r - voidR * voidR / max(r, 1.0), 0.0) / spacing;
    float du = max(length(float2(ddx(u), ddy(u))), 1e-4);  // rings per pixel
    float ring = floor(u);
    float seed = ring * 7.13 + iMonitor * 1013.0;
    float h1 = Hash11(seed), h2 = Hash11(seed + 1.0), h3 = Hash11(seed + 2.0);

    // Each ring has a line somewhere near its middle, 0.6..1.5 px wide.
    float width = lerp(0.6, 1.5, h2 * h2) * px;
    float across = abs(frac(u) - 0.5 - (h3 - 0.5) * 0.3) / du;  // pixels from the line
    float core = saturate(width + 0.5 - across) * min(width, 1.0) / max(width, 1.0) * 1.4;

    // Along the ring: inner rings turn faster (Kepler-like, capped).
    float rr = (ring + 0.5) * spacing / H;
    float k = 0.22 + rr;
    float omega = 0.045 / (k * sqrt(k));
    float a = atan2(q.y, q.x) / kTau + omega * iTime / kTau + h1;  // turns, wrapping at integers

    // Three slots per turn, each with at most one trail; the wrap of `a` falls on a slot
    // border, so no trail is cut by it.
    float slots = 3.0;
    float x = frac(a) * slots;
    float slot = floor(x);
    float s = seed + slot * 3.7;
    float on = step(0.15, Hash11(s + 5.0));
    float start = Hash11(s + 6.0) * 0.2;
    float len = lerp(0.25, 0.78, Hash11(s + 7.0));
    float along = frac(x) - start;  // in slot fractions
    float arcPx = r * kTau / slots;  // one slot fraction in pixels
    float ends = saturate(min(along, len - along) * arcPx / px + 0.5);
    float taper = 0.55 + 0.45 * saturate(along / len);  // brighter toward the leading end

    float mag = Hash11(s + 8.0);
    float bright = 0.30 + 0.85 * mag * mag;
    float3 tint = lerp(float3(0.92, 0.90, 1.00), float3(1.00, 0.80, 0.90), Hash11(s + 9.0));

    // Fade out where rings crowd below a pixel, inside the void and far away.
    float visible = saturate(2.0 - du * 2.0)
                  * smoothstep(voidR * 1.02, voidR * 1.45, r)
                  * (0.45 + 0.55 * exp(-r / (1.1 * H)));
    float3 c = tint * (core * ends * on * taper * bright * visible);

    // A near-black sky, a shade lighter around the vortex than in the void.
    c += float3(0.010, 0.009, 0.014) * (0.4 + 0.6 * smoothstep(voidR * 0.8, voidR * 2.0, r));
    col = float4(c, 1.0);
}
