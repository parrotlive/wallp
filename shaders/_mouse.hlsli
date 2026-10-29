// Helpers for mouse-reactive wallpapers (their sidecar sets "mouse": true).
// iTrail[0..15] holds cursor positions about 1/30 s apart, newest first; iMouse.xy is the
// position this frame, so the newest trail segment runs from iMouse.xy to iTrail[0].

#ifndef WALLP_MOUSE_HLSLI
#define WALLP_MOUSE_HLSLI

static const float kTrailStep = 1.0 / 30.0;

// Distance from p to the segment a-b; `along` receives the position on it (0 at a, 1 at b).
float SegmentDistance(float2 p, float2 a, float2 b, out float along) {
    float2 ab = b - a, ap = p - a;
    along = saturate(dot(ap, ab) / max(dot(ab, ab), 1e-6));
    return length(ap - ab * along);
}

// Trail point i (0 = this frame's cursor, 1..16 = iTrail[0..15]): xy = position, z = age.
float3 TrailPoint(int i) {
    return i == 0 ? float3(iMouse.xy, 0.0) : iTrail[max(i - 1, 0)].xyz;  // both sides are evaluated
}

// Cursor speed around trail point i, in pixels per second.
float TrailSpeed(int i) {
    int j = min(i, 14);
    float3 a = iTrail[j].xyz, b = iTrail[j + 1].xyz;
    return length(a.xy - b.xy) / max(b.z - a.z, 1e-3);
}

#endif
