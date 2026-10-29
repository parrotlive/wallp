// Value noise and fBm, shared by shaders that #include "_noise.hlsli".
// Files starting with '_' (and *.hlsli files) are not listed as wallpapers.

#ifndef WALLP_NOISE_HLSLI
#define WALLP_NOISE_HLSLI

// Integer hash: stays exact for large coordinates, unlike frac(sin(x) * 43758.5453).
float Hash(int2 p) {
    uint n = (uint(p.x) * 1597334677u) ^ (uint(p.y) * 3812015801u);
    n = (n ^ (n >> 16)) * 0x45d9f3bu;
    n ^= n >> 16;
    return float(n) * (1.0 / 4294967296.0);
}

float Noise(float2 p) {
    float2 i = floor(p);
    float2 f = p - i;
    float2 u = f * f * (3.0 - 2.0 * f);
    int2 c = int2(i);
    return lerp(lerp(Hash(c), Hash(c + int2(1, 0)), u.x), lerp(Hash(c + int2(0, 1)), Hash(c + int2(1, 1)), u.x), u.y);
}

// Six octaves; each one rotated and scaled by 2.
float Fbm(float2 p) {
    const float2x2 octave = float2x2(1.6, 1.2, -1.2, 1.6);
    float v = 0.0, a = 0.5;
    [unroll] for (int i = 0; i < 6; ++i) {
        v += a * Noise(p);
        p = mul(octave, p);
        a *= 0.5;
    }
    return v;
}

// lowbias32 (Chris Wellons) on a mix of the three coordinates: one well-mixed 32-bit hash.
uint Hash3(int3 c) {
    uint n = (uint(c.x) * 0x8da6b343u) ^ (uint(c.y) * 0xd8163841u) ^ (uint(c.z) * 0xcb1ab31fu);
    n ^= n >> 16; n *= 0x7feb352du;
    n ^= n >> 15; n *= 0x846ca68bu;
    n ^= n >> 16;
    return n;
}

float3 GradA(uint h) { return float3(uint3(h, h >> 5, h >> 10) & 31u) * (2.0 / 31.0) - 1.0; }
float3 GradB(uint h) { return float3(uint3(h >> 15, h >> 20, h >> 25) & 31u) * (2.0 / 31.0) - 1.0; }

float Trilerp(float n[8], float3 u) {
    return lerp(lerp(lerp(n[0], n[1], u.x), lerp(n[2], n[3], u.x), u.y),
                lerp(lerp(n[4], n[5], u.x), lerp(n[6], n[7], u.x), u.y), u.z);
}

// Gradient (Perlin-style) noise in 3D, roughly -1..1. Its level sets are smooth curves, so it
// suits contour lines; animate by moving through z.
float GNoise(float3 p) {
    float3 i = floor(p);
    float3 f = p - i;
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    int3 c = int3(i);
    float n[8];
    [unroll] for (int k = 0; k < 8; ++k) {
        int3 o = int3(k & 1, (k >> 1) & 1, k >> 2);
        n[k] = dot(GradA(Hash3(c + o)), f - float3(o));
    }
    return Trilerp(n, u);
}

// Two independent gradient noises for the price of one hash per corner (for domain warps).
float2 GNoise2(float3 p) {
    float3 i = floor(p);
    float3 f = p - i;
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    int3 c = int3(i);
    float a[8], b[8];
    [unroll] for (int k = 0; k < 8; ++k) {
        int3 o = int3(k & 1, (k >> 1) & 1, k >> 2);
        uint h = Hash3(c + o);
        a[k] = dot(GradA(h), f - float3(o));
        b[k] = dot(GradB(h), f - float3(o));
    }
    return float2(Trilerp(a, u), Trilerp(b, u));
}

#endif
