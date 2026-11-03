// Real-time light for translucent surfaces drawn by forward twins (shaders/twins with FORWARD_TWIN): the translator
// includes this in their translated pixel shaders (Variant::forwardLighting), which the game's command lists draw with
// the forward frame constants, the ray-traced scene and the indirect-only probe volume in space1. The sun, the
// strongest local lights and projected lights are traced per pixel; the volume adds the indirect light. Glass reflects
// and Refract glass refracts the traced scene. These draws come after anti-aliasing and denoising, so every ray here is
// deterministic: random choices would sparkle.

#include "reflection.hlsli"

static const uint kForwardLights = 4;

float3 ForwardSunLight(float3 position, float3 normal)
{
    uint water = WaterBodyAt(position);
    float3 direction = SunArrival(water, g_frame.sunDirection);
    float lambert = Lambert(normal, direction);
    if (g_frame.sunEnabled == 0.0 || lambert <= 0.0)
        return 0.0;
    return g_frame.sunColor * lambert * SunVisibilityFrom(position + direction * 0.5, water, g_frame.sunDirection, g_frame.sunShadowMask);
}

// Points of the cell's tail distribution its lights are taken at.
static const uint kForwardTailSamples = 4;

float3 ForwardLocalLight(float3 position, float3 normal)
{
    g_receiverNormal = normal;
    LightSet set = LightSetAt(position);
    uint top[kForwardLights];
    float topWeight[kForwardLights];
    [unroll]
    for (uint k = 0; k < kForwardLights; ++k) {
        top[k] = 0;
        topWeight[k] = 0.0;
    }
    float3 toLight;
    float distance;
    // The lights left out of the strongest, and the cell's tail at fixed points of its distribution, share the shadow
    // ray of the strongest of them: with only the strongest, surfaces lit by many lamps, such as a room's far end in a
    // mirror, came out darker than the room, and a light picked at random per pixel sparkled on glass, which is drawn
    // after anti-aliasing and never denoised.
    float3 remainder = 0.0;
    uint strongest = 0;
    float strongestWeight = 0.0;
    for (uint i = 0; i < set.pointLights; ++i) {
        uint candidate = i;
        float candidateWeight = Luminance(SetLightIrradiance(set, i, position, normal, toLight, distance));
        [unroll]
        for (uint k = 0; k < kForwardLights; ++k) {
            if (candidateWeight > topWeight[k]) {
                uint index = top[k];
                float w = topWeight[k];
                top[k] = candidate;
                topWeight[k] = candidateWeight;
                candidate = index;
                candidateWeight = w;
            }
        }
        if (candidateWeight > 0.0) {
            remainder += SetLightIrradiance(set, candidate, position, normal, toLight, distance);
            if (candidateWeight > strongestWeight) {
                strongest = candidate;
                strongestWeight = candidateWeight;
            }
        }
    }
    for (uint t = 0; t < kForwardTailSamples; ++t) {
        uint tailLight;
        float probability;
        if (!SampleTailLight(set.ranges, (t + 0.5) / kForwardTailSamples, tailLight, probability))
            break;
        float3 irradiance = SetLightIrradiance(set, set.count + tailLight, position, normal, toLight, distance);
        remainder += irradiance / (probability * kForwardTailSamples);
        if (Luminance(irradiance) > strongestWeight) {
            strongest = set.count + tailLight;
            strongestWeight = Luminance(irradiance);
        }
    }
    // The strongest lights, the remainder's shadow ray and the projected lights, in one loop: DXC inlines every call of
    // LightTransmittance.
    uint exact = 0;
    [unroll]
    for (uint e = 0; e < kForwardLights; ++e)
        exact += topWeight[e] > 0.0 ? 1 : 0;
    uint pointRays = strongestWeight > 0.0 ? exact + 1 : exact;
    float3 light = 0.0;
    [loop]
    for (uint s = 0; s < pointRays + set.count - set.pointLights; ++s) {
        uint index = set.pointLights + s - pointRays;
        if (s < exact)
            index = top[s];
        else if (s < pointRays)
            index = strongest;
        float3 irradiance = SetLightIrradiance(set, index, position, normal, toLight, distance);
        if (s >= exact && s < pointRays)
            irradiance = remainder;
        else if (s >= pointRays && Luminance(irradiance) <= VisibleIrradiance())
            continue;
        light += irradiance * LightTransmittance(set, index, position + toLight * 0.5, position + toLight * distance, kLightCentre);
    }
    g_receiverNormal = 0.0;
    return light;
}

// A surface forward twins light: its position, where its direct light is traced from, its normal, albedo and emission.
struct ForwardSurface {
    float3 position;
    float3 lit;
    float3 normal;
    float3 albedo;
    float3 emission;
};

// The light reaching a ForwardSurface, before the tonemap scale. Beyond the volume's window, its nearest probes: the
// sky's colour lit far walls of dark rooms in mirrors.
float3 ForwardLight(ForwardSurface surface)
{
    float3 indirect = VolumeIrradiance(surface.position, surface.normal, true);
    if (indirect.x < 0.0)
        indirect = g_frame.skyColor;
    return ForwardSunLight(surface.lit, surface.normal) + ForwardLocalLight(surface.lit, surface.normal) + indirect * g_frame.indirectScale;
}

// What a ray from a translucent surface meets: the first surface it hits, to be lit as forward twins light theirs, or
// else (false) the light it sees there, before the tonemap scale: water as reflections see it (SeenWater), or past the
// map the 3D skybox and sky (BeyondMap).
bool ForwardRayHit(float3 origin, float3 direction, out ForwardSurface surface, out float3 radiance)
{
    surface = (ForwardSurface)0;
    radiance = 0.0;
    HitSurface hit;
    HitInfo hitInfo;
    uint mask = g_frame.traceMask | ((g_frame.traceMask & kSkyboxMask) == 0 ? kWaterMask : 0u);
    if (!TraceClosest(origin, direction, kMaxDistance, mask, hit, hitInfo)) {
        radiance = BeyondMap(origin, direction, 1.0, 0.002, true, true);
        return false;
    }
    if ((hit.flags & kInstanceWater) != 0) {
        radiance = ClampSample(SeenWater(hit.position, direction, hitInfo.instance, hitInfo.primitive));
        return false;
    }
    surface.position = hit.position;
    surface.lit = hit.position + hit.normal;
    surface.normal = hit.normal;
    surface.albedo = TexturedAlbedo(hitInfo, hit.albedo, 1.0 + hitInfo.t * 0.002);
    surface.emission = HitEmission(hit, hitInfo, hitInfo.t);
    return true;
}

// The share of light a pane of glass reflects at `cosine` to its normal, both faces and the light reflected back and
// forth between them: 2F / (1 + F), what GlassTransmission leaves.
float GlassReflectance(float cosine)
{
    return 1.0 - GlassTransmission(cosine);
}

// Source's Refract blur averages a 3x3 box of 1/512 of the screen each way, about this angle at a 90 degree view: blurred
// glass refracts over four more rays this far around the view's, a fixed pattern (nothing here is denoised).
static const float kGlassBlurAngle = 2.0 / 512.0;

// The rays a forward twin's pixel traces: `refractions` refracted rays (the view's, then the blur's), then a reflected
// one if `count` leaves room. gmodpp_ForwardTwin traces them in one loop (ForwardRayHit).
struct GlassRays {
    uint count;
    uint refractions;
    float3 refractOrigin;
    float3 refractDirection;
    float3 refractWeight;
    float3 reflectOrigin;
    float3 reflectDirection;
    float3 reflectWeight;
};

GlassRays NoGlassRays()
{
    GlassRays rays = (GlassRays)0;
    return rays;
}

// Ray `k` of `rays`, with the share of its light the pixel takes.
void GlassRay(GlassRays rays, uint k, out float3 origin, out float3 direction, out float3 weight)
{
    if (k >= rays.refractions) {
        origin = rays.reflectOrigin;
        direction = rays.reflectDirection;
        weight = rays.reflectWeight;
        return;
    }
    origin = rays.refractOrigin;
    direction = rays.refractDirection;
    weight = rays.refractWeight;
    if (k > 0) {
        float3 t, b;
        BuildBasis(direction, t, b);
        float2 offset = float2(k == 1 ? 1.0 : k == 2 ? -1.0 : 0.0, k == 3 ? 1.0 : k == 4 ? -1.0 : 0.0) * kGlassBlurAngle;
        direction = normalize(direction + t * offset.x + b * offset.y);
    }
}

// Refract glass at a point (shaders/twins/refract_ps2x.fxc): its front face has the normal map's ripples. The view bends
// into the glass there (Snell's law). In a thick pane it crosses to the pane's other face (GlassExit) and bends out of
// it, losing what each face reflects (Fresnel); in a thin pane both faces are the one triangle, flat behind the ripples,
// and it loses what the pane reflects (GlassTransmission). What comes through is tinted; where the material has a
// cubemap (`reflectivity`, its mask) the reflection at the front face is traced too. `blur` (Refract's BLUR combo)
// spreads the refraction over kGlassBlurAngle. The rays' light is before the tonemap scale.
GlassRays RefractedGlass(float3 tint, float3 bumped, float3 faceNormal, float3 position, float reflectivity, float blur)
{
    float3 view = normalize(position - g_frame.eyePosition);
    float3 face = dot(faceNormal, view) > 0.0 ? -faceNormal : faceNormal;
    float3 front = dot(bumped, face) < 0.0 ? -bumped : bumped;
    float cosine = saturate(dot(front, -view));
    float fresnel = Fresnel(cosine, kGlassIor);
    float3 inside = refract(view, front, 1.0 / kGlassIor);
    inside = dot(inside, inside) > 0.0 ? normalize(inside) : view;
    float exitDistance;
    float3 exitNormal;
    bool thick = GlassExit(position - face * 0.01, inside, exitDistance, exitNormal);
    float3 through;
    float3 origin;
    float transmission;
    if (thick) {
        through = refract(inside, exitNormal, kGlassIor);
        // Beyond the critical angle the back face mirrors the light inside the pane: none comes through here.
        transmission = dot(through, through) > 0.0 ? (1.0 - fresnel) * (1.0 - Fresnel(dot(inside, -exitNormal), 1.0 / kGlassIor)) : 0.0;
        origin = position - face * 0.01 + inside * exitDistance - exitNormal * 0.5;
    } else {
        through = refract(inside, face, kGlassIor);
        transmission = GlassTransmission(cosine);
        origin = position - face * 0.5;
    }
    if (dot(through, through) <= 0.0 || dot(through, face) >= 0.0)
        through = view;
    GlassRays rays = NoGlassRays();
    if (transmission > 0.0) {
        rays.refractions = blur > 0.0 ? 5 : 1;
        rays.refractOrigin = origin;
        rays.refractDirection = normalize(through);
        rays.refractWeight = tint * transmission / rays.refractions;
    }
    rays.count = rays.refractions;
    if (reflectivity > 0.0) {
        rays.count += 1;
        rays.reflectOrigin = position + face * 0.5;
        rays.reflectDirection = reflect(view, front);
        rays.reflectWeight = (thick ? fresnel : GlassReflectance(cosine)) * reflectivity;
    }
    return rays;
}

// oC0 albedo (alpha: opacity), oC1 normal (alpha: the cubemap's mask, glass's reflectivity), oC2 light the material
// shows unlit, oC3 world position: the colour the game's lit shaders would write, with the tonemap scale and range fog
// as on opaque surfaces (shaders/deferred_lighting.hlsl). Refract's twin writes its own outputs (RefractedGlass): oC1's
// alpha -1 less its cubemap's mask, oC2 the face normal and in alpha 1 where it blurs.
// Glass reflects what a pane's two faces reflect (GlassReflectance) of the traced scene along the mirrored view, where
// its cubemap was. The game blends the draw by its alpha: where the pane is clear (1 - alpha) the reflection takes its
// share and leaves the rest of what lies behind to the blend, so it adds to both the colour and the alpha. A face of a
// thick pane reflects Fresnel's share alone; a thin pane, 2F / (1 + F).
float4 gmodpp_ForwardTwin(float4 albedo, float4 normal, float4 emissive, float4 position)
{
    float3 n = dot(normal.xyz, normal.xyz) > 0.0625 ? normalize(normal.xyz) : float3(0.0, 0.0, 1.0);
    // Debug views show the light alone, opaque.
    bool lightOnly = g_frame.debugView != 0;
    float3 toEye = normalize(g_frame.eyePosition - position.xyz);
    float alpha = lightOnly ? 1.0 : albedo.a;
    GlassRays rays = NoGlassRays();
    bool refracts = normal.w <= -1.0 && !lightOnly && g_frame.traceMask != 0;
    // The share of the pixel's colour its own lit surface takes; Refract glass shows only its rays.
    float surfaceWeight = 1.0;
    if (refracts) {
        rays = RefractedGlass(albedo.rgb, n, normalize(emissive.xyz), position.xyz, -1.0 - normal.w, emissive.w);
        alpha = 1.0;
    } else {
        if (normal.w > 0.0 && !lightOnly && g_frame.traceMask != 0 && g_frame.reflectionScale > 0.0) {
            float3 facing = dot(n, toEye) < 0.0 ? -n : n;
            // A face of a thick pane reflects alone (its other face is drawn too); a thin pane reflects at both faces.
            float exitDistance;
            float3 exitNormal;
            bool thick = GlassExit(position.xyz - facing * 0.01, -facing, exitDistance, exitNormal);
            float cosine = saturate(dot(facing, toEye));
            float reflectance = (thick ? Fresnel(cosine, kGlassIor) : GlassReflectance(cosine)) * saturate(normal.w) * g_frame.reflectionScale;
            float clear = (1.0 - saturate(albedo.a)) * reflectance;
            alpha = saturate(albedo.a) + clear;
            surfaceWeight = saturate(albedo.a) / max(alpha, 1e-4);
            rays.count = 1;
            rays.reflectOrigin = position.xyz + facing * 0.5;
            rays.reflectDirection = reflect(-toEye, facing);
            rays.reflectWeight = clear / max(alpha, 1e-4);
        }
    }
    // The pixel's own surface (k = 0), then what its rays hit, lit by one call of ForwardLight: DXC inlines every call,
    // and each copy of the lighting took seconds to compile on the render thread.
    float3 color = 0.0;
    [loop]
    for (uint k = refracts ? 1 : 0; k <= rays.count; ++k) {
        ForwardSurface surface;
        float3 weight = surfaceWeight;
        if (k == 0) {
            surface.position = position.xyz;
            surface.lit = position.xyz;
            surface.normal = n;
            surface.albedo = lightOnly ? 1.0 : albedo.rgb;
            surface.emission = emissive.rgb;
        } else {
            float3 origin, direction;
            GlassRay(rays, k - 1, origin, direction, weight);
            float3 radiance;
            if (!ForwardRayHit(origin, direction, surface, radiance)) {
                color += radiance * weight;
                continue;
            }
        }
        float3 radiance = surface.albedo * ForwardLight(surface) + surface.emission;
        color += (k == 0 ? radiance : ClampSample(radiance)) * weight;
    }
    color *= g_frame.tonemapScale;
    if (g_frame.eyeWater != kNoView) {
        // Seen from under water, through the water (ThroughWater, at one fixed point: nothing here is denoised).
        float distance = length(position.xyz - g_frame.eyePosition);
        uint seed = 0;
        float3 inscatter = ThroughWater(0.0, g_frame.eyePosition, -toEye, distance, g_frame.eyeWater, 1, seed);
        color = color * exp(-LoadWaterBody(g_frame.eyeWater).extinction * distance) + inscatter * g_frame.tonemapScale;
    } else if (g_frame.fogEnabled != 0) {
        float fog = FogOpacity(mul(g_frame.viewProj, float4(position.xyz, 1.0)).w);
        // Lit fog (shaders/volumetrics.hlsl) is lit where it meets the surface, a unit towards the eye.
        float3 fogColor = g_frame.fogColor;
        if (g_frame.volumetricFog != 0) {
            float3 air = position.xyz + toEye;
            fogColor = LitFog(air, uint3(g_frame.probeRedView, g_frame.probeGreenView, g_frame.probeBlueView)) * g_frame.tonemapScale;
        }
        color = lerp(color, fogColor, fog);
    }
    return float4(color, alpha);
}
