// Field: a grid of glowing dashes, like iron filings, combed by a slow current. They swirl
// around the cursor and light up near it, sweep along when it moves fast, and every click
// sends a shockwave that knocks them outward.

#include "_noise.hlsli"
#include "_mouse.hlsli"

void mainImage(out float4 col, float2 fragCoord) {
    float H = iResolution.y;
    float px = H / 1080.0;
    float t = iTime;
    float cell = 22.0 * px;
    float2 id = floor(fragCoord / cell);
    float2 centre = (id + 0.5) * cell;  // a whole dash shares one field value

    // The current: an angle field from slowly evolving gradient noise.
    float a = GNoise(float3(centre / H * 1.8 + iMonitor * 5.0, t * 0.05)) * 4.0;
    float2 flow = float2(cos(a), sin(a)) * 0.35;

    // The cursor: a vortex around it, plus a pull along its motion.
    float2 v = centre - iMouse.xy;
    float r = length(v) + 1e-3;
    float near = exp(-r / (0.22 * H));
    flow += float2(-v.y, v.x) / r * near * 1.4;
    float2 vel = iMouseVel / H;
    float speed = length(vel);
    flow += vel / max(speed, 1e-3) * saturate(speed * 0.8) * exp(-r / (0.12 * H)) * 2.0;

    // Shockwaves from clicks.
    float shock = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        float age = iClick[i].z;
        float2 w = centre - iClick[i].xy;
        float rw = length(w) + 1e-3;
        float band = exp(-pow((rw - age * 0.55 * H) / (0.05 * H), 2.0)) * exp(-age * 1.2);
        flow += w / rw * band * 3.0;
        shock += band;
    }

    // Draw the dash: length grows with field strength, brightness with closeness and shock.
    float strength = length(flow);
    float2 dir = flow / max(strength, 1e-4);
    float half_ = cell * (0.18 + 0.24 * saturate(strength));
    float along;
    float d = SegmentDistance(fragCoord, centre - dir * half_, centre + dir * half_, along);
    float ink = saturate(1.1 * px + 0.5 - d);

    float heat = saturate(near * 1.3 + shock);
    float3 cold = float3(0.12, 0.30, 0.40), warm = float3(0.55, 0.95, 1.00), hot = float3(0.95, 0.55, 1.00);
    float3 dash = lerp(cold, warm, saturate(near * 1.5));
    dash = lerp(dash, hot, saturate(shock * 1.5));
    dash *= 0.55 + 0.45 * heat + 0.15 * sin(t * 0.7 + a * 2.0);

    float2 uv = fragCoord / iResolution.xy;
    float3 ground = lerp(float3(0.012, 0.020, 0.030), float3(0.020, 0.028, 0.045), uv.y);
    ground += float3(0.02, 0.06, 0.08) * near * near;  // a faint halo where the cursor is
    col = float4(ground + dash * ink, 1.0);
}
