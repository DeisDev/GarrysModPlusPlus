#define ROOT_SIGNATURE "DescriptorTable(SRV(t0), visibility = SHADER_VISIBILITY_PIXEL), DescriptorTable(SRV(t1), visibility = SHADER_VISIBILITY_PIXEL)"

Texture2D<float4> g_result : register(t0);
// The G-buffer's coverage flags (red), for CoveredPS.
Texture2D<float4> g_flags : register(t1);

struct VertexOutput
{
    float4 position : SV_Position;
};

[RootSignature(ROOT_SIGNATURE)]
VertexOutput VSMain(uint vertex : SV_VertexID)
{
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    VertexOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// The lit image, as the linear light the frame holds.
float4 Result(float2 pixel)
{
    return g_result.Load(int3(pixel, 0));
}

[RootSignature(ROOT_SIGNATURE)]
float4 PSMain(VertexOutput input) : SV_Target
{
    return Result(input.position.xy);
}

// PSMain for the pixels the run drew (G-buffer surfaces and water): the viewmodel's and the 3D skybox's runs light only a
// rectangle around them (shaders/view_rect.hlsl).
[RootSignature(ROOT_SIGNATURE)]
float4 CoveredPS(VertexOutput input) : SV_Target
{
    float4 coverage = g_flags.Load(int3(input.position.xy, 0));
    if (coverage.r < 0.5 && coverage.g < 0.5)
        discard;
    return Result(input.position.xy);
}

// Writes a depth texture (shaders/translucent_depth.hlsl) into the depth buffer.
[RootSignature(ROOT_SIGNATURE)]
float DepthPS(VertexOutput input) : SV_Depth
{
    return g_result.Load(int3(input.position.xy, 0)).r;
}
