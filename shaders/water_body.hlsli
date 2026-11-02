#ifndef GMODPP_WATER_BODY_HLSLI
#define GMODPP_WATER_BODY_HLSLI

// The map's bodies of water (Scene::SetWater): which one holds a point, the optics of its water, and the caustics its
// rippled surface throws (shaders/caustics.hlsl).

// Mirrors GpuWaterBody in src/scene.h. The water is a medium that attenuates light by `extinction` per unit and whose
// own light tends to `fogColor` (linear, before the tonemap scale) where the open sun and sky light it fully.
struct WaterBody
{
    float3 extinction;
    float surfaceZ;
    float3 fogColor;
    float reflectTint;
    float3 refractTint;
    uint framesFirst;
    // The normal map's coordinates at a point p: (dot(p, textureS.xyz) + textureS.w, the same with textureT), then
    // through the bump transform's rows applied to (u, v, seconds, 1): z scrolls them (TextureScroll).
    float4 textureS;
    float4 textureT;
    float4 bumpS;
    float4 bumpT;
    float3 mins;
    uint frameCount;
    float3 maxs;
    float frameRate;
};

static const uint kNoWater = 0xFFFFFFFFu;
// Water surfaces' instances (kWaterMask in src/scene.h), which only rays that look for water meet.
static const uint kWaterMask = 0x40;
static const float kWaterIor = 1.333;

static const uint kWaterBodyStride = 144;

WaterBody LoadWaterBody(uint index)
{
    ByteAddressBuffer bodies = ResourceDescriptorHeap[g_frame.waterBodiesView];
    return bodies.Load<WaterBody>(index * kWaterBodyStride);
}

// The BSP leaf holding a point, by the engine's point-in-leaf walk, or -1 in solid space or without visibility data.
int LeafAt(float3 position)
{
    if (g_frame.clusterCount == 0)
        return -1;
    ByteAddressBuffer nodes = ResourceDescriptorHeap[g_frame.bspNodesView];
    int node = 0;
    for (uint steps = 0; node >= 0 && steps < 512; ++steps) {
        float4 plane = asfloat(nodes.Load4(node * 24));
        int2 children = asint(nodes.Load2(node * 24 + 16));
        node = dot(plane.xyz, position) - plane.w >= 0.0 ? children.x : children.y;
    }
    return node >= 0 ? -1 : -node - 1;
}

// The body of water holding a point, or kNoWater. Brush entities' bodies (from g_frame.brushWaterFirst on) are found by
// their bounds; points within a world body's bounds by their BSP leaf, without visibility data by the bounds.
// The last point WaterBodyAt walked the tree for in this thread and its body: a hit's sun and lamp shadow rays leave
// from the same point.
static float3 g_waterLookupPoint = float3(1e30, 1e30, 1e30);
static uint g_waterLookupBody = kNoWater;

uint WaterBodyAt(float3 position)
{
    if (g_frame.waterBodyCount == 0 || position.z >= g_frame.waterTop)
        return kNoWater;
    if (all(position == g_waterLookupPoint))
        return g_waterLookupBody;
    g_waterLookupPoint = position;
    g_waterLookupBody = kNoWater;
    // Brush entities' bodies first, then the world's: the loop's index wraps from the last body to 0.
    uint inside = kNoWater;
    for (uint n = 0; n < g_frame.waterBodyCount && inside == kNoWater; ++n) {
        uint b = (g_frame.brushWaterFirst + n) % g_frame.waterBodyCount;
        WaterBody body = LoadWaterBody(b);
        if (all(position.xy >= body.mins.xy) && all(position.xy <= body.maxs.xy) && position.z < body.surfaceZ && position.z > body.mins.z - 1.0)
            inside = b;
    }
    if (inside == kNoWater || inside >= g_frame.brushWaterFirst || g_frame.leafWaterView == kNoView) {
        g_waterLookupBody = inside;
        return inside;
    }
    int leaf = LeafAt(position);
    if (leaf < 0)
        return kNoWater;
    ByteAddressBuffer leafWater = ResourceDescriptorHeap[g_frame.leafWaterView];
    int water = asint(leafWater.Load(leaf * 4));
    g_waterLookupBody = water >= 0 && uint(water) < g_frame.brushWaterFirst ? uint(water) : kNoWater;
    return g_waterLookupBody;
}

// The body of water a ray met where it hit a water surface triangle (kWaterMask), without a BSP walk: a brush entity's
// instance holds its body (InstanceRecord::waterBody, at byte 88 of the record), the world's surfaces each triangle's.
uint WaterBodyOfTriangle(uint instance, uint primitive)
{
    ByteAddressBuffer records = ResourceDescriptorHeap[g_frame.instancesView];
    uint brushBody = records.Load(instance * 96 + 88);
    if (brushBody != kNoView)
        return brushBody < g_frame.waterBodyCount ? brushBody : kNoWater;
    if (g_frame.waterTrianglesView == kNoView)
        return kNoWater;
    ByteAddressBuffer triangles = ResourceDescriptorHeap[g_frame.waterTrianglesView];
    uint water = triangles.Load(primitive * 4);
    return water < g_frame.waterBodyCount ? water : kNoWater;
}

// The share of unpolarised light a smooth interface reflects, for light meeting it at `cosine` to its normal from the
// side whose index of refraction is 1 / `ior` of the other's (Fresnel's equations); 1 beyond the critical angle.
float Fresnel(float cosine, float ior)
{
    cosine = saturate(cosine);
    float sine2 = (1.0 - cosine * cosine) / (ior * ior);
    if (sine2 >= 1.0)
        return 1.0;
    float cosineT = sqrt(1.0 - sine2);
    float s = (cosine - ior * cosineT) / (cosine + ior * cosineT);
    float p = (ior * cosine - cosineT) / (ior * cosine + cosineT);
    return 0.5 * (s * s + p * p);
}

// The direction towards the sun under a flat water surface, for sunlight from `airDirection` above it.
float3 RefractedSunDirection(float3 airDirection)
{
    float3 down = refract(-airDirection, float3(0.0, 0.0, 1.0), 1.0 / kWaterIor);
    return dot(down, down) > 0.0 ? -down : float3(0.0, 0.0, 1.0);
}

// The share of sunlight from `airDirection` that a flat surface lets into the water, as irradiance across its beam:
// what is not reflected, gathered into a beam narrower by the ratio of the cosines.
float SunIntoWater(float3 airDirection)
{
    float cosine = saturate(airDirection.z);
    float cosineT = RefractedSunDirection(airDirection).z;
    return cosine > 0.0 ? (1.0 - Fresnel(cosine, kWaterIor)) * cosine / max(cosineT, 1e-3) : 0.0;
}

// The caustics and the water's sun maps (shaders/caustics.hlsl) index the lines of flat water's bent sunlight
// (RefractedSunDirection) by where they cross the plane of the highest water surface (g_frame.waterTop), so every
// point of a line shares its cell, whichever body of water holds it. `along` is a point's distance along its line below
// that plane.
float2 SunLineCell(float3 position, out float along)
{
    float3 up = RefractedSunDirection(g_frame.sunDirection);
    along = (g_frame.waterTop - position.z) / max(up.z, 1e-3);
    return position.xy + up.xy * along;
}

// The caustics fade out over this outer share of their square's half-width, so its edge shows no line on the floor.
static const float kCausticsEdgeFade = 0.5;

// How much the rippled surface focuses sunlight at a point under water: 1 where the light falls as through flat water,
// or outside the caustics' square.
float CausticsAt(float3 position)
{
    if (g_frame.causticsView == kNoView)
        return 1.0;
    float along;
    float2 uv = (SunLineCell(position, along) - g_frame.causticsOrigin) * g_frame.causticsInvSize;
    if (any(uv <= 0.0) || any(uv >= 1.0))
        return 1.0;
    Texture2D<float> caustics = ResourceDescriptorHeap[g_frame.causticsView];
    float2 edge = 0.5 - abs(uv - 0.5);
    return lerp(1.0, caustics.SampleLevel(g_linearClamp, uv, 0.0), saturate(min(edge.x, edge.y) / (0.5 * kCausticsEdgeFade)));
}

// A sun map texel's line meets no water surface (negative), or its light is not stopped.
static const float kNoWaterCell = -1.0;
static const float kOpenWater = 1e30;

// The sunlight a sun map lets through at `along` on the line crossing the plane at `crossing` (SunLineCell), bilinear
// over the four nearest cells (each the light above the surface its line enters, or none where something stops it
// before `along`); -1 outside the map or where no water lies on the lines.
float SunMapAt(uint view, float2 origin, float invSize, float2 crossing, float along, out float3 light)
{
    light = 1.0;
    if (view == kNoView)
        return -1.0;
    Texture2D<float4> sunMap = ResourceDescriptorHeap[view];
    uint cells, height;
    sunMap.GetDimensions(cells, height);
    float2 cell = (crossing - origin) * invSize * cells - 0.5;
    if (any(cell < 0.0) || any(cell > cells - 1.0))
        return -1.0;
    int2 base = int2(floor(cell));
    float2 f = cell - base;
    // Where the light stops on neighbouring lines differs by up to the cells' width on a sloped receiver.
    float bias = 1.5 / (invSize * cells);
    light = 0.0;
    float weight = 0.0;
    [unroll]
    for (uint k = 0; k < 4; ++k) {
        int2 offset = int2(k & 1, k >> 1);
        float4 texel = sunMap.Load(int3(min(base + offset, int(cells) - 1), 0));
        float w = (offset.x != 0 ? f.x : 1.0 - f.x) * (offset.y != 0 ? f.y : 1.0 - f.y);
        if (texel.a < 0.0)
            continue;
        weight += w;
        light += w * texel.rgb * (texel.a + bias >= along ? 1.0 : 0.0);
    }
    if (weight <= 0.0) {
        light = 1.0;
        return -1.0;
    }
    light /= weight;
    return 1.0;
}

// The shadows above the surface and in the water at a point under water: the fine sun map, else the coarse one; false
// (and 1) beyond both.
bool WaterSunMapAt(float3 position, out float3 light)
{
    float along;
    float2 crossing = SunLineCell(position, along);
    if (SunMapAt(g_frame.waterSunView, g_frame.causticsOrigin, g_frame.causticsInvSize, crossing, along, light) >= 0.0)
        return true;
    return SunMapAt(g_frame.coarseWaterSunView, g_frame.coarseCausticsOrigin, g_frame.coarseCausticsInvSize, crossing, along, light) >= 0.0;
}

// The sunlight that reaches a point under water against the light that would through open water: WaterSunMapAt's
// shadows and the caustics.
float3 WaterSunAt(float3 position)
{
    float3 light;
    WaterSunMapAt(position, light);
    return light * CausticsAt(position);
}

// A body's surface as the game last drew it (Lighting::WriteWaterDraws): its bump transform rows over (u, v, 0, 1), its
// normal map's frame then, and when (waterTime's clock); normalMap is kNoView where it was not drawn.
struct WaterDraw
{
    float4 bumpS;
    float4 bumpT;
    // Its texture coordinates at a point, as its vertices hold them (where `mapped`).
    float4 uvS;
    float4 uvT;
    uint normalMap;
    float time;
    uint mapped;
    uint padding;
};

// How long a drawn frame of an animated normal map stands in for the animation, in seconds.
static const float kWaterDrawFrameAge = 0.1;

// The world-space normal of a body's rippled surface at a point, facing up. Where the game drew the surface, its own
// normal map frame and bump transform, the scroll carried on from when it was drawn at the material's rate
// (TextureScroll); elsewhere the material's frames on the mod's clock (AnimatedTexture, TextureScroll).
// The tangent-space normal of the body's normal map at a point (WaterSurfaceNormal), or 0 without a normal map.
float3 WaterSurfaceSlope(uint water, WaterBody body, float3 position)
{
    float2 uv = float2(dot(position, body.textureS.xyz) + body.textureS.w, dot(position, body.textureT.xyz) + body.textureT.w);
    WaterDraw draw = (WaterDraw)0;
    draw.normalMap = kNoView;
    if (g_frame.waterDrawsView != kNoView && water < 256) {
        ByteAddressBuffer draws = ResourceDescriptorHeap[g_frame.waterDrawsView];
        draw = draws.Load<WaterDraw>(water * 80);
    }
    // The engine maps the material by another width than its texinfo's texdata (on gm_construct half the scale, offset
    // included): the drawn coordinates over the texinfo's scale the body's own mapping, which moves with a brush
    // entity's water and holds for the material's bodies the game has not drawn.
    if (draw.mapped != 0)
        uv *= length(draw.uvS.xy) / max(length(body.textureS.xy), 1e-9);
    float4 coordinates = float4(uv, g_frame.waterTime, 1.0);
    float2 bump = float2(dot(coordinates, body.bumpS), dot(coordinates, body.bumpT));
    uint view = kNoView;
    if (draw.normalMap != kNoView) {
        float age = g_frame.waterTime - draw.time;
        if (age >= 0.0 && (body.frameCount <= 1 || age < kWaterDrawFrameAge)) {
            float4 drawn = float4(uv, 0.0, 1.0);
            bump = float2(dot(drawn, draw.bumpS), dot(drawn, draw.bumpT)) + float2(body.bumpS.z, body.bumpT.z) * age;
            view = draw.normalMap;
        }
    }
    if (view == kNoView) {
        if (body.frameCount == 0)
            return 0.0;
        uint frame = body.frameCount > 1 ? uint(g_frame.waterTime * body.frameRate) % body.frameCount : 0;
        ByteAddressBuffer frames = ResourceDescriptorHeap[g_frame.waterFramesView];
        view = frames.Load((body.framesFirst + frame) * 4);
    }
    Texture2D<float4> normalMap = ResourceDescriptorHeap[NonUniformResourceIndex(view)];
    return normalMap.SampleLevel(g_linearWrap, bump, 0.0).xyz * 2.0 - 1.0;
}

// The world directions of a body's texture axes s and t on its surface: as its drawn vertices map them when the game
// drew it, else by its texinfo. False without either.
bool WaterTextureAxes(uint water, WaterBody body, out float2 s, out float2 t)
{
    s = body.textureS.xy;
    t = body.textureT.xy;
    if (g_frame.waterDrawsView != kNoView && water < 256) {
        ByteAddressBuffer draws = ResourceDescriptorHeap[g_frame.waterDrawsView];
        WaterDraw draw = draws.Load<WaterDraw>(water * 80);
        if (draw.mapped != 0) {
            s = draw.uvS.xy;
            t = draw.uvT.xy;
        }
    }
    if (dot(s, s) <= 0.0 || dot(t, t) <= 0.0)
        return false;
    s = normalize(s);
    t = normalize(t);
    return true;
}

float3 WaterSurfaceNormal(uint water, WaterBody body, float3 position)
{
    float3 tangent = WaterSurfaceSlope(water, body, position);
    if (all(tangent == 0.0))
        return float3(0.0, 0.0, 1.0);
    // Tangent-space x runs along the texture's s axis and y along t (Water_vs20's vTangentS and vTangentT).
    float2 s, t;
    if (!WaterTextureAxes(water, body, s, t))
        return float3(0.0, 0.0, 1.0);
    return normalize(float3(s * tangent.x + t * tangent.y, max(tangent.z, 0.05)));
}

#endif
