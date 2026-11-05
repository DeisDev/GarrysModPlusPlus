#include "night_sky.hlsli"

// The game's 2D sky passes drawn with the procedural sky and its night sky (SkyMain), or the sun's disc added over the
// game's own sky (DiscMain), in place of the game's pixel shader (src/shaderapi_pipelines.cpp, DrawSky). The translated
// vertex shader still places the faces; the constants come through the forward twins' root parameter (register space 1).
struct SkyDrawConstants
{
    // Clip space to points relative to the eye for the draw: the inverse of its view-projection rows (vertex c8-c11) as
    // drawn, less the eye's position.
    row_major float4x4 clipToEye;
    // The viewport's left, top, width and height in pixels.
    float4 viewport;
    // cLightScale.x of the game's sky shader, which Source multiplies its colour by.
    float tonemapScale;
    uint skyView;
    uint2 padding0;
    // The clouds over the sky (shaders/clouds.hlsl), as (light, transmittance): the last main view's history, whose clip
    // space a direction reaches through cloudViewProj's rows and which spans the screen widened by cloudOverscan on each
    // side, and their cube for directions outside it; kNoView without clouds.
    float4 cloudViewProj[4];
    uint cloudHistoryView;
    uint cloudCubeView;
    float cloudOverscan;
    // Aerial perspective's change of the sky's light by the cloud shadows (crepuscular rays) over the history's view, in
    // the procedural sky's units, or kNoView.
    uint aerialSkyView;
    NightSkyConstants night;
};

static const uint kNoView = 0xFFFFFFFFu;

ConstantBuffer<SkyDrawConstants> g_sky : register(b0, space1);
SamplerState g_linearClamp : register(s0, space1);

// The translated shaders' interpolants (Interpolants in src/shader_translator.cpp), so this shader links with them.
struct Interpolants
{
    float4 position : SV_Position;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float4 t4 : TEXCOORD4;
    float4 t5 : TEXCOORD5;
    float4 t6 : TEXCOORD6;
    float4 t7 : TEXCOORD7;
    float4 t8 : TEXCOORD8;
    float4 t9 : TEXCOORD9;
    float4 color0 : COLOR0;
    float4 color1 : COLOR1;
    float4 fog : FOG;
};

// The world direction through a pixel centre. Translated vertex shaders move the game's geometry by D3D9's half-pixel
// offset (gmodpp_halfPixel), so a pixel centre shows the game's clip-space point half a pixel up and left of it.
float3 ViewDirection(float2 pixel)
{
    float2 ndc = (pixel - g_sky.viewport.xy) / g_sky.viewport.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
    ndc -= float2(1.0, -1.0) / g_sky.viewport.zw;
    float4 nearPoint = mul(g_sky.clipToEye, float4(ndc, 0.0, 1.0));
    float4 farPoint = mul(g_sky.clipToEye, float4(ndc, 1.0, 1.0));
    return normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
}

// The angle a pixel spans around the direction through it.
float PixelAngle(float2 pixel, float3 direction)
{
    return sqrt(length(ViewDirection(pixel + float2(1.0, 0.0)) - direction) * length(ViewDirection(pixel + float2(0.0, 1.0)) - direction));
}

// The clouds toward a direction as (light, transmittance): the main view's history where it holds the direction, fading
// to the cube over its overscan, else the cube.
float4 CloudsToward(float3 direction)
{
    float4 clouds = float4(0.0, 0.0, 0.0, 1.0);
    if (g_sky.cloudCubeView == kNoView)
        return clouds;
    float inside = 0.0;
    float2 uv = 0.0;
    if (g_sky.cloudHistoryView != kNoView) {
        float4 clip = float4(dot(g_sky.cloudViewProj[0].xyz, direction), dot(g_sky.cloudViewProj[1].xyz, direction), 0.0,
            dot(g_sky.cloudViewProj[3].xyz, direction));
        uv = clip.xy / max(clip.w, 1e-6) * float2(0.5, -0.5) + 0.5;
        float2 edge = min(uv, 1.0 - uv);
        inside = clip.w > 0.0 ? saturate(min(edge.x, edge.y) * (1.0 + 2.0 * g_sky.cloudOverscan) / max(g_sky.cloudOverscan, 1e-3)) : 0.0;
    }
    if (inside < 1.0) {
        TextureCube<float4> cube = ResourceDescriptorHeap[g_sky.cloudCubeView];
        clouds = cube.SampleLevel(g_linearClamp, direction, 0.0);
    }
    if (inside > 0.0) {
        Texture2D<float4> history = ResourceDescriptorHeap[g_sky.cloudHistoryView];
        clouds = lerp(clouds, history.SampleLevel(g_linearClamp, uv, 0.0), inside);
    }
    return clouds;
}

// The cloud shadows' change of the sky's light toward a direction (shaders/sky_atmosphere.hlsl step 7), fading out over
// the overscan of the view it covers; none outside it.
float3 AerialSkyToward(float3 direction)
{
    if (g_sky.aerialSkyView == kNoView)
        return 0.0;
    float4 clip = float4(dot(g_sky.cloudViewProj[0].xyz, direction), dot(g_sky.cloudViewProj[1].xyz, direction), 0.0,
        dot(g_sky.cloudViewProj[3].xyz, direction));
    if (clip.w <= 0.0)
        return 0.0;
    float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
    float2 edge = min(uv, 1.0 - uv);
    float inside = saturate(min(edge.x, edge.y) * (1.0 + 2.0 * g_sky.cloudOverscan) / max(g_sky.cloudOverscan, 1e-3));
    if (inside <= 0.0)
        return 0.0;
    Texture2D<float4> aerial = ResourceDescriptorHeap[g_sky.aerialSkyView];
    return aerial.SampleLevel(g_linearClamp, uv, 0.0).rgb * inside;
}

float4 SkyMain(Interpolants input) : SV_Target0
{
    float3 direction = ViewDirection(input.position.xy);
    float pixelAngle = PixelAngle(input.position.xy, direction);
    TextureCube<float4> sky = ResourceDescriptorHeap[g_sky.skyView];
    Texture2D<float4> transmittance = ResourceDescriptorHeap[g_sky.night.transmittanceView];
    float4 procedural = sky.SampleLevel(g_linearClamp, direction, 0.0);
    // The clouds hide what lies beyond them: the night sky and the Moon by its gain, the disc as far as the target shows.
    float4 clouds = CloudsToward(direction);
    float logGain = LogGainThroughClouds(procedural.a, clouds.a);
    float moonCoverage;
    float3 moon = MoonDisc(g_sky.night, direction, pixelAngle, logGain, g_sky.tonemapScale, transmittance, g_linearClamp, moonCoverage);
    float3 color = max(procedural.rgb + AerialSkyToward(direction), 0.0) * clouds.a + clouds.rgb;
    if (g_sky.night.parts != 0)
        color += NightSkyLight(g_sky.night, direction, logGain, kPointFootprint * pixelAngle, g_sky.tonemapScale, transmittance, g_linearClamp) *
            (1.0 - moonCoverage);
    color = OverDisc(max(color, 0.0) * g_sky.tonemapScale, moon / max(moonCoverage, 1e-6), moonCoverage);
    if (g_sky.night.disc != 0) {
        float discCoverage;
        float3 disc = SunDisc(g_sky.night, direction, pixelAngle, transmittance, g_linearClamp, discCoverage) * DiscThroughClouds(g_sky.night, clouds.a, g_sky.tonemapScale);
        color = OverDisc(color, disc / max(discCoverage, 1e-6), discCoverage);
    }
    return float4(max(color, 0.0), 1.0);
}

float4 DiscMain(Interpolants input) : SV_Target0
{
    float3 direction = ViewDirection(input.position.xy);
    Texture2D<float4> transmittance = ResourceDescriptorHeap[g_sky.night.transmittanceView];
    float coverage;
    return float4(SunDisc(g_sky.night, direction, PixelAngle(input.position.xy, direction), transmittance, g_linearClamp, coverage), 0.0);
}
