#ifndef GMODPP_GAME_FOG_HLSLI
#define GMODPP_GAME_FOG_HLSLI

// Fog for the game's own shaders drawn in the main view's translucent pass (src/shader_translator.cpp): the translator
// includes this where a pixel shader blends Source's fog (g_FogParams, g_LinearFogColor) or draws what Source leaves
// unfogged (Variant::fogEpilogue). They reach the main view's frame constants in space1 (src/shaderapi_pipelines.cpp),
// read only while gmodpp_alpha.w is not 0: 1 where the pass blends the scene's fog colour, 2 where it blends another
// (black, grey or white), 3 where it has no fog.

#ifndef GMODPP_GAME_DRAW
#define GMODPP_GAME_DRAW 1
#endif
#include "froxels.hlsli"

// A pixel the game draws, from its SV_Position in a viewport of `viewportSize`.
struct GamePixel
{
    float2 uv;
    float viewDepth;
    float distance;
};

GamePixel GamePixelAt(float4 position, float2 viewportSize)
{
    GamePixel pixel;
    pixel.uv = position.xy / viewportSize;
    float depth = (position.z - g_frame.depthMin) / g_frame.depthScale;
    float4 offset = mul(g_frame.clipToEye, float4(pixel.uv.x * 2.0 - 1.0, 1.0 - pixel.uv.y * 2.0, depth, 1.0));
    // The inverse view-projection maps clip space over w back to the point (from the eye) over w.
    pixel.viewDepth = 1.0 / max(offset.w, 1e-6);
    pixel.distance = length(offset.xyz * pixel.viewDepth);
    return pixel;
}

// Source's range fog parameters (g_FogParams: start over range, water height, maximum density, 1 / range) a game shader
// computes its fog from, set so that its factor is the fog at the pixel's distance (FogOpacity) rather than its view
// depth: x the factor's negative, z 1, w 0. Fog under water, which Source blends by height, stays.
float4 gmodpp_FogParams(float4 original, float4 position, float2 viewportSize)
{
    if (g_frame.fogEnabled == 0 || g_frame.fogParams.y > 0.0)
        return original;
    GamePixel pixel = GamePixelAt(position, viewportSize);
    float fog = saturate(min(g_frame.fogParams.z, pixel.distance * g_frame.fogParams.w - g_frame.fogParams.x));
    return float4(-fog, original.y, 1.0, 0.0);
}

// The fog colour (after the tonemap scale) a game shader blends over a pixel by Source's fog, in place of the map's flat
// fog colour `original`: with lit fog, the light the froxel volume gathered up to the pixel over the fog's opacity
// there, so that Source's blend adds that light; otherwise the flat colour lit as the light is now (FogDaylight).
// original.w (the destination alpha's depth range) is kept.
float4 gmodpp_LitFogColor(float4 original, float4 position, float2 viewportSize)
{
    float3 flat = original.rgb * FogDaylight();
    if (g_frame.volumetricView == kNoView || g_frame.volumetricFog == 0)
        return float4(flat, original.w);
    GamePixel pixel = GamePixelAt(position, viewportSize);
    float opacity = FogOpacity(pixel.distance);
    float3 light = FroxelLight(pixel.uv, pixel.viewDepth).rgb * g_frame.tonemapScale;
    return float4(opacity > 1e-3 ? light / opacity : flat, original.w);
}

// A game draw Source leaves unfogged (particles) seen through the medium the froxel volume holds: its colour comes
// through as far as the medium lets it, and the share of what lies behind that it covers takes the medium's light in
// front of it, which is already in what lies behind. `mode` says how the draw blends: 1 by its alpha, 2 premultiplied
// by its alpha, 3 added; 0 leaves it as drawn. Under water the game's own water fog stays.
float4 gmodpp_FogBlend(float4 color, float4 position, float2 viewportSize, float mode)
{
    if (mode == 0.0 || g_frame.volumetricView == kNoView || g_frame.eyeWater != kNoView)
        return color;
    GamePixel pixel = GamePixelAt(position, viewportSize);
    float4 medium = FroxelLight(pixel.uv, pixel.viewDepth);
    float transmittance = g_frame.volumetricFog != 0 ? 1.0 - FogOpacity(pixel.distance) : medium.a;
    float3 light = medium.rgb * g_frame.tonemapScale;
    color.rgb *= transmittance;
    if (mode == 1.0)
        color.rgb += light;
    else if (mode == 2.0)
        color.rgb += light * color.a;
    return color;
}

#endif
