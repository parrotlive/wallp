// Built-in shaders, compiled offline by fxc into headers (see CMakeLists.txt).

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Fullscreen triangle generated from SV_VertexID; no vertex or index buffer.
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

Texture2D    gSource : register(t0);
SamplerState gLinear : register(s0);

// Bilinear upscale of the reduced-resolution render target to the back buffer.
float4 PSBlit(VSOut i) : SV_Target {
    return float4(gSource.SampleLevel(gLinear, i.uv, 0).rgb, 1.0);
}
