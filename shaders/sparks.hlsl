// Sparks: the cursor drags a glowing comet tail that cools from white through amber to
// magenta and sheds embers; every click bursts into a firework that falls and fades. With
// the cursor still, only faint embers drift up the dark sky.

#include "_noise.hlsli"
#include "_mouse.hlsli"

static const float kTau = 6.2831853;

float3 Heat(float x) {  // 1 = white hot, 0 = cold magenta
    float3 c = lerp(float3(0.55, 0.10, 0.45), float3(1.00, 0.45, 0.12), saturate(x * 1.6));
    return lerp(c, float3(1.00, 0.92, 0.80), saturate(x * 2.0 - 1.0));
}

void mainImage(out float4 col, float2 fragCoord) {
    float H = iResolution.y;
    float2 p = fragCoord;
    float t = iTime;
    float2 uv = fragCoord / iResolution.xy;
    float3 c = lerp(float3(0.018, 0.012, 0.030), float3(0.004, 0.004, 0.012), uv.y);

    // Comet tail: the polyline through this frame's cursor and the trail, thinning and
    // cooling with age. Pixels well outside the trail's bounding box skip it.
    float2 lo = iMouse.xy, hi = iMouse.xy;
    [unroll] for (int b = 0; b < 16; ++b) {
        lo = min(lo, iTrail[b].xy);
        hi = max(hi, iTrail[b].xy);
    }
    float offTrail = length(max(max(lo - p, p - hi), 0.0));
    float3 tail = 0.0;
    if (offTrail < 0.06 * H) [unroll] for (int i = 0; i < 16; ++i) {
        float3 a = TrailPoint(i), b = TrailPoint(i + 1);
        float along;
        float d = SegmentDistance(p, a.xy, b.xy, along);
        float age = lerp(a.z, b.z, along);
        float life = saturate(1.0 - age / 0.5);
        float width = (0.0025 + 0.008 * life) * H;
        float glow = exp(-d / width) * life * life;
        tail += Heat(life) * glow;
    }
    c += tail * 1.1;

    // Embers shed along the trail: each trail point owns a few, drifting up and flickering.
    if (offTrail < 0.10 * H) [unroll] for (int j = 1; j <= 16; j += 2) {
        float3 tp = TrailPoint(j);
        float fast = saturate(TrailSpeed(j - 1) / (0.6 * H));
        if (fast < 0.02) continue;
        [unroll] for (int k = 0; k < 3; ++k) {
            int2 id = int2(int(tp.x) * 3 + k, int(tp.y) + j * 17);
            float2 r = float2(Hash(id), Hash(id + 31)) - 0.5;
            float2 at = tp.xy + r * 0.05 * H + float2(r.x * 0.06, 0.08 + 0.05 * r.y) * H * tp.z;
            float2 v = p - at;
            float life = saturate(1.0 - tp.z / 0.55) * fast;
            float flick = 0.6 + 0.4 * sin(t * 30.0 + Hash(id + 77) * kTau);
            c += Heat(life * 0.8) * exp(-dot(v, v) / (0.000006 * H * H)) * life * flick * 1.6;
        }
    }

    // Fireworks: 40 sparks per click, thrown out, pulled down, cooling as they fall. In a
    // frame that falls with them the sparks sit on rays from the origin, spark s near angle
    // s / 40 of a turn, so a pixel only needs the three sparks nearest its own angle.
    const float kSparks = 40.0;
    [unroll] for (int n = 0; n < 4; ++n) {
        float age = iClick[n].z;
        if (age > 2.2) continue;
        float2 origin = iClick[n].xy;
        float fade = saturate(1.0 - age / 2.2);
        // Each burst gets its own colour; young sparks burn white.
        int2 burst = int2(origin);
        float3 hues[4] = {float3(1.0, 0.55, 0.15), float3(0.35, 0.75, 1.0), float3(1.0, 0.30, 0.75), float3(0.55, 1.0, 0.45)};
        float3 hue = hues[int(Hash(burst) * 3.99)];
        float3 sparkColour = lerp(hue, float3(1.0, 0.95, 0.85), exp(-age * 3.0));
        float drag = (1.0 - exp(-age * 2.2)) / 2.2;  // they slow down in the air
        float2 fall = float2(0.0, 0.10 * H) * age * age;
        float2 q = p - origin + fall;
        if (length(q) > 0.55 * H * drag + 0.04 * H) continue;  // beyond the fastest spark
        float nearest = floor(atan2(q.y, q.x) / kTau * kSparks);
        [unroll] for (int o = -1; o <= 1; ++o) {
            float s = nearest + o;
            s -= kSparks * floor(s / kSparks);  // 0..39
            int2 id = int2(int(s), int(origin.x) * 7 + int(origin.y));
            float ang = (s + 0.5 + 0.8 * (Hash(id) - 0.5)) / kSparks * kTau;
            float speed = (0.25 + 0.30 * Hash(id + 13)) * H;
            float2 dir = float2(cos(ang), sin(ang));
            float2 at = origin + dir * speed * drag - fall;
            float2 vel = dir * speed * exp(-age * 2.2) - float2(0.0, 0.20 * H) * age;
            // A short streak along the motion, so fast sparks read as lines.
            float along;
            float d = SegmentDistance(p, at, at - vel * 0.06, along);
            float twinkle = 0.7 + 0.3 * sin(t * 25.0 + s * 1.7);
            float spark = exp(-d / (0.0016 * H)) * 1.6 + exp(-d / (0.006 * H)) * 0.25;
            c += sparkColour * spark * fade * fade * twinkle * (1.0 - 0.6 * along);
        }
        float2 v = p - origin;  // the flash
        c += float3(1.0, 0.8, 0.6) * exp(-dot(v, v) / (0.0025 * H * H)) * exp(-age * 9.0);
    }

    // Faint embers rising all the time, so the sky is never still.
    float2 q = float2(p.x / H * 9.0, p.y / H * 9.0 - t * 0.25);
    int2 cell = int2(floor(q)) + int2(int(iMonitor) * 97, 0);
    float2 f = frac(q) - (float2(Hash(cell), Hash(cell + 5)) * 0.6 + 0.2);
    f.x += 0.12 * sin(t * 0.8 + Hash(cell + 9) * kTau);
    float ember = exp(-dot(f, f) / 0.0009) * step(Hash(cell + 3), 0.18);
    c += float3(0.85, 0.35, 0.18) * ember * (0.25 + 0.2 * sin(t * 3.0 + Hash(cell + 11) * kTau));

    col = float4(1.0 - exp(-c * 1.3), 1.0);
}
