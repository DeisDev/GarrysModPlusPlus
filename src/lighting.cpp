#include "lighting.h"

#include "busy_window.h"
#include "command_line.h"
#include "light_queries.h"
#include "log.h"
#include "options.h"
#include "parallel_jobs.h"
#include "settings.h"

#include <g_aoCS.h>
#include <g_bakedBounceCS.h>
#include <g_causticsCS.h>
#include <g_aoOmmCS.h>
#include <g_bakedBounceOmmCS.h>
#include <g_causticsOmmCS.h>
#include <g_flareVisibilityCS.h>
#include <g_flareVisibilityOmmCS.h>
#include <g_regenInputsCS.h>
#include <g_regenResolveCS.h>
#include <g_deferredLightingCacheOmmCS.h>
#include <g_deferredLightingOmmCS.h>
#include <g_translucentDepthOmmCS.h>
#include <g_compositeCoveredPS.h>
#include <g_compositeDepthPS.h>
#include <g_compositePS.h>
#include <g_compositeVS.h>
#include <g_deferredLightingCS.h>
#include <g_deferredLightingCacheCS.h>
#include <g_directCS.h>
#include <g_directOmmCS.h>
#include <g_directSunCS.h>
#include <g_directSunOmmCS.h>
#include <g_directWideCS.h>
#include <g_directWideOmmCS.h>
#include <g_dlssInputsCS.h>
#include <g_dlssResolveCS.h>
#include <g_giFilterCS.h>
#include <g_giTemporalCS.h>
#include <g_giTraceCS.h>
#include <g_giTraceCacheCS.h>
#include <g_giTraceCacheOmmCS.h>
#include <g_giTraceOmmCS.h>
#include <g_nrdInputsCS.h>
#include <g_sunShadowCS.h>
#include <g_sunShadowOmmCS.h>
#include <g_nrdResolveCS.h>
#include <g_translucentDepthCS.h>
#include <g_viewRectCS.h>
#include <g_primaryCS.h>
#include <g_primaryOmmCS.h>
#include <g_probesCS.h>
#include <g_probesCacheCS.h>
#include <g_probesCacheOmmCS.h>
#include <g_probesOmmCS.h>
#include <g_reflectionTemporalCS.h>
#include <g_reflectionsCS.h>
#include <g_reflectionsCacheCS.h>
#include <g_reflectionsCacheOmmCS.h>
#include <g_reflectionsOmmCS.h>
#include <g_restirInitialCS.h>
#include <g_restirInitialOmmCS.h>
#include <g_restirPdfCS.h>
#include <g_restirPresampleCS.h>
#include <g_restirRegirCS.h>
#include <g_restirShadeCS.h>
#include <g_restirShadeOmmCS.h>
#include <g_restirSpatialCS.h>
#include <g_restirTemporalCS.h>
#include <g_sharcResolveCS.h>
#include <g_sharcUpdateCS.h>
#include <g_sharcUpdateOmmCS.h>
#include <g_cloudsCS.h>
#include <g_skinCS.h>
#include <g_skyAtmosphereCS.h>
#include <g_skyCS.h>
#include <g_surfaceCS.h>
#include <g_swayCS.h>
#include <g_taaCS.h>
#include <g_taaSharpenCS.h>
#include <g_volumetricsCS.h>
#include <g_volumetricsOmmCS.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>
#include <numeric>
#include <thread>
#include <tuple>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

namespace {

constexpr UINT kConstantsStride = 6 * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
static_assert(sizeof(FrameConstants) <= kConstantsStride);
static_assert(offsetof(FrameConstants, lightWarpViews) % 16 == 0);
static_assert(offsetof(FrameConstants, skyboxFogParams) % 16 == 0);
static_assert(offsetof(FrameConstants, causticsOrigin) % 16 == 0);
static_assert(offsetof(FrameConstants, cloudShadowU) % 16 == 0);
// Each slot has the view's constants and a copy for the passes Ray Reconstruction runs at render resolution.
constexpr UINT kConstantsPerSlot = 2;
// Per slot: colour copy, depth, G-buffer normal, flags, light, albedo and self-illumination views, then projector cookies.
constexpr UINT kInputViews = 7;
constexpr UINT kMaxProjectors = 8;
constexpr UINT kViewsPerSlot = kInputViews + kMaxProjectors + static_cast<UINT>(gbuffer::kMaxWaterDraws);
// Bodies of water whose drawn surface the shaders can follow (WaterDraw in shaders/water_body.hlsli, 80 bytes each).
constexpr UINT kMaxDrawnWaterBodies = 256;
constexpr UINT kWaterDrawStride = 80;
constexpr UINT kInvalidView = ~0u;
// Texels on a side of each face of the sky light (kSize in shaders/sky.hlsl).
constexpr UINT kSkyLightSize = 8;
// The procedural sky (shaders/sky_atmosphere.hlsl, shaders/atmosphere.hlsli): its tables, its cubes' faces, its colour
// bands, and the shader's group size.
constexpr UINT kTransmittanceWidth = 256;
constexpr UINT kTransmittanceHeight = 64;
constexpr UINT kMultipleScatteringSize = 32;
constexpr UINT kProceduralSkySize = 256;
constexpr UINT kSkyBands = 32;
constexpr UINT kSkyGroup = 16;
// The daylight table's entries, over the sun's elevation from -90 to 90 degrees (DaylightElevation in
// shaders/sky_atmosphere.hlsl).
constexpr UINT kDaylightEntries = 256;
// Texels on a side of each face of the Milky Way's cube: its finest noise is about 0.3 degrees across.
constexpr UINT kMilkyWaySize = 512;
// Night maps make the map's sun the Moon and draw their sky dark. With MapSun::Auto a map is taken for one while its
// sky above the horizon averages less than kNightSkyShown on screen (linear, after the tonemap scale; sRGB 48 of 255),
// and for a day map again above kNightSkyShown x kNightSkyHysteresis, so it does not flip with the exposure.
constexpr float kNightSkyShown = 0.03f;
constexpr float kNightSkyHysteresis = 1.5f;
// Moonlight comes in over civil twilight, as the sun sinks to 6 degrees below the horizon (USNO): sin(6 degrees).
constexpr float kCivilTwilight = 0.104528f;
// The clear sky is made again once the sun has moved this far (cosine of 0.1 degrees); its disc follows it exactly.
constexpr float kSkySunMoved = 0.9999985f;
// The sun's disc (shaders/sky_draw.hlsl) is as wide as env_sun's sprite looks: its half size m_nSize at 100 units
// (CGlowOverlay::CalcBasis), times the radius at which the default sprite's texture (sprites/light_glow02, 128x128,
// read as sRGB) falls to half its centre's brightness, 0.12 of the sprite's half size. Maps without env_sun get three
// times the real sun's angular radius, which the shadows keep (times Settings::sunShadowSoftness).
constexpr float kSunSpriteDistance = 100.0f;
constexpr float kSunSpriteHalfBrightness = 0.12f;
constexpr float kRealSunRadius = 0.00465f;
constexpr float kSunWithoutSprite = 3.0f;
// Caustics (shaders/caustics.hlsl, kCausticsCells there) cover this many units square around the main view's eye.
constexpr UINT kCausticsCells = 1024;
constexpr float kCausticsRange = 2048.0f;
// The coarse sun map (kCoarseCells there) covers the largest map Source allows.
constexpr UINT kCoarseCausticsCells = 512;
constexpr float kCoarseCausticsRange = 32768.0f;
// Per slot: SamplingConstants, then RestirConstants from kRestirConstantsOffset.
constexpr UINT kSamplingStride = 4096;
constexpr UINT kRestirConstantsOffset = 256;
static_assert(sizeof(RestirConstants) <= kSamplingStride - kRestirConstantsOffset);
// The indirect light volume is read by the lighting passes and by the game's forward draws in their own command lists.
constexpr D3D12_RESOURCE_STATES kVolumeRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
// The clouds (shaders/clouds.hlsl): their noise textures' sizes (kShapeSize, kDetailSize, kWeatherSize there), the
// shadow map's cascades (texels a side, and their widths in units: the fine one 3.25 m a texel over any map Source allows,
// the coarse one sixteen times as wide over the 3D skybox's), the clouds' light (kLightTexels), the cube (as large as the
// procedural sky's, so the sky light reads both at the same texels), and the history's overscan, the share of the
// screen it adds on each side so the sky the next frame turns to is there (the sky is drawn with last frame's clouds).
constexpr UINT kCloudShapeSize = 128;
constexpr UINT kCloudDetailSize = 64;
constexpr UINT kCloudWeatherSize = 512;
// The clouds' shapes over each layer's weather tile (about 40 m a texel for the low layer at Size 1), and their
// distribution's bins per channel (kHistogramBins in shaders/clouds.hlsl).
constexpr UINT kCloudShapesSize = 1024;
constexpr UINT kCloudShapeBins = 256;
// The cirrus' strands (kCirrusSize there) and their mips, down to one texel.
constexpr UINT kCirrusSize = 1024;
constexpr UINT kCirrusMips = 11;
constexpr UINT kCloudShadowSize = 512;
constexpr float kCloudShadowFineWidth = 65536.0f;
constexpr float kCloudShadowCoarseWidth = 16.0f * kCloudShadowFineWidth;
constexpr UINT kCloudLightTexels = 3;
constexpr UINT kCloudCubeSize = kProceduralSkySize;
constexpr float kCloudOverscan = 0.1f;
// The clouds are lit by the sun until it is this far below the horizon (sin(4 degrees)), as seen from the ground: clouds
// up to 15 km high see it a while longer, which the planet's shadow (LightAt in shaders/clouds.hlsl) takes care of.
constexpr float kCloudSunBelow = -0.0698f;
// The froxel volume's columns (shaders/volumetrics.hlsl): at least this many pixels wide, at most this many rows, as
// Frostbite's 8x8 tiles at 900p (Hillaire 2015).
constexpr UINT kFroxelTile = 8;
constexpr UINT kFroxelRows = 160;
// The haze's reach (kMaxDistance in shaders/volumetrics.hlsl) and the farthest the volume follows the map's fog.
constexpr float kHazeDistance = 4096.0f;
constexpr float kMaxFogDistance = 65536.0f;
// What the froxel volume's light was gathered in; its history is kept only while that stays the same.
enum class FroxelMedium { Haze, Fog, Water };
static_assert(sizeof(SamplingConstants) <= kSamplingStride);
// The radiance cache (NVIDIA SHaRC): 2^22 entries of 8 + 16 + 16 bytes (160 MiB), SHaRC's recommended size. Voxels are
// about their distance from the camera over kCacheSceneScale across (a ratio, so Source's units need no conversion);
// RTXPT's advanced sample uses 35. Entries are evicted after 64 frames unseen and radiance is summed in fixed point
// with 1000 steps per unit (SHaRC's recommended starting values). Entries accumulate over up to 5 frames: with 10, a
// switched light's bounce light took about 1 s to settle, as the window compounds over the cached bounces; 5 halves
// that for 0.2 points more frame-to-frame change in denoised indirect light, none measurable on de_investment.
constexpr UINT kCacheCapacity = 1u << 22;
constexpr float kCacheSceneScale = 35.0f;
constexpr UINT kCacheAccumulationFrames = 5;
constexpr UINT kCacheStaleFrames = 64;
constexpr float kCacheRadianceScale = 1000.0f;
// shaders/sharc_update.hlsl traces one path per block of this many pixels on a side.
constexpr UINT kCacheBlock = 4;

// Mirrors Projector in shaders/scene.hlsli.
struct GpuProjector {
    float worldToTexture[4][4];
    float origin[3];
    float farZ;
    float color[3];
    uint32_t cookieView;
    float attenuation[3];
    float fullDistance;
    uint32_t flags;
    uint32_t padding[3];
};
static_assert(sizeof(GpuProjector) == 128);
// kProjectorLocalFlashlight in shaders/scene.hlsli.
constexpr uint32_t kProjectorLocalFlashlight = 0x01;

// GMod's flashlight (CFlashlightEffect::UpdateLightNew) sits r_flashlightoffsety (-20) units below the eye along the
// view's up axis and aims at the point r_flashlightfar ahead of the eye, but within 128 units of a wall it backs away
// along its axis so that its texture does not shrink, and then shines through the player's body. A projector whose axis
// follows the view this closely and passes the eye this near is taken to be it, and moved back to where it is held.
constexpr float kFlashlightAxisAlignment = 0.99f;
constexpr float kFlashlightEyeDistance = 32.0f;
constexpr float kFlashlightMaxPullBack = 160.0f;
// In third person GMod's flashlight sits on the local player's model (the hand holding the weapon), which the view's
// axis does not pass. A projector this close to the middle of the player's body (kPlayerCentreHeight above its feet) is
// taken to be it; it stays where it is.
constexpr float kPlayerCentreHeight = 36.0f;
constexpr float kThirdPersonFlashlightReach = 56.0f;

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// How far the local player's flashlight was pulled back from where it is held, or nothing when the projector is not it.
std::optional<float> FlashlightPullBack(const GpuProjector& projector, const float eye[3], const float viewForward[3])
{
    const float* axisRow = projector.worldToTexture[3];
    const float length = std::sqrt(Dot(axisRow, axisRow));
    if (length <= 0.0f)
        return std::nullopt;
    const float axis[3] = {axisRow[0] / length, axisRow[1] / length, axisRow[2] / length};
    if (Dot(axis, viewForward) < kFlashlightAxisAlignment)
        return std::nullopt;
    const float toEye[3] = {eye[0] - projector.origin[0], eye[1] - projector.origin[1], eye[2] - projector.origin[2]};
    const float along = Dot(toEye, axis);
    const float off = std::sqrt(std::max(Dot(toEye, toEye) - along * along, 0.0f));
    if (along < -1.0f || along > kFlashlightMaxPullBack || off > kFlashlightEyeDistance)
        return std::nullopt;
    return std::max(along, 0.0f);
}

// Moves a projector's apex `distance` along its axis, keeping its orientation and field of view: each texture
// coordinate row is evaluated at the point that far behind.
void AdvanceApex(GpuProjector& projector, float distance)
{
    const float* axisRow = projector.worldToTexture[3];
    const float length = std::sqrt(Dot(axisRow, axisRow));
    const float axis[3] = {axisRow[0] / length, axisRow[1] / length, axisRow[2] / length};
    for (auto& row : projector.worldToTexture)
        row[3] -= distance * Dot(row, axis);
    for (int i = 0; i < 3; ++i)
        projector.origin[i] += distance * axis[i];
}

// Where Source's attenuation c + l/d + q/d² first reaches full strength (1), or 0 when it never falls below it
// or never reaches it.
float FullStrengthDistance(const std::array<float, 3>& attenuation)
{
    const auto [c, l, q] = attenuation;
    if (c >= 1.0f)
        return 0.0f;
    // With x = 1/d: q x² + l x + (c - 1) = 0.
    float x = 0.0f;
    if (q > 0.0f)
        x = (-l + std::sqrt(l * l - 4.0f * q * (c - 1.0f))) / (2.0f * q);
    else if (l > 0.0f)
        x = (1.0f - c) / l;
    return x > 0.0f ? 1.0f / x : 0.0f;
}
constexpr UINT kTraceMask = kWorldMask | kStaticPropMask | kShadowlessPropMask | kLampShellMask | kDynamicMask | kLocalPlayerMask;
// The viewmodel is drawn inside the local player's body, which would shadow it.
constexpr UINT kViewmodelTraceMask = kTraceMask & ~kLocalPlayerMask;
constexpr UINT kFilterSteps[] = {1, 2, 4, 8, 16};
// The viewmodel has no history, so it takes several indirect samples per pixel, stratified over each 2 x 2 block
// (shaders/gi_trace.hlsl): eight directions a block. Eight a pixel cost 2.4 ms on gm_construct at 2560x1440 (2026-11-03)
// for light its denoiser blends over some 30 pixels anyway.
constexpr UINT kViewmodelIndirectSamples = 2;
constexpr UINT kPassCount = static_cast<UINT>(GpuPass::Count);
// A timestamp before the first pass and after each one.
constexpr UINT kMarkers = kPassCount + 1;
constexpr uint32_t kTimingSamples = 120;
constexpr UINT kProbeBytes = 64;
// Luxels the bounce light bake traces per main view or loading screen update, which keeps each part well below the
// GPU timeout.
constexpr UINT kBakeLuxelsPerView = 1 << 14;
constexpr UINT kMaxGroups = 65535;

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

template <typename T>
bool Invert(const T m[4][4], T out[4][4])
{
    const T* a = &m[0][0];
    T inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const T det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < T(1e-20))
        return false;
    for (int i = 0; i < 16; ++i)
        (&out[0][0])[i] = inv[i] / det;
    return true;
}

// Clip space back to points relative to the eye, the point the projection sends to w = 0, worked out in double precision.
// The whole inverse in single precision put far points units off their surfaces, by roundings that changed with every
// jittered matrix: in the 3D skybox (a near plane of 2 units, a camera far from the origin) shadows and reflections
// jumped by many pixels from frame to frame when zoomed in. Relative to the eye nothing large cancels.
bool ClipToEye(const float viewProj[4][4], float clipToEye[4][4], float eye[3])
{
    double m[4][4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            m[r][c] = viewProj[r][c];
    }
    double inverse[4][4];
    if (!Invert(m, inverse) || std::fabs(inverse[3][2]) < 1e-300)
        return false;
    // The inverse maps clip-space (0, 0, 1, 0) to the eye.
    for (int r = 0; r < 4; ++r) {
        const double origin = r < 3 ? inverse[r][2] / inverse[3][2] : 0.0;
        if (r < 3)
            eye[r] = static_cast<float>(origin);
        for (int c = 0; c < 4; ++c)
            clipToEye[r][c] = static_cast<float>(inverse[r][c] - origin * inverse[3][c]);
    }
    return true;
}

// Splits a view-projection into a rigid world-to-view transform (x right, y up, z forward from the eye) and the
// projection after it, both row-major for row vectors (v' = v M) as Ray Reconstruction takes them.
bool SplitViewProjection(const float viewProj[4][4], const float eye[3], float worldToView[16], float viewToClip[16])
{
    auto normalize = [](float v[3]) {
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length < 1e-12f)
            return false;
        for (int i = 0; i < 3; ++i)
            v[i] /= length;
        return true;
    };
    auto dot = [](const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    float axes[3][3];
    for (int i = 0; i < 3; ++i)
        axes[2][i] = viewProj[3][i];
    if (!normalize(axes[2]))
        return false;
    // Right and up from clip x and y, made orthogonal to forward and to each other.
    for (int row = 0; row < 2; ++row) {
        for (int i = 0; i < 3; ++i)
            axes[row][i] = viewProj[row][i];
        for (int other : {2, 0}) {
            if (other == row)
                continue;
            const float d = dot(axes[row], axes[other]);
            for (int i = 0; i < 3; ++i)
                axes[row][i] -= d * axes[other][i];
        }
        if (!normalize(axes[row]))
            return false;
    }
    // Column-vector forms: view = [axes, -axes * eye], its inverse = [axes^T, eye].
    float view[4][4] = {};
    float inverseView[4][4] = {};
    for (int a = 0; a < 3; ++a) {
        for (int i = 0; i < 3; ++i) {
            view[a][i] = axes[a][i];
            inverseView[i][a] = axes[a][i];
        }
        view[a][3] = -dot(axes[a], eye);
        inverseView[a][3] = eye[a];
    }
    view[3][3] = inverseView[3][3] = 1.0f;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float projection = 0.0f;
            for (int k = 0; k < 4; ++k)
                projection += viewProj[i][k] * inverseView[k][j];
            viewToClip[j * 4 + i] = projection;
            worldToView[j * 4 + i] = view[i][j];
        }
    }
    return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

DXGI_FORMAT ColorViewFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
    default: return format;
    }
}

// The sRGB view of a texture the game samples with D3DSAMP_SRGBTEXTURE, or its plain view when there is none.
DXGI_FORMAT SrgbViewFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM_SRGB;
    case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM_SRGB;
    case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM_SRGB;
    default: return ColorViewFormat(format);
    }
}

DXGI_FORMAT DepthStencilFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;
    default: return format;
    }
}

DXGI_FORMAT DepthViewFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES properties = {heap};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer))))
        return nullptr;
    return buffer;
}

float Luminance(const float color[3]) { return 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2]; }

// light_environment's sun, which the game's sky was made for, or nothing on a map without one.
std::optional<std::array<float, 3>> MapSunDirection(const Scene& scene)
{
    const bsp::SunLight& sun = scene.Sun();
    const float length = std::sqrt(sun.direction.x * sun.direction.x + sun.direction.y * sun.direction.y + sun.direction.z * sun.direction.z);
    if (!sun.present || length <= 0.0f)
        return std::nullopt;
    return std::array<float, 3>{sun.direction.x / length, sun.direction.y / length, sun.direction.z / length};
}

// The direction to the sun the menu's sun direction override puts it at.
std::array<float, 3> OverrideSunDirection(const Settings& settings)
{
    constexpr float kRadians = 3.14159265f / 180.0f;
    const float elevation = settings.sunElevation * kRadians;
    const float azimuth = settings.sunAzimuth * kRadians;
    return {std::cos(elevation) * std::cos(azimuth), std::cos(elevation) * std::sin(azimuth), std::sin(elevation)};
}

// 1 - cos(angle), without the cancellation of subtracting a cosine this close to 1.
float ConeSpan(float angle)
{
    const float half = std::sin(0.5f * angle);
    return 2.0f * half * half;
}

// A colour picked in the menu as linear light, by Source's GammaToLinear as VRAD reads light_environment's colours.
std::array<float, 3> LinearColor(const Color& color)
{
    return {std::pow(color[0], 2.2f), std::pow(color[1], 2.2f), std::pow(color[2], 2.2f)};
}

// The sun the sky shows: the menu's override, else env_sun's, else the one GMod's painted sky draws, else the map's.
std::optional<std::array<float, 3>> SkySun(const ViewInputs& inputs, const Scene& scene)
{
    const Settings& settings = GetSettings();
    if (settings.sunDirectionOverride)
        return OverrideSunDirection(settings);
    if (inputs.envSun)
        return inputs.envSun->direction;
    return inputs.paintedSun ? inputs.paintedSun : MapSunDirection(scene);
}

}

HRESULT Lighting::CreatePipelines(std::stop_token stop)
{
    const bool micromaps = m_opacityMicromaps;
    // Devices with the radiance cache run the passes that read it in their variants built with SHaRC (which read it only
    // while it is on); others run the plain passes and have no cache passes.
    const bool cache = m_radianceCacheSupported;
    std::vector<std::function<HRESULT()>> jobs;
    auto compute = [&](std::span<const BYTE> shader, Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline) {
        if (shader.empty())
            return;
        jobs.push_back([this, shader, &pipeline, stop] {
            // The game is closing.
            if (stop.stop_requested())
                return E_ABORT;
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = m_computeRoot.Get();
            desc.CS = {shader.data(), shader.size()};
            return m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
        });
    };
    const std::span<const BYTE> none;
    const std::span<const BYTE> shaders[] = {g_surfaceCS, g_primaryCS, cache ? std::span<const BYTE>(g_probesCacheCS) : g_probesCS, g_aoCS,
        g_volumetricsCS, g_directCS, cache ? std::span<const BYTE>(g_giTraceCacheCS) : g_giTraceCS, g_giTemporalCS, g_giFilterCS,
        cache ? std::span<const BYTE>(g_deferredLightingCacheCS) : g_deferredLightingCS, g_taaCS, g_taaSharpenCS,
        cache ? std::span<const BYTE>(g_reflectionsCacheCS) : g_reflectionsCS, g_reflectionTemporalCS, g_dlssInputsCS, g_dlssResolveCS, g_directWideCS,
        g_swayCS, g_translucentDepthCS, g_viewRectCS, g_bakedBounceCS, g_skyCS, cache ? std::span<const BYTE>(g_sharcUpdateCS) : none,
        cache ? std::span<const BYTE>(g_sharcResolveCS) : none, g_directSunCS, g_restirPdfCS, g_restirPresampleCS, g_restirRegirCS, g_restirInitialCS,
        g_restirTemporalCS, g_restirSpatialCS, g_restirShadeCS, g_nrdInputsCS, g_nrdResolveCS, g_sunShadowCS, g_causticsCS, g_skinCS,
        g_skyAtmosphereCS, g_flareVisibilityCS, g_cloudsCS, g_regenInputsCS, g_regenResolveCS};
    static_assert(std::size(shaders) == static_cast<size_t>(Pass::Count));
    for (size_t i = 0; i < m_pipelines.size(); ++i)
        compute(shaders[i], m_pipelines[i]);
    // Variants of the passes that trace rays whose ray queries let opacity micromaps settle alpha tests.
    const std::pair<Pass, std::span<const BYTE>> micromapShaders[] = {{Pass::Primary, g_primaryOmmCS},
        {Pass::Probes, cache ? std::span<const BYTE>(g_probesCacheOmmCS) : g_probesOmmCS}, {Pass::Ao, g_aoOmmCS}, {Pass::Volumetrics, g_volumetricsOmmCS},
        {Pass::Direct, g_directOmmCS}, {Pass::GiTrace, cache ? std::span<const BYTE>(g_giTraceCacheOmmCS) : g_giTraceOmmCS},
        {Pass::Reflections, cache ? std::span<const BYTE>(g_reflectionsCacheOmmCS) : g_reflectionsOmmCS}, {Pass::DirectWide, g_directWideOmmCS},
        {Pass::SharcUpdate, cache ? std::span<const BYTE>(g_sharcUpdateOmmCS) : none}, {Pass::DirectSun, g_directSunOmmCS},
        {Pass::RestirInitial, g_restirInitialOmmCS}, {Pass::RestirShade, g_restirShadeOmmCS}, {Pass::SunShadow, g_sunShadowOmmCS},
        {Pass::Lighting, cache ? std::span<const BYTE>(g_deferredLightingCacheOmmCS) : g_deferredLightingOmmCS},
        {Pass::TranslucentDepth, g_translucentDepthOmmCS}, {Pass::Baked, g_bakedBounceOmmCS}, {Pass::Caustics, g_causticsOmmCS},
        {Pass::FlareVisibility, g_flareVisibilityOmmCS}};
    for (size_t i = 0; i < std::size(micromapShaders) && micromaps; ++i)
        compute(micromapShaders[i].second, m_micromapPipelines[static_cast<size_t>(micromapShaders[i].first)]);
    for (const bool covered : {false, true}) {
        jobs.push_back([this, covered] {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = m_compositeRoot.Get();
            desc.VS = {g_compositeVS, sizeof(g_compositeVS)};
            desc.PS = covered ? D3D12_SHADER_BYTECODE{g_compositeCoveredPS, sizeof(g_compositeCoveredPS)} : D3D12_SHADER_BYTECODE{g_compositePS, sizeof(g_compositePS)};
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.RasterizerState.DepthClipEnable = TRUE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            return m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(covered ? &m_compositeCoveredPipeline : &m_compositePipeline));
        });
    }
    m_pipelinesTotal = jobs.size();
    return RunInParallel(jobs, &m_pipelinesCreated);
}

bool Lighting::WaitForPipelines()
{
    if (m_pipelinesChecked)
        return !m_failed;
    m_pipelinesChecked = true;
    const auto start = std::chrono::steady_clock::now();
    const bool waited = PreparingPipelines();
    if (m_pipelineBuilder.joinable()) {
        const busy::Scope busy(busy::Reason::Pipelines);
        m_pipelineBuilder.join();
    }
    if (waited)
        log::Info("Lighting: waited {:.0f} ms for the pipeline states", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    if (FAILED(m_pipelinesResult)) {
        log::Info("Lighting: creating the pipeline states failed ({:#010x}); lighting disabled", Hex(m_pipelinesResult));
        m_failed = true;
        m_pipelinesFailed = true;
    }
    return !m_failed;
}

bool Lighting::Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue, DescriptorHeap& descriptors, const RaytracingSupport& support,
    const std::filesystem::path& dlssDirectory)
{
    m_device = device;
    m_queue = queue;
    m_descriptors = &descriptors;
    m_opacityMicromaps = support.opacityMicromaps;
    m_radianceCacheSupported = support.radianceCache;
    HRESULT hr = S_OK;
    for (auto& allocator : m_allocators) {
        if (SUCCEEDED(hr))
            hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    }
    if (SUCCEEDED(hr))
        hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].Get(), nullptr, IID_PPV_ARGS(&m_list));
    if (SUCCEEDED(hr))
        hr = m_list->Close();
    if (SUCCEEDED(hr))
        hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kSlots};
        hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
        hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_dsvHeap));
    }
    if (SUCCEEDED(hr)) {
        m_constants = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, kConstantsStride * kConstantsPerSlot * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
        hr = m_constants ? m_constants->Map(0, nullptr, reinterpret_cast<void**>(&m_constantsMapped)) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        m_sampling = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, kSamplingStride * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
        hr = m_sampling ? m_sampling->Map(0, nullptr, reinterpret_cast<void**>(&m_samplingMapped)) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 256;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_unusedRootUav));
    }
    if (SUCCEEDED(hr)) {
        m_forwardConstants = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, kConstantsStride * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
        hr = m_forwardConstants ? m_forwardConstants->Map(0, nullptr, reinterpret_cast<void**>(&m_forwardConstantsMapped)) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        m_bakeConstants = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, kConstantsStride, D3D12_RESOURCE_STATE_GENERIC_READ);
        hr = m_bakeConstants ? m_bakeConstants->Map(0, nullptr, reinterpret_cast<void**>(&m_bakeConstantsMapped)) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        m_bakeStyles = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, engine::kLightStyles * sizeof(float), D3D12_RESOURCE_STATE_GENERIC_READ);
        float* styles = nullptr;
        hr = m_bakeStyles ? m_bakeStyles->Map(0, nullptr, reinterpret_cast<void**>(&styles)) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) {
            std::fill_n(styles, engine::kLightStyles, 0.0f);
            styles[0] = 1.0f;
            m_bakeStyles->Unmap(0, nullptr);
            m_bakeStylesView = m_descriptors->CreateRawView(m_bakeStyles.Get());
        }
    }
    if (SUCCEEDED(hr)) {
        m_projectors = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, sizeof(GpuProjector) * kMaxProjectors * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
        hr = m_projectors ? m_projectors->Map(0, nullptr, reinterpret_cast<void**>(&m_projectorsMapped)) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        m_waterDraws = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, kWaterDrawStride * kMaxDrawnWaterBodies * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
        hr = m_waterDraws ? m_waterDraws->Map(0, nullptr, reinterpret_cast<void**>(&m_waterDrawsMapped)) : E_OUTOFMEMORY;
    }
    for (UINT slot = 0; slot < kSlots && SUCCEEDED(hr); ++slot) {
        m_waterDrawViews[slot] = m_descriptors->CreateRawView(m_waterDraws.Get(), static_cast<UINT64>(slot) * kWaterDrawStride * kMaxDrawnWaterBodies,
            static_cast<UINT64>(kWaterDrawStride) * kMaxDrawnWaterBodies);
        if (m_waterDrawViews[slot] == DescriptorHeap::kInvalid)
            hr = E_OUTOFMEMORY;
    }
    for (UINT slot = 0; slot < kSlots && SUCCEEDED(hr); ++slot) {
        m_projectorViews[slot] = m_descriptors->Allocate();
        if (m_projectorViews[slot] == DescriptorHeap::kInvalid) {
            hr = E_OUTOFMEMORY;
            break;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = slot * kMaxProjectors * sizeof(GpuProjector) / 4;
        srv.Buffer.NumElements = kMaxProjectors * sizeof(GpuProjector) / 4;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        m_device->CreateShaderResourceView(m_projectors.Get(), &srv, m_descriptors->Cpu(m_projectorViews[slot]));
    }
    if (SUCCEEDED(hr) && GetOptions().devMode) {
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kProbeBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_probeBuffer));
        m_probeReadback = SUCCEEDED(hr) ? CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, kProbeBytes, D3D12_RESOURCE_STATE_COPY_DEST) : nullptr;
        m_probeUav = m_descriptors->Allocate();
        if (SUCCEEDED(hr) && (!m_probeReadback || m_probeUav == DescriptorHeap::kInvalid))
            hr = E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = kProbeBytes / 4;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            m_device->CreateUnorderedAccessView(m_probeBuffer.Get(), nullptr, &uav, m_descriptors->Cpu(m_probeUav));
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * kMarkers};
        hr = m_device->CreateQueryHeap(&desc, IID_PPV_ARGS(&m_timestamps));
    }
    if (SUCCEEDED(hr)) {
        m_timestampReadback = CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, sizeof(uint64_t) * kMarkers * kSlots, D3D12_RESOURCE_STATE_COPY_DEST);
        hr = m_timestampReadback ? m_timestampReadback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint64_t**>(&m_timestampValues))) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        UINT64 frequency = 0;
        hr = m_queue->GetTimestampFrequency(&frequency);
        m_timestampPeriodMs = frequency ? 1000.0 / static_cast<double>(frequency) : 0.0;
    }
    if (SUCCEEDED(hr))
        hr = m_device->CreateRootSignature(0, g_surfaceCS, sizeof(g_surfaceCS), IID_PPV_ARGS(&m_computeRoot));
    const bool micromaps = support.opacityMicromaps;
    const bool cache = m_radianceCacheSupported;
    if (SUCCEEDED(hr))
        hr = m_device->CreateRootSignature(0, g_compositeVS, sizeof(g_compositeVS), IID_PPV_ARGS(&m_compositeRoot));
    if (SUCCEEDED(hr)) {
        D3D12_INDIRECT_ARGUMENT_DESC argument = {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
        D3D12_COMMAND_SIGNATURE_DESC desc = {sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument, 0};
        hr = m_device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&m_dispatchSignature));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 64;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_viewRect.resource));
        if (SUCCEEDED(hr)) {
            m_viewRect.srv = m_descriptors->CreateRawView(m_viewRect.resource.Get());
            m_viewRect.uav = m_descriptors->CreateRawUav(m_viewRect.resource.Get());
            if (m_viewRect.srv == DescriptorHeap::kInvalid || m_viewRect.uav == DescriptorHeap::kInvalid)
                hr = E_OUTOFMEMORY;
        }
    }
    if (SUCCEEDED(hr)) {
        m_slotViews = m_descriptors->Reserve(kSlots * kViewsPerSlot);
        m_rrInputUavs = m_descriptors->Reserve(RrInputCount);
        if (m_slotViews == DescriptorHeap::kInvalid || m_rrInputUavs == DescriptorHeap::kInvalid)
            hr = E_OUTOFMEMORY;
    }
    if (FAILED(hr)) {
        log::Info("Lighting: initialization failed ({:#010x})", Hex(hr));
        return false;
    }
    const UINT* grid = gbuffer::kProbeGrid;
    bool volume = true;
    for (auto& set : m_probeHistory) {
        for (size_t i = 0; i < set.size(); ++i)
            volume = volume && CreateTarget(set[i], grid[0], grid[1], i < 3 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16B16A16_SINT, grid[2]);
    }
    for (Texture& texture : m_probeVolume)
        volume = volume && CreateTarget(texture, grid[0], grid[1], DXGI_FORMAT_R16G16B16A16_FLOAT, grid[2]);
    const UINT indirectUavs = m_descriptors->Reserve(static_cast<UINT>(m_probeIndirect.size()));
    for (size_t i = 0; i < m_probeIndirect.size() && indirectUavs != DescriptorHeap::kInvalid; ++i) {
        m_probeIndirect[i].uav = indirectUavs + static_cast<UINT>(i);
        if (!CreateTarget(m_probeIndirect[i], grid[0], grid[1], DXGI_FORMAT_R16G16B16A16_FLOAT, grid[2]))
            m_probeIndirect[0].resource.Reset();
    }
    if (!volume) {
        log::Info("Lighting: creating the irradiance volume failed; translucent surfaces keep their baked light");
        m_probeVolume = {};
    } else {
        const D3D12_RESOURCE_DESC desc = m_probeVolume[0].resource->GetDesc();
        UINT64 bytes = 0;
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &m_volumeFootprints[0], nullptr, nullptr, &bytes);
        bytes = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        for (size_t i = 1; i < m_volumeFootprints.size(); ++i) {
            m_volumeFootprints[i] = m_volumeFootprints[0];
            m_volumeFootprints[i].Offset = bytes * i;
        }
        for (auto& readback : m_volumeReadback)
            readback = CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, bytes * m_volumeFootprints.size(), D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (!CreateSkyLight())
        log::Info("Lighting: creating the sky light failed; indirect light takes the map's flat sky light");
    if (!CreateTarget(m_causticCounts, 2 * kCausticsCells, kCausticsCells, DXGI_FORMAT_R32_UINT) ||
        !CreateTarget(m_caustics, kCausticsCells, kCausticsCells, DXGI_FORMAT_R16_FLOAT) ||
        !CreateTarget(m_waterSun, kCausticsCells, kCausticsCells, DXGI_FORMAT_R32G32B32A32_FLOAT) ||
        !CreateTarget(m_coarseWaterSun, kCoarseCausticsCells, kCoarseCausticsCells, DXGI_FORMAT_R32G32B32A32_FLOAT)) {
        m_causticCounts = {};
        m_caustics = {};
        m_waterSun = {};
        m_coarseWaterSun = {};
        log::Info("Lighting: creating the caustics' targets failed; water throws no caustics and its light is unshadowed");
    }
    if (!CreateTarget(m_flareVisibility, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        m_flareVisibility = {};
        log::Info("Lighting: creating the lens flare's visibility failed; no lens flare");
    }
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_rtvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_skinned.Initialize(m_device, *m_descriptors);
    m_sway.Initialize(m_device, *m_descriptors);
    for (UINT i = 0; i < RrInputCount; ++i)
        m_rrInputs[i].uav = m_rrInputUavs + i;
    GetStatistics().rayReconstructionAvailable = m_rayReconstruction.Initialize(m_device, dlssDirectory, ModDirectory() / L"logs");
    GetStatistics().fsrAvailable = m_fsr.Initialize(m_device, dlssDirectory);
    GetStatistics().rayRegenerationAvailable = m_rayRegeneration.Initialize(m_device, dlssDirectory);
    GetStatistics().opacityMicromapsAvailable = micromaps;
    GetStatistics().radianceCacheAvailable = cache;
    m_restirAvailable = m_restir.Initialize(m_device, *m_descriptors);
    m_pipelineBuilder = std::jthread([this](std::stop_token stop) {
        const auto start = std::chrono::steady_clock::now();
        m_pipelinesResult = CreatePipelines(stop);
        log::Info("Lighting: created {} pipeline states in {:.0f} ms", m_pipelinesTotal.load(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        m_pipelinesDone.store(true, std::memory_order_release);
    });
    log::Info("Lighting: initialized; creating its pipeline states in the background");
    return true;
}

bool Lighting::CreateTarget(Texture& texture, UINT width, UINT height, DXGI_FORMAT format, UINT depth)
{
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = depth != 0 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = static_cast<UINT16>(depth != 0 ? depth : 1);
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&texture.resource));
    if (FAILED(hr)) {
        log::Info("Lighting: {}x{} target creation failed ({:#010x})", width, height, Hex(hr));
        return false;
    }
    texture.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (texture.srv == DescriptorHeap::kInvalid)
        texture.srv = m_descriptors->Allocate();
    if (texture.uav == DescriptorHeap::kInvalid)
        texture.uav = m_descriptors->Allocate();
    if (texture.srv == DescriptorHeap::kInvalid || texture.uav == DescriptorHeap::kInvalid)
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = format;
    srv.ViewDimension = depth != 0 ? D3D12_SRV_DIMENSION_TEXTURE3D : D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(texture.resource.Get(), &srv, m_descriptors->Cpu(texture.srv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = format;
    uav.ViewDimension = depth != 0 ? D3D12_UAV_DIMENSION_TEXTURE3D : D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture3D.WSize = depth != 0 ? depth : 0;
    m_device->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &uav, m_descriptors->Cpu(texture.uav));
    return true;
}

bool Lighting::EnsureFroxels(const std::array<UINT, 3>& dims)
{
    if (dims == m_froxelDims)
        return true;
    if (dims == m_froxelFailedDims)
        return false;
    // Frames in flight may still read the old volume.
    if (m_froxelIntegrated.resource)
        WaitForGpu();
    m_froxelDims = {};
    m_volumetricHistoryValid = false;
    bool ok = true;
    for (Texture& texture : m_froxelLight)
        ok = ok && CreateTarget(texture, dims[0], dims[1], DXGI_FORMAT_R16G16B16A16_FLOAT, dims[2]);
    ok = ok && CreateTarget(m_froxelProjected, dims[0], dims[1], DXGI_FORMAT_R11G11B10_FLOAT, dims[2]);
    ok = ok && CreateTarget(m_froxelIntegrated, dims[0], dims[1], DXGI_FORMAT_R16G16B16A16_FLOAT, dims[2]);
    if (!ok) {
        for (Texture& texture : m_froxelLight)
            texture.resource.Reset();
        m_froxelProjected.resource.Reset();
        m_froxelIntegrated.resource.Reset();
        m_froxelFailedDims = dims;
        log::Info("Lighting: the {}x{}x{} froxel volume could not be made; volumetric light is off", dims[0], dims[1], dims[2]);
        return false;
    }
    m_froxelDims = dims;
    log::Info("Lighting: volumetric light's froxel volume is {}x{}x{}", dims[0], dims[1], dims[2]);
    return true;
}

void Lighting::WriteWaterDraws(UINT slot, const ViewInputs& inputs, const Scene& scene, UINT firstView, FrameConstants& constants)
{
    const std::span<const GpuWaterBody> bodies = scene.WaterBodies();
    if (bodies.empty() || inputs.waterDraws.empty() || constants.waterBodyCount == 0)
        return;
    struct WaterDraw {
        float bumpS[4];
        float bumpT[4];
        float uvS[4];
        float uvT[4];
        uint32_t normalMap;
        float time;
        uint32_t mapped;
        uint32_t padding;
    };
    static_assert(sizeof(WaterDraw) == kWaterDrawStride);
    WaterDraw* entries = reinterpret_cast<WaterDraw*>(m_waterDrawsMapped + static_cast<size_t>(slot) * kWaterDrawStride * kMaxDrawnWaterBodies);
    const size_t count = std::min<size_t>(bodies.size(), kMaxDrawnWaterBodies);
    for (size_t b = 0; b < count; ++b)
        entries[b] = {{}, {}, {}, {}, kInvalidView, 0.0f, 0, 0};
    for (size_t d = 0; d < inputs.waterDraws.size() && d < gbuffer::kMaxWaterDraws; ++d) {
        ID3D12Resource* normalMap = d < inputs.waterNormalMaps.size() ? inputs.waterNormalMaps[d] : nullptr;
        if (normalMap == nullptr)
            continue;
        const gbuffer::WaterSurfaceDraw& draw = inputs.waterDraws[d];
        const D3D12_RESOURCE_DESC desc = normalMap->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = ColorViewFormat(desc.Format);
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = desc.MipLevels;
        const UINT view = firstView + static_cast<UINT>(d);
        m_device->CreateShaderResourceView(normalMap, &srv, m_descriptors->Cpu(view));
        // The draw's pixel c6 is its material's linear $fogcolor, the same value WaterBodyFor reads for its body.
        for (size_t b = 0; b < count; ++b) {
            bool same = true;
            for (int c = 0; c < 3; ++c)
                same = same && std::fabs(bodies[b].fogColor[c] - draw.fog[c]) < 1e-4f;
            if (same) {
                if (!m_loggedWaterDraw) {
                    m_loggedWaterDraw = true;
                    log::Info("Lighting: water body {} follows its drawn surface (bump rows ({:.3f} {:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f} {:.3f}), "
                              "texture mapping ({:.5f} {:.5f} {:.3f}) ({:.5f} {:.5f} {:.3f}){})", b,
                        draw.bumpRows[0][0], draw.bumpRows[0][1], draw.bumpRows[0][2], draw.bumpRows[0][3], draw.bumpRows[1][0], draw.bumpRows[1][1],
                        draw.bumpRows[1][2], draw.bumpRows[1][3], draw.uvS[0], draw.uvS[1], draw.uvS[3], draw.uvT[0], draw.uvT[1], draw.uvT[3],
                        draw.mapped ? "" : ", not read yet");
                }
                std::copy_n(draw.bumpRows[0].begin(), 4, entries[b].bumpS);
                std::copy_n(draw.bumpRows[1].begin(), 4, entries[b].bumpT);
                std::copy_n(draw.uvS.begin(), 4, entries[b].uvS);
                std::copy_n(draw.uvT.begin(), 4, entries[b].uvT);
                entries[b].mapped = draw.mapped ? 1u : 0u;
                entries[b].normalMap = view;
                entries[b].time = draw.time;
            }
        }
    }
    constants.waterDrawsView = m_waterDrawViews[slot];
}

void Lighting::SetCausticsConstants(FrameConstants& constants) const
{
    constants.causticsView = m_caustics.srv;
    std::copy_n(m_causticsOrigin, 2, constants.causticsOrigin);
    constants.causticsInvSize = m_causticsInvSize;
    constants.waterSunView = m_waterSun.srv;
    constants.coarseWaterSunView = m_coarseWaterSun.srv;
    std::copy_n(m_coarseCausticsOrigin, 2, constants.coarseCausticsOrigin);
    constants.coarseCausticsInvSize = m_coarseCausticsInvSize;
}

void Lighting::RecordCaustics(FrameConstants& constants, const Scene& scene)
{
    m_causticsReady = false;
    const float cell = kCausticsRange / static_cast<float>(kCausticsCells);
    const float originX = std::floor(constants.eyePosition[0] / cell) * cell - 0.5f * kCausticsRange;
    const float originY = std::floor(constants.eyePosition[1] / cell) * cell - 0.5f * kCausticsRange;
    const bool sun = constants.sunEnabled != 0.0f && constants.sunDirection[2] > 0.0f &&
        constants.sunColor[0] + constants.sunColor[1] + constants.sunColor[2] > 0.0f;
    if (!m_caustics.resource || constants.traceMask == 0 || !sun || constants.waterBodyCount == 0)
        return;
    // Without water near the eye the fine square is only cleared: no caustics, and the coarse map's shadows.
    const bool waterNear = scene.WaterWithin(originX, originY, originX + kCausticsRange, originY + kCausticsRange);
    m_causticsOrigin[0] = originX;
    m_causticsOrigin[1] = originY;
    m_causticsInvSize = 1.0f / kCausticsRange;
    const float coarseCell = kCoarseCausticsRange / static_cast<float>(kCoarseCausticsCells);
    m_coarseCausticsOrigin[0] = std::floor(constants.eyePosition[0] / coarseCell) * coarseCell - 0.5f * kCoarseCausticsRange;
    m_coarseCausticsOrigin[1] = std::floor(constants.eyePosition[1] / coarseCell) * coarseCell - 0.5f * kCoarseCausticsRange;
    m_coarseCausticsInvSize = 1.0f / kCoarseCausticsRange;
    // Fine: clear, photons, resolve; coarse (step bit 2): clear, photons (sun map only).
    const D3D12_RESOURCE_BARRIER counted = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_causticCounts.resource.Get()}}};
    Use(m_causticCounts, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Use(m_caustics, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Use(m_waterSun, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Use(m_coarseWaterSun, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const PassArgs fine = {.input1 = std::bit_cast<UINT>(originX), .input2 = std::bit_cast<UINT>(originY), .input3 = std::bit_cast<UINT>(m_causticsInvSize),
        .output0 = m_causticCounts.uav, .output1 = m_caustics.uav, .output2 = m_waterSun.uav};
    const PassArgs coarse = {.input1 = std::bit_cast<UINT>(m_coarseCausticsOrigin[0]), .input2 = std::bit_cast<UINT>(m_coarseCausticsOrigin[1]),
        .input3 = std::bit_cast<UINT>(m_coarseCausticsInvSize), .output0 = m_causticCounts.uav, .output2 = m_coarseWaterSun.uav};
    // The clear and the photons both write the counts and the sun map, so each dispatch waits for the last one's writes
    // to both.
    const auto run = [&](PassArgs args, UINT step, UINT cells, ID3D12Resource* sunMap) {
        args.step = step;
        DispatchGroups(Pass::Caustics, args, cells / 8, cells / 8, 1);
        const D3D12_RESOURCE_BARRIER barriers[] = {counted, {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {sunMap}}}};
        m_list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
    };
    run(fine, 0, kCausticsCells, m_waterSun.resource.Get());
    if (waterNear)
        run(fine, 1, kCausticsCells, m_waterSun.resource.Get());
    Use(m_causticCounts, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PassArgs resolve = fine;
    resolve.input0 = m_causticCounts.srv;
    run(resolve, 2, kCausticsCells, m_waterSun.resource.Get());
    Use(m_causticCounts, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    run(coarse, 4, kCoarseCausticsCells, m_coarseWaterSun.resource.Get());
    run(coarse, 5, kCoarseCausticsCells, m_coarseWaterSun.resource.Get());
    // Forward twins read them in pixel shaders, in the game's command lists (SeenWater).
    Use(m_caustics, kVolumeRead);
    Use(m_waterSun, kVolumeRead);
    Use(m_coarseWaterSun, kVolumeRead);
    SetCausticsConstants(constants);
    m_causticsReady = true;
}

bool Lighting::CreateCube(Texture& texture, UINT size, DXGI_FORMAT format, UINT* faces)
{
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 6;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&texture.resource))))
        return false;
    texture.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    texture.srv = m_descriptors->Allocate();
    texture.uav = m_descriptors->Allocate();
    if (faces != nullptr)
        *faces = m_descriptors->Allocate();
    if (texture.srv == DescriptorHeap::kInvalid || texture.uav == DescriptorHeap::kInvalid || (faces != nullptr && *faces == DescriptorHeap::kInvalid))
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MipLevels = 1;
    m_device->CreateShaderResourceView(texture.resource.Get(), &srv, m_descriptors->Cpu(texture.srv));
    if (faces != nullptr) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray = {};
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = 6;
        m_device->CreateShaderResourceView(texture.resource.Get(), &srv, m_descriptors->Cpu(*faces));
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray.ArraySize = 6;
    m_device->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &uav, m_descriptors->Cpu(texture.uav));
    return true;
}

bool Lighting::CreateSkyLight()
{
    if (CreateCube(m_skyLight, kSkyLightSize, DXGI_FORMAT_R16G16B16A16_FLOAT, nullptr))
        return true;
    m_skyLight = {};
    return false;
}

bool Lighting::MakeAtmosphereTables(UINT slot, D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    // 32-bit tables: through twilight the light they hold falls below what half floats keep.
    Texture daylight;
    const bool created = CreateTarget(m_transmittance, kTransmittanceWidth, kTransmittanceHeight, DXGI_FORMAT_R32G32B32A32_FLOAT) &&
        CreateTarget(m_multipleScattering, kMultipleScatteringSize, kMultipleScatteringSize, DXGI_FORMAT_R32G32B32A32_FLOAT) &&
        CreateTarget(daylight, kDaylightEntries, 1, DXGI_FORMAT_R32G32B32A32_FLOAT) &&
        CreateCube(m_milkyWay, kMilkyWaySize, DXGI_FORMAT_R16G16B16A16_FLOAT, nullptr);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    ComPtr<ID3D12Resource> readback;
    if (created) {
        const D3D12_RESOURCE_DESC desc = daylight.resource->GetDesc();
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        readback = CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    HRESULT hr = readback ? m_allocators[slot]->Reset() : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
        hr = m_list->Reset(m_allocators[slot].Get(), nullptr);
    if (SUCCEEDED(hr)) {
        BindCompute(frameConstants, 0, slot);
        DispatchGroups(Pass::SkyAtmosphere, {.output0 = m_transmittance.uav, .step = 0}, kTransmittanceWidth / kSkyGroup, kTransmittanceHeight / kSkyGroup, 1);
        Use(m_transmittance, kVolumeRead);
        DispatchGroups(Pass::SkyAtmosphere, {.input0 = m_transmittance.srv, .output0 = m_multipleScattering.uav, .step = 1},
            kMultipleScatteringSize / kSkyGroup, kMultipleScatteringSize / kSkyGroup, 1);
        Use(m_multipleScattering, kVolumeRead);
        DispatchGroups(Pass::SkyAtmosphere, {.input0 = m_transmittance.srv, .input1 = m_multipleScattering.srv, .output0 = daylight.uav, .step = 5},
            kDaylightEntries, 1, 1);
        DispatchGroups(Pass::SkyAtmosphere, {.output0 = m_milkyWay.uav, .step = 6}, kMilkyWaySize / kSkyGroup, kMilkyWaySize / kSkyGroup, 6);
        Use(m_milkyWay, kVolumeRead);
        Use(daylight, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const D3D12_TEXTURE_COPY_LOCATION destination = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
        const D3D12_TEXTURE_COPY_LOCATION source = {daylight.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
        m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        hr = m_list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        WaitForGpu();
        void* mapped = nullptr;
        const D3D12_RANGE range = {0, static_cast<SIZE_T>(bytes)};
        hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(hr)) {
            m_daylight.resize(kDaylightEntries);
            std::memcpy(m_daylight.data(), static_cast<const std::byte*>(mapped) + footprint.Offset, kDaylightEntries * sizeof(m_daylight[0]));
            const D3D12_RANGE written = {0, 0};
            readback->Unmap(0, &written);
        }
    }
    for (const UINT view : {daylight.srv, daylight.uav}) {
        if (view != DescriptorHeap::kInvalid)
            m_descriptors->Free(view);
    }
    if (!created || FAILED(hr)) {
        log::Info("Lighting: making the procedural sky's tables failed ({:#010x}); the game's sky and sun stay", Hex(created ? hr : E_OUTOFMEMORY));
        m_atmosphereFailed = true;
        return false;
    }
    m_atmosphereTables = true;
    return true;
}

bool Lighting::ShowsPhysicalSky(const ViewInputs& inputs, const Scene& scene) const
{
    const std::optional<std::array<float, 3>> mapSun = MapSunDirection(scene);
    return GetSettings().physicalSky && m_atmosphereTables && mapSun && (*mapSun)[2] > 0.0f && inputs.skyCube != nullptr;
}

std::array<float, 4> Lighting::DaylightAt(float sunZ) const
{
    const float entries = static_cast<float>(m_daylight.size());
    const float elevation = std::asin(std::clamp(sunZ, -1.0f, 1.0f));
    const float position = std::clamp((elevation / std::numbers::pi_v<float> + 0.5f) * entries - 0.5f, 0.0f, entries - 1.0f);
    const size_t first = std::min(static_cast<size_t>(position), m_daylight.size() - 2);
    const float t = position - static_cast<float>(first);
    std::array<float, 4> result;
    for (size_t i = 0; i < 4; ++i) {
        const float a = m_daylight[first][i];
        const float b = m_daylight[first + 1][i];
        // Light toward a low sun falls by orders of magnitude a degree: it is interpolated in its logarithm.
        result[i] = a > 0.0f && b > 0.0f ? a * std::pow(b / a, t) : a + (b - a) * t;
    }
    return result;
}

std::optional<Lighting::MoonState> Lighting::MoonFor(const ViewInputs& inputs, const Scene& scene) const
{
    const Settings& settings = GetSettings();
    const std::optional<std::array<float, 3>> moon = m_nightSky.Moon();
    if (!moon || !settings.moon || settings.moonPhase <= 0.0f || MapSunIsMoon() || !ShowsPhysicalSky(inputs, scene))
        return std::nullopt;
    const float night = std::clamp(-(*SkySun(inputs, scene))[2] / kCivilTwilight, 0.0f, 1.0f);
    const float irradiance = MoonIrradiance(settings.moonPhase);
    const float moonlit = std::max(settings.moonlitNight - settings.moonlessNight, 0.0f);
    return MoonState{*moon, irradiance, night * moonlit * irradiance / MoonIrradiance(1.0f)};
}

std::optional<Lighting::Daylight> Lighting::DaylightFor(const FrameConstants& constants, const ViewInputs& inputs, const Scene& scene) const
{
    if (!ShowsPhysicalSky(inputs, scene))
        return std::nullopt;
    const std::array<float, 4> map = DaylightAt((*MapSunDirection(scene))[2]);
    if (map[3] <= 0.0f)
        return std::nullopt;
    const std::array<float, 4> sky = DaylightAt((*SkySun(inputs, scene))[2]);
    const float floor = GetSettings().moonlessNight;
    const std::optional<MoonState> moon = MoonFor(inputs, scene);
    const std::array<float, 4> moonlit = moon ? DaylightAt(moon->direction[2]) : std::array<float, 4>{};
    Daylight daylight;
    daylight.sky = (sky[3] + floor * map[3] + (moon ? moon->scale * moonlit[3] : 0.0f)) / ((1.0f + floor) * map[3]);
    if (constants.sunEnabled != 0.0f) {
        const std::array<float, 4> sun = DaylightAt(constants.sunDirection[2]);
        for (size_t i = 0; i < 3; ++i)
            daylight.sun[i] = map[i] > 0.0f ? sun[i] / map[i] : 1.0f;
    } else if (moon && moon->direction[2] > 0.0f && moon->scale > 0.0f) {
        for (size_t i = 0; i < 3; ++i)
            daylight.sun[i] = map[i] > 0.0f ? moon->scale * moonlit[i] / map[i] : 0.0f;
        daylight.moonlight = moon->direction;
    }
    return daylight;
}

bool Lighting::MapSunIsMoon() const
{
    const MapSun mapSun = GetSettings().mapSun;
    return mapSun == MapSun::Moon || (mapSun == MapSun::Auto && m_nightMap.value_or(false));
}

void Lighting::ReadMapSky(UINT slot)
{
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, sizeof(float)};
    if (FAILED(m_mapSkyReadback[slot]->Map(0, &range, &mapped)))
        return;
    float luminance = 0.0f;
    std::memcpy(&luminance, mapped, sizeof(luminance));
    const D3D12_RANGE written = {0, 0};
    m_mapSkyReadback[slot]->Unmap(0, &written);
    // A sky drawn only below the horizon tells nothing.
    if (luminance < 0.0f)
        return;
    const float shown = luminance * m_mapSkyReadbackTonemap[slot];
    const bool night = shown < kNightSkyShown * (m_nightMap.value_or(false) ? kNightSkyHysteresis : 1.0f);
    if (m_nightMap != night)
        log::Info("Lighting: the map's sky looks {} on screen ({:.3f}); with Map's sun on Auto its sun is the {}", night ? "dark" : "bright", shown,
            night ? "Moon" : "Sun");
    m_nightMap = night;
}

bool Lighting::WriteNightSky(UINT slot, FrameConstants& constants)
{
    if (m_nightSkyFailed)
        return false;
    if (!m_nightSkyConstants) {
        m_nightSkyConstants = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, sizeof(NightSkyConstants) * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        const D3D12_RANGE none = {0, 0};
        if (!m_nightSkyConstants || FAILED(m_nightSkyConstants->Map(0, &none, &mapped))) {
            log::Info("Lighting: creating the night sky's constants failed; reflections show no stars, Milky Way or disc");
            m_nightSkyConstants.Reset();
            m_nightSkyFailed = true;
            return false;
        }
        m_nightSkyMapped = static_cast<std::byte*>(mapped);
        for (UINT s = 0; s < kSlots; ++s) {
            m_nightSkyViews[s] = m_descriptors->Allocate();
            if (m_nightSkyViews[s] == DescriptorHeap::kInvalid) {
                log::Info("Lighting: no descriptors left for the night sky's constants; reflections show no stars, Milky Way or disc");
                m_nightSkyFailed = true;
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.FirstElement = s;
            srv.Buffer.NumElements = 1;
            srv.Buffer.StructureByteStride = sizeof(NightSkyConstants);
            m_device->CreateShaderResourceView(m_nightSkyConstants.Get(), &srv, m_descriptors->Cpu(m_nightSkyViews[s]));
        }
    }
    const NightSkyConstants night = PackNightSky(m_skyDraw);
    std::memcpy(m_nightSkyMapped + slot * sizeof(NightSkyConstants), &night, sizeof(night));
    m_nightSkyView = m_nightSkyViews[slot];
    constants.nightSkyView = m_nightSkyView;
    return true;
}

void Lighting::RecordSky(FrameConstants& constants, const ViewInputs& inputs, const Scene& scene, UINT slot)
{
    const Settings& settings = GetSettings();
    const std::optional<std::array<float, 3>> sun = SkySun(inputs, scene);
    const bool sky = sun && ShowsPhysicalSky(inputs, scene);
    // The disc replaces env_sun's sprite, and is the physical sky's sun on maps without one.
    const bool disc = settings.sunDisc && sun && m_atmosphereTables && (inputs.envSun ? inputs.envSun->on : sky);
    m_skyDraw = {};
    m_proceduralSkyReady = false;
    constants.skyCubeView = inputs.skyCube != nullptr ? inputs.skyCubeView : kInvalidView;
    if (settings.physicalSky && !sky && !m_loggedLowMapSun) {
        const std::optional<std::array<float, 3>> mapSun = MapSunDirection(scene);
        if (mapSun && (*mapSun)[2] <= 0.0f) {
            log::Info("Lighting: light_environment's sun is below the horizon; the map keeps its own sky");
            m_loggedLowMapSun = true;
        }
    }
    if ((!sky && !disc) || m_atmosphereFailed)
        return;
    if (sky && !m_proceduralSky.resource) {
        const bool created = CreateCube(m_atmosphereSky, kProceduralSkySize, DXGI_FORMAT_R32G32B32A32_FLOAT, &m_atmosphereSkyFaces) &&
            CreateCube(m_mapSky, kProceduralSkySize, DXGI_FORMAT_R32G32B32A32_FLOAT, &m_mapSkyFaces) &&
            CreateCube(m_moonSky, kProceduralSkySize, DXGI_FORMAT_R32G32B32A32_FLOAT, &m_moonSkyFaces) &&
            CreateTarget(m_skyBands, kSkyBands + 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT) &&
            CreateCube(m_proceduralSky, kProceduralSkySize, DXGI_FORMAT_R16G16B16A16_FLOAT, &m_proceduralSkyFaces) &&
            std::ranges::all_of(m_mapSkyReadback, [&](ComPtr<ID3D12Resource>& readback) {
                readback = CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, D3D12_RESOURCE_STATE_COPY_DEST);
                return readback != nullptr;
            });
        if (!created) {
            m_atmosphereFailed = true;
            m_proceduralSky = {};
            log::Info("Lighting: creating the procedural sky's targets failed; the game's sky and sun stay");
            return;
        }
    }

    // What the 2D sky shows beyond its atmosphere: the sun's disc, the Moon of day maps and the night sky.
    m_skyDraw.sky = sky;
    m_skyDraw.sunDisc = disc;
    m_skyDraw.discIsMoon = MapSunIsMoon();
    m_skyDraw.skyView = m_proceduralSky.srv;
    m_skyDraw.toneCurve = static_cast<uint32_t>(GetSettings().toneCurve);
    m_skyDraw.highlightRolloff = GetSettings().highlightRolloff;
    m_skyDraw.transmittanceView = m_transmittance.srv;
    m_skyDraw.sunDirection = *sun;
    m_skyDraw.sunRadius = kRealSunRadius * kSunWithoutSprite;
    if (inputs.envSun) {
        m_skyDraw.sunColor = inputs.envSun->color;
        m_skyDraw.spriteDirection = inputs.envSun->direction;
        if (inputs.envSun->size > 0.0f)
            m_skyDraw.sunRadius = std::atan(kSunSpriteHalfBrightness * inputs.envSun->size / kSunSpriteDistance);
    }
    m_skyDraw.sunRadius *= settings.sunSize;
    // Set every frame, so a day map's Moon cannot linger into a night map loaded after it.
    const std::optional<MoonState> moon = MoonFor(inputs, scene);
    m_skyDraw.moonDirection = moon ? moon->direction : std::array<float, 3>{};
    m_skyDraw.moonIrradiance = moon ? moon->irradiance : 0.0f;
    m_skyDraw.moonScale = moon ? moon->scale : 0.0f;
    m_skyDraw.moonlessFloor = settings.moonlessNight;
    // The physical sky's stars are drawn against the sky the map's sun lights: the Sun's, or the Moon's on night maps.
    m_skyDraw.mapLight = MapSunIsMoon() ? MoonIrradiance(1.0f) : 1.0f;
    if (const std::optional<std::array<float, 3>> mapSun = MapSunDirection(scene))
        m_skyDraw.mapSunDirection = *mapSun;
    // Earlier stars: the sun's sky counts as dark as it would be with the sun lower, by the clear sky's irradiance there
    // against here.
    const float sunElevation = std::asin(std::clamp((*sun)[2], -1.0f, 1.0f));
    const float here = DaylightAt((*sun)[2])[3];
    const float lower = DaylightAt(std::sin(std::max(sunElevation - settings.earlierStars * std::numbers::pi_v<float> / 180.0f, -0.5f * std::numbers::pi_v<float>)))[3];
    m_skyDraw.starTwilight = here > 0.0f ? std::min(lower / here, 1.0f) : 1.0f;
    if (sky) {
        const uint32_t parts = (settings.stars ? gbuffer::kNightStars : 0u) | (settings.milkyWay ? gbuffer::kNightMilkyWay : 0u) |
            (settings.satellites ? gbuffer::kNightSatellites : 0u) | (settings.shootingStars ? gbuffer::kNightMeteors : 0u);
        if (parts != 0)
            m_nightSky.Write(parts, *sun, settings.shootingStarFrequency, m_skyDraw);
        m_skyDraw.milkyWayView = m_milkyWay.srv;
        m_skyDraw.starDensity = settings.starAmount;
        m_skyDraw.faintestStar = settings.faintestStars;
        m_skyDraw.starBrightness = settings.starBrightness;
        m_skyDraw.twinkleStrength = settings.twinkling ? settings.twinkleStrength : 0.0f;
        m_skyDraw.twinkleSpeed = settings.twinkleSpeed;
        m_skyDraw.milkyWayBrightness = settings.milkyWayBrightness;
    }
    // The 2D sky's motion since the last main view, for TAA and Ray Reconstruction: with the physical sky all of it turns
    // with the sun (its stars, disc, Moon and glow); over the game's own sky only the disc moves, over the directions it
    // covered then or now.
    const std::array<std::array<float, 3>, 3> back = m_nightSky.TurnBack();
    for (size_t row = 0; row < 3; ++row)
        std::copy(back[row].begin(), back[row].end(), constants.skyMotion[row]);
    std::copy(sun->begin(), sun->end(), constants.skyMotionDisc);
    constants.skyMotionDisc[3] = sky ? -2.0f : std::cos(std::min(1.5f * m_skyDraw.sunRadius + m_nightSky.TurnAngle(), std::numbers::pi_v<float>));
    // How much of the disc shows through clouds: the real sun's radiance in the procedural sky's units (or the night map's
    // Moon's, the sky's sun there), its irradiance (the sky's scale times the air's transmittance toward it) over the
    // solid angle it fills.
    if (sky && m_skyScale > 0.0f)
        m_skyDraw.discRadiance = m_skyScale * Luminance(DaylightAt((*sun)[2]).data()) / (std::numbers::pi_v<float> * kRealSunRadius * kRealSunRadius);
    if (!WriteNightSky(slot, constants)) {
        // The procedural sky reads what WriteNightSky writes.
        if (sky) {
            m_atmosphereFailed = true;
            m_skyDraw = {};
        }
        return;
    }
    if (!sky)
        return;

    // The game's draws (shaders/sky_draw.hlsl) read the transmittance and the sky in their own command lists.
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // The clear sky under a sun, made again once the sun has moved.
    const auto clearSky = [&](Texture& cube, const std::array<float, 3>& direction, std::optional<std::array<float, 3>>& madeFor) {
        if (madeFor && direction[0] * (*madeFor)[0] + direction[1] * (*madeFor)[1] + direction[2] * (*madeFor)[2] >= kSkySunMoved)
            return;
        Use(cube, write);
        DispatchGroups(Pass::SkyAtmosphere, {.input0 = m_transmittance.srv, .input1 = m_multipleScattering.srv, .input2 = std::bit_cast<UINT>(direction[0]),
            .input3 = std::bit_cast<UINT>(direction[1]), .input4 = std::bit_cast<UINT>(direction[2]), .output0 = cube.uav, .step = 2},
            kProceduralSkySize / kSkyGroup, kProceduralSkySize / kSkyGroup, 6);
        madeFor = direction;
    };
    clearSky(m_atmosphereSky, *sun, m_atmosphereSun);
    clearSky(m_mapSky, *MapSunDirection(scene), m_mapSkySun);
    if (moon)
        clearSky(m_moonSky, moon->direction, m_moonSkySun);
    Use(m_atmosphereSky, read);
    Use(m_mapSky, read);
    Use(m_moonSky, read);
    const UINT floor = std::bit_cast<UINT>(settings.moonlessNight);
    // The map's sky light before day and night and the clouds scale it, for a game's sky not drawn above the horizon.
    const std::optional<Daylight> daylight = DaylightFor(constants, inputs, scene);
    const float mapSkyLight = Luminance(constants.skyColor) / ((daylight ? daylight->sky : 1.0f) * CloudSkyFactor(inputs, scene));
    Use(m_skyBands, write);
    DispatchGroups(Pass::SkyAtmosphere, {.input0 = inputs.skyFacesView, .input1 = m_mapSkyFaces, .input2 = floor, .input3 = std::bit_cast<UINT>(mapSkyLight),
        .output0 = m_skyBands.uav, .step = 3}, 1, 1, 1);
    Use(m_skyBands, read | D3D12_RESOURCE_STATE_COPY_SOURCE);
    // How bright the game's sky is above the horizon, for the CPU to tell night maps by (ReadMapSky).
    const D3D12_TEXTURE_COPY_LOCATION destination = {m_mapSkyReadback[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        {.PlacedFootprint = {0, {DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}}}};
    const D3D12_TEXTURE_COPY_LOCATION source = {m_skyBands.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
    const D3D12_BOX texel = {kSkyBands, 0, 0, kSkyBands + 1, 1, 1};
    m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, &texel);
    m_mapSkyReadbackTonemap[slot] = inputs.tonemapScale;
    m_mapSkyReadbackPending[slot] = true;
    Use(m_proceduralSky, write);
    DispatchGroups(Pass::SkyAtmosphere, {.input0 = m_atmosphereSkyFaces, .input1 = m_skyBands.srv, .input2 = m_mapSkyFaces, .input3 = constants.nightSkyView,
        .input4 = m_moonSkyFaces, .output0 = m_proceduralSky.uav, .output1 = std::bit_cast<UINT>(GetSettings().sunGlow), .step = 4},
        kProceduralSkySize / kSkyGroup, kProceduralSkySize / kSkyGroup, 6);
    Use(m_proceduralSky, kVolumeRead);
    constants.skyCubeView = m_proceduralSky.srv;
    m_proceduralSkyReady = true;
}

float Lighting::CloudSkyFactor(const ViewInputs& inputs, const Scene& scene) const
{
    return GetSettings().clouds && m_cloudsReady && ShowsPhysicalSky(inputs, scene) ? m_cloudSkyFactor : 1.0f;
}

void Lighting::ReadCloudLight(UINT slot)
{
    void* mapped = nullptr;
    float texels[2][4];
    const D3D12_RANGE range = {0, sizeof(texels)};
    if (FAILED(m_cloudReadback[slot]->Map(0, &range, &mapped)))
        return;
    std::memcpy(texels, mapped, sizeof(texels));
    const D3D12_RANGE written = {0, 0};
    m_cloudReadback[slot]->Unmap(0, &written);
    m_skyScale = std::isfinite(texels[0][3]) ? std::max(texels[0][3], 0.0f) : 0.0f;
    m_cloudSkyFactor = std::isfinite(texels[1][3]) && texels[1][3] > 0.0f ? texels[1][3] : 1.0f;
}

void Lighting::SetCloudConstants(FrameConstants& constants) const
{
    constants.cloudCubeView = m_cloudsReady ? m_cloudCube.srv : kInvalidView;
    constants.cloudShadowView = m_cloudsReady && m_cloudShadowReady ? m_cloudShadow.srv : kInvalidView;
    std::copy_n(m_cloudShadowU, 4, constants.cloudShadowU);
    std::copy_n(m_cloudShadowV, 4, constants.cloudShadowV);
    constants.cloudShadowFineScale = 1.0f / kCloudShadowFineWidth;
    constants.cloudShadowCoarseScale = 1.0f / kCloudShadowCoarseWidth;
    constants.cloudShadowEdge = 0.5f / static_cast<float>(kCloudShadowSize);
}

void Lighting::RecordClouds(FrameConstants& constants, const ViewInputs& inputs, const Scene& scene, UINT slot)
{
    const Settings& settings = GetSettings();
    const auto none = [&] {
        m_cloudsReady = false;
        m_cloudShadowReady = false;
        m_cloudShadowValid = false;
        m_cloudCubeValid = false;
        m_cloudHistoryValid = false;
        m_cloudSkyFactor = 1.0f;
        m_cloudReadbackPending = {};
        SetCloudConstants(constants);
    };
    if (!settings.clouds || !m_proceduralSkyReady || m_cloudsFailed) {
        none();
        return;
    }
    const auto fail = [&](const char* what) {
        log::Info("Lighting: {}; the sky stays clear", what);
        m_cloudsFailed = true;
        none();
    };
    // The cirrus' strands with all their mips: sampled at the pixel's footprint, they do not shimmer far away.
    const auto createStrands = [&] {
        static_assert(std::tuple_size_v<decltype(m_cirrusMipViews)> == kCirrusMips && (kCirrusSize >> (kCirrusMips - 1)) == 1);
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kCirrusSize;
        desc.Height = kCirrusSize;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = kCirrusMips;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&m_cirrusStrands.resource))))
            return false;
        m_cirrusStrands.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_cirrusStrands.srv = m_descriptors->Allocate();
        if (m_cirrusStrands.srv == DescriptorHeap::kInvalid)
            return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = desc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = kCirrusMips;
        m_device->CreateShaderResourceView(m_cirrusStrands.resource.Get(), &srv, m_descriptors->Cpu(m_cirrusStrands.srv));
        for (UINT mip = 0; mip < kCirrusMips; ++mip) {
            m_cirrusMipViews[mip] = m_descriptors->Allocate();
            if (m_cirrusMipViews[mip] == DescriptorHeap::kInvalid)
                return false;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = desc.Format;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Texture2D.MipSlice = mip;
            m_device->CreateUnorderedAccessView(m_cirrusStrands.resource.Get(), nullptr, &uav, m_descriptors->Cpu(m_cirrusMipViews[mip]));
        }
        return true;
    };
    if (!m_cloudConstants) {
        const bool created = createStrands() &&
            CreateTarget(m_cloudShape, kCloudShapeSize, kCloudShapeSize, DXGI_FORMAT_R8G8B8A8_UNORM, kCloudShapeSize) &&
            CreateTarget(m_cloudDetail, kCloudDetailSize, kCloudDetailSize, DXGI_FORMAT_R8G8B8A8_UNORM, kCloudDetailSize) &&
            CreateTarget(m_cloudWeatherMap, kCloudWeatherSize, kCloudWeatherSize, DXGI_FORMAT_R8G8B8A8_UNORM) &&
            CreateTarget(m_cloudShapes, kCloudShapesSize, kCloudShapesSize, DXGI_FORMAT_R16G16B16A16_UNORM) &&
            CreateTarget(m_cloudShapeDistribution, kCloudShapeBins, 4, DXGI_FORMAT_R32_FLOAT) &&
            CreateTarget(m_cloudShadow, 2 * kCloudShadowSize, kCloudShadowSize, DXGI_FORMAT_R16_FLOAT) &&
            CreateTarget(m_cloudLight, kCloudLightTexels, 1, DXGI_FORMAT_R32G32B32A32_FLOAT) &&
            CreateCube(m_cloudCube, kCloudCubeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, &m_cloudCubeFaces) &&
            std::ranges::all_of(m_cloudReadback, [&](ComPtr<ID3D12Resource>& readback) {
                readback = CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, D3D12_RESOURCE_STATE_COPY_DEST);
                return readback != nullptr;
            });
        if (!created)
            return fail("creating the clouds' textures failed");
        m_cloudConstants = CreateBuffer(m_device, D3D12_HEAP_TYPE_UPLOAD, sizeof(CloudConstants) * kSlots, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        const D3D12_RANGE nothing = {0, 0};
        if (!m_cloudConstants || FAILED(m_cloudConstants->Map(0, &nothing, &mapped))) {
            m_cloudConstants.Reset();
            return fail("creating the clouds' constants failed");
        }
        m_cloudConstantsMapped = static_cast<std::byte*>(mapped);
        for (UINT s = 0; s < kSlots; ++s) {
            m_cloudConstantViews[s] = m_descriptors->Allocate();
            if (m_cloudConstantViews[s] == DescriptorHeap::kInvalid)
                return fail("no descriptors left for the clouds' constants");
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.FirstElement = s;
            srv.Buffer.NumElements = 1;
            srv.Buffer.StructureByteStride = sizeof(CloudConstants);
            m_device->CreateShaderResourceView(m_cloudConstants.Get(), &srv, m_descriptors->Cpu(m_cloudConstantViews[s]));
        }
    }
    // The main view's history at the setting's share of the screen, widened by the overscan; the trace a texel of each
    // block of it.
    const float share = settings.cloudResolution == CloudResolution::Full ? 1.0f : settings.cloudResolution == CloudResolution::Half ? 0.5f : 0.25f;
    const std::array<UINT, 2> historySize = {static_cast<UINT>(std::ceil(static_cast<float>(m_width) * share * (1.0f + 2.0f * kCloudOverscan))),
        static_cast<UINT>(std::ceil(static_cast<float>(m_height) * share * (1.0f + 2.0f * kCloudOverscan)))};
    // A texel of each block is traced a frame: a full-sized history in 4 x 4 blocks costs what a half-sized one does in
    // 2 x 2 (a sixteenth of the screen's pixels), and stays sharp.
    const UINT traceBlock = settings.cloudResolution == CloudResolution::Full ? 4 : 2;
    const std::array<UINT, 2> traceSize = {(historySize[0] + traceBlock - 1) / traceBlock, (historySize[1] + traceBlock - 1) / traceBlock};
    if (historySize != m_cloudHistorySize || traceSize != m_cloudTraceSize) {
        // Frames in flight may still draw the sky with the old history.
        WaitForGpu();
        const bool created = CreateTarget(m_cloudTrace, traceSize[0], traceSize[1], DXGI_FORMAT_R16G16B16A16_FLOAT) &&
            CreateTarget(m_cloudTraceDepth, traceSize[0], traceSize[1], DXGI_FORMAT_R32_FLOAT) &&
            CreateTarget(m_cloudHistory[0], historySize[0], historySize[1], DXGI_FORMAT_R16G16B16A16_FLOAT) &&
            CreateTarget(m_cloudHistory[1], historySize[0], historySize[1], DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (!created) {
            m_cloudHistorySize = {};
            return fail("creating the clouds' history failed");
        }
        m_cloudHistorySize = historySize;
        m_cloudTraceSize = traceSize;
        m_cloudHistoryValid = false;
    }

    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (!m_cloudNoiseReady) {
        DispatchGroups(Pass::Clouds, {.output0 = m_cloudShape.uav, .step = 0}, kCloudShapeSize / 8, kCloudShapeSize / 8, kCloudShapeSize);
        DispatchGroups(Pass::Clouds, {.output0 = m_cloudDetail.uav, .step = 1}, kCloudDetailSize / 8, kCloudDetailSize / 8, kCloudDetailSize);
        DispatchGroups(Pass::Clouds, {.output0 = m_cloudWeatherMap.uav, .step = 2}, kCloudWeatherSize / 8, kCloudWeatherSize / 8, 1);
        // The shapes, their distribution, and the shapes spread evenly by it.
        const D3D12_RESOURCE_BARRIER shapesWritten = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_cloudShapes.resource.Get()}}};
        const D3D12_RESOURCE_BARRIER distributionWritten = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE,
            {.UAV = {m_cloudShapeDistribution.resource.Get()}}};
        DispatchGroups(Pass::Clouds, {.output0 = m_cloudShapes.uav, .step = 10}, kCloudShapesSize / 8, kCloudShapesSize / 8, 1);
        m_list->ResourceBarrier(1, &shapesWritten);
        DispatchGroups(Pass::Clouds, {.input1 = m_cloudShapes.uav, .output0 = m_cloudShapeDistribution.uav, .step = 11}, 1, 1, 1);
        m_list->ResourceBarrier(1, &distributionWritten);
        DispatchGroups(Pass::Clouds, {.input1 = m_cloudShapeDistribution.uav, .output0 = m_cloudShapes.uav, .step = 12}, kCloudShapesSize / 8,
            kCloudShapesSize / 8, 1);
        DispatchGroups(Pass::Clouds, {.output0 = m_cirrusMipViews[0], .step = 8}, kCirrusSize / 8, kCirrusSize / 8, 1);
        const D3D12_RESOURCE_BARRIER between = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_cirrusStrands.resource.Get()}}};
        for (UINT mip = 1; mip < kCirrusMips; ++mip) {
            m_list->ResourceBarrier(1, &between);
            const UINT groups = std::max((kCirrusSize >> mip) / 8, 1u);
            DispatchGroups(Pass::Clouds, {.input1 = m_cirrusMipViews[mip - 1], .output0 = m_cirrusMipViews[mip], .step = 9}, groups, groups, 1);
        }
        for (Texture* texture : {&m_cloudShape, &m_cloudDetail, &m_cloudWeatherMap, &m_cloudShapes, &m_cirrusStrands})
            Use(*texture, read);
        m_cloudNoiseReady = true;
    }

    CloudConstants clouds = {};
    const std::array<float, 2> windStep = m_cloudWeather.Advance(settings);
    m_cloudWeather.Write(settings, clouds);
    Statistics& statistics = GetStatistics();
    std::tie(statistics.weather, statistics.nextWeather) = m_cloudWeather.Now();
    if (clouds.layerMask == 0) {
        none();
        return;
    }
    // The clouds are lit by the sun the sky shows until it has set below them, then by the Moon of day maps at its share
    // of the night's sky.
    const std::optional<std::array<float, 3>> sun = SkySun(inputs, scene);
    std::array<float, 3> light = sun.value_or(std::array<float, 3>{0.0f, 0.0f, 1.0f});
    clouds.lightScale = 1.0f;
    if (light[2] < kCloudSunBelow) {
        const std::optional<MoonState> moon = MoonFor(inputs, scene);
        clouds.lightScale = moon ? moon->scale : 0.0f;
        if (moon)
            light = moon->direction;
    }
    std::copy(light.begin(), light.end(), clouds.lightDirection);
    // In the map's sun's hue, the air's colouring taken out of it: the clouds' light passes through the air on its own.
    std::array<float, 3> tint = {1.0f, 1.0f, 1.0f};
    if (constants.sunEnabled != 0.0f) {
        const std::array<float, 4> toSun = DaylightAt(constants.sunDirection[2]);
        float color[3];
        for (size_t i = 0; i < 3; ++i)
            color[i] = toSun[i] > 1e-6f ? constants.sunColor[i] / toSun[i] : 0.0f;
        const float luminance = Luminance(color);
        if (luminance > 0.0f) {
            for (size_t i = 0; i < 3; ++i)
                tint[i] = color[i] / luminance;
        }
    }
    std::copy(tint.begin(), tint.end(), clouds.lightTint);
    clouds.shapeNoiseView = m_cloudShape.srv;
    clouds.detailNoiseView = m_cloudDetail.srv;
    clouds.weatherView = m_cloudWeatherMap.srv;
    clouds.shapesView = m_cloudShapes.srv;
    clouds.cirrusView = m_cirrusStrands.srv;
    clouds.transmittanceView = m_transmittance.srv;
    clouds.skyView = m_proceduralSky.srv;
    clouds.bandsView = m_skyBands.srv;
    clouds.cubeTexels = kCloudCubeSize;
    // The history's clip space: the view's as its G-buffer was drawn (jittered), widened by the overscan; and back from it
    // to directions in double precision, as the sky's draw finds its directions (ClipToEye in shaderapi_pipelines.cpp).
    float rows[4][4];
    std::memcpy(rows, constants.viewProj, sizeof(rows));
    for (int c = 0; c < 4; ++c) {
        rows[0][c] = (rows[0][c] + inputs.jitter[0] * rows[3][c]) / (1.0f + 2.0f * kCloudOverscan);
        rows[1][c] = (rows[1][c] + inputs.jitter[1] * rows[3][c]) / (1.0f + 2.0f * kCloudOverscan);
    }
    const double a[3][3] = {{rows[0][0], rows[0][1], rows[0][2]}, {rows[1][0], rows[1][1], rows[1][2]}, {rows[3][0], rows[3][1], rows[3][2]}};
    const double determinant = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
        a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (std::fabs(determinant) < 1e-30) {
        none();
        return;
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            // The inverse's (r, c) is the cofactor of (c, r) over the determinant.
            const int r1 = (c + 1) % 3;
            const int r2 = (c + 2) % 3;
            const int c1 = (r + 1) % 3;
            const int c2 = (r + 2) % 3;
            clouds.clipToDirection[r][c] = static_cast<float>((a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1]) / determinant);
        }
    }
    std::memcpy(clouds.viewProj, rows, sizeof(rows));
    std::memcpy(clouds.prevViewProj, m_cloudViewProj, sizeof(m_cloudViewProj));
    clouds.historySize[0] = historySize[0];
    clouds.historySize[1] = historySize[1];
    clouds.overscan = kCloudOverscan;
    clouds.historyValid = m_cloudHistoryValid ? 1u : 0u;
    // A texel of each block a frame, all of them in turn: in 2 x 2 blocks diagonally opposite ones one after the other, in
    // 4 x 4 the same within each 2 x 2 step across the block (Bayer's order), so consecutive frames' texels lie apart.
    constexpr UINT kTraceOrder[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    const UINT* coarse = kTraceOrder[m_cloudFrame % 4];
    const UINT* fine = kTraceOrder[(m_cloudFrame / 4) % 4];
    ++m_cloudFrame;
    clouds.traceOffset[0] = traceBlock == 4 ? 2 * coarse[0] + fine[0] : coarse[0];
    clouds.traceOffset[1] = traceBlock == 4 ? 2 * coarse[1] + fine[1] : coarse[1];
    clouds.traceBlock = traceBlock;
    clouds.windStep[0] = windStep[0];
    clouds.windStep[1] = windStep[1];

    // The shadow map over the plane across the sun the map's light comes from, centred on the eye in whole texels of the
    // coarse cascade, so it does not crawl as the eye moves.
    const float* toSun = constants.sunDirection;
    m_cloudShadowReady = constants.sunEnabled != 0.0f && clouds.shadowStrength > 0.0f && (clouds.layerMask != 0);
    bool shadowPlaced = false;
    if (m_cloudShadowReady) {
        // Any axis across the sun: x, or y for a sun along x, with its share along the sun taken out.
        const float axis[3] = {std::fabs(toSun[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(toSun[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
        const float along = Dot(axis, toSun);
        float u[3] = {axis[0] - toSun[0] * along, axis[1] - toSun[1] * along, axis[2] - toSun[2] * along};
        const float length = std::sqrt(Dot(u, u));
        for (float& component : u)
            component /= length;
        const float v[3] = {toSun[1] * u[2] - toSun[2] * u[1], toSun[2] * u[0] - toSun[0] * u[2], toSun[0] * u[1] - toSun[1] * u[0]};
        const float texel = kCloudShadowCoarseWidth / static_cast<float>(kCloudShadowSize);
        const float centreU = std::round(Dot(constants.eyePosition, u) / texel) * texel;
        const float centreV = std::round(Dot(constants.eyePosition, v) / texel) * texel;
        const float placedU[4] = {u[0], u[1], u[2], -centreU};
        const float placedV[4] = {v[0], v[1], v[2], -centreV};
        shadowPlaced = m_cloudShadowValid && std::equal(std::begin(placedU), std::end(placedU), m_cloudShadowU) &&
            std::equal(std::begin(placedV), std::end(placedV), m_cloudShadowV);
        std::copy(std::begin(placedU), std::end(placedU), m_cloudShadowU);
        std::copy(std::begin(placedV), std::end(placedV), m_cloudShadowV);
    } else {
        m_cloudShadowValid = false;
    }
    std::memcpy(m_cloudConstantsMapped + slot * sizeof(CloudConstants), &clouds, sizeof(clouds));
    const UINT cloudView = m_cloudConstantViews[slot];
    // The shadow map reads its placement from `constants`.
    m_cloudsReady = true;
    SetCloudConstants(constants);
    if (m_cloudShadowReady) {
        Use(m_cloudShadow, write);
        // Half its rows a frame, as the clouds move centimetres a frame; all of them where it was placed anew (the eye moved
        // a coarse texel, the sun turned) or first made. Its rows of every frame cost 0.4 ms on gm_construct (2026-11-03).
        const UINT shadowRows = shadowPlaced ? 1u + (m_cloudFrame & 1u) : 0u;
        DispatchGroups(Pass::Clouds, {.input0 = cloudView, .input1 = shadowRows, .output0 = m_cloudShadow.uav, .step = 3}, 2 * kCloudShadowSize / 8,
            (shadowRows == 0 ? kCloudShadowSize : kCloudShadowSize / 2) / 8, 1);
        m_cloudShadowValid = true;
        Use(m_cloudShadow, kVolumeRead);
    }

    // Their light, from last frame's cube; on its way to the CPU for the sky's light and the disc (ReadCloudLight).
    Use(m_cloudLight, write);
    DispatchGroups(Pass::Clouds, {.input0 = cloudView, .input1 = m_cloudCubeValid ? m_cloudCubeFaces : kInvalidView,
        .input2 = m_cloudShadowReady ? m_cloudShadow.srv : kInvalidView, .output0 = m_cloudLight.uav, .step = 4}, 1, 1, 1);
    Use(m_cloudLight, read | D3D12_RESOURCE_STATE_COPY_SOURCE);
    const D3D12_TEXTURE_COPY_LOCATION destination = {m_cloudReadback[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        {.PlacedFootprint = {0, {DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}}}};
    const D3D12_TEXTURE_COPY_LOCATION source = {m_cloudLight.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
    const D3D12_BOX texels = {0, 0, 0, 2, 1, 1};
    m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, &texels);
    m_cloudReadbackPending[slot] = true;

    // The cube: a texel of each 2 x 2 block, or all of it the first time.
    const bool everyTexel = !m_cloudCubeValid;
    const UINT cubeTexels = everyTexel ? kCloudCubeSize : kCloudCubeSize / 2;
    Use(m_cloudCube, write);
    DispatchGroups(Pass::Clouds, {.input0 = cloudView, .input1 = m_cloudLight.srv, .input2 = everyTexel ? 1u : 0u, .output0 = m_cloudCube.uav, .step = 5},
        cubeTexels / 8, cubeTexels / 8, 6);
    Use(m_cloudCube, kVolumeRead);
    m_cloudCubeValid = true;

    // The main view's trace and history. The 3D skybox's distances, when its view drew this frame, tell its pixels from
    // the sky's.
    const bool skyboxDistance = m_skyboxDistanceFrame == inputs.frame && m_skyboxDistance.resource;
    if (skyboxDistance)
        Use(m_skyboxDistance, read);
    Use(m_cloudTrace, write);
    Use(m_cloudTraceDepth, write);
    DispatchGroups(Pass::Clouds, {.input0 = cloudView, .input1 = m_cloudLight.srv, .input2 = skyboxDistance ? m_skyboxDistance.srv : kInvalidView,
        .output0 = m_cloudTrace.uav, .output1 = m_cloudTraceDepth.uav, .step = 6}, (traceSize[0] + 7) / 8, (traceSize[1] + 7) / 8, 1);
    Use(m_cloudTrace, read);
    Use(m_cloudTraceDepth, read);
    const UINT next = 1 - m_cloudHistoryCurrent;
    Use(m_cloudHistory[next], write);
    DispatchGroups(Pass::Clouds, {.input0 = cloudView, .input1 = m_cloudHistory[m_cloudHistoryCurrent].srv, .input2 = m_cloudTrace.srv,
        .input3 = m_cloudTraceDepth.srv, .output0 = m_cloudHistory[next].uav, .step = 7}, (historySize[0] + 7) / 8, (historySize[1] + 7) / 8, 1);
    Use(m_cloudHistory[next], kVolumeRead);
    m_cloudHistoryCurrent = next;
    m_cloudHistoryValid = true;
    std::memcpy(m_cloudViewProj, rows, sizeof(rows));

    // The next frame's sky draws show them.
    m_skyDraw.cloudHistoryView = m_cloudHistory[next].srv;
    m_skyDraw.cloudCubeView = m_cloudCube.srv;
    for (size_t r = 0; r < 4; ++r)
        std::copy(std::begin(rows[r]), std::end(rows[r]), m_skyDraw.cloudViewProj[r].begin());
    m_skyDraw.cloudOverscan = kCloudOverscan;
}

bool Lighting::EnsureTargets(UINT width, UINT height)
{
    if (m_colorCopy && width == m_width && height == m_height)
        return true;
    WaitForGpu();
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    m_colorCopy.Reset();
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_colorCopy))))
        return false;
    bool ok = true;
    for (Texture& texture : m_surface)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R32G32B32A32_FLOAT);
    ok = ok && CreateTarget(m_visibility, width, height, DXGI_FORMAT_R32G32B32A32_UINT);
    ok = ok && CreateTarget(m_motion, width, height, DXGI_FORMAT_R16G16_FLOAT);
    ok = ok && CreateTarget(m_postMotion, width, height, DXGI_FORMAT_R16G16_FLOAT);
    ok = ok && CreateTarget(m_previousDistance, width, height, DXGI_FORMAT_R32_FLOAT);
    ok = ok && CreateTarget(m_pixelFlags, width, height, DXGI_FORMAT_R8_UINT);
    ok = ok && CreateTarget(m_geometricNormal, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_skyboxDistance, width, height, DXGI_FORMAT_R32_FLOAT);
    for (Texture& texture : m_direct)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_directRemainder, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_specular, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_giRaw, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_giHistory)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // Squared luminance of fireflies overflows FP16.
    for (Texture& texture : m_giMoments)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R32G32_FLOAT);
    for (Texture& texture : m_giFiltered)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // Linear (Ray Reconstruction) or encoded shaded colour.
    ok = ok && CreateTarget(m_lit, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_restirDebug, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    ok = ok && CreateTarget(m_selfIllum, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // NRD's inputs and outputs in the formats NRD.hlsli packs them for.
    ok = ok && CreateTarget(m_nrdViewZ, width, height, DXGI_FORMAT_R32_FLOAT);
    ok = ok && CreateTarget(m_nrdNormalRoughness, width, height, DXGI_FORMAT_R10G10B10A2_UNORM);
    for (Texture* texture : {&m_nrdDiffuse, &m_nrdSpecular, &m_nrdDiffuseOut, &m_nrdSpecularOut})
        ok = ok && CreateTarget(*texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_nrdValidation, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    ok = ok && CreateTarget(m_regenMotion, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_regenNormals, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_sunPenumbra, width, height, DXGI_FORMAT_R16_FLOAT);
    ok = ok && CreateTarget(m_sunTranslucency, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    ok = ok && CreateTarget(m_sunShadow, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    m_nrdHistory.valid = false;
    m_sigmaHistory.valid = false;
    ok = ok && CreateTarget(m_reflections, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_aoHistory)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16_FLOAT);
    ok = ok && CreateTarget(m_ao, width, height, DXGI_FORMAT_R16_FLOAT);
    for (Texture& texture : m_reflectionHistory)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_taaHistory)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (Texture& texture : m_viewmodelIndirect)
        ok = ok && CreateTarget(texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_resolved, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    ok = ok && CreateTarget(m_translucentDepth, width, height, DXGI_FORMAT_R32_FLOAT);
    // Ray Reconstruction's inputs use the top-left render-size part of these.
    constexpr DXGI_FORMAT kRrFormats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT};
    static_assert(std::size(kRrFormats) == RrInputCount);
    for (UINT i = 0; i < RrInputCount; ++i)
        ok = ok && CreateTarget(m_rrInputs[i], width, height, kRrFormats[i]);
    ok = ok && CreateTarget(m_rrSpecularAlbedo, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && CreateTarget(m_rrRoughness, width, height, DXGI_FORMAT_R16_FLOAT);
    ok = ok && CreateTarget(m_rrHitDistance, width, height, DXGI_FORMAT_R16_FLOAT);
    ok = ok && CreateTarget(m_rrOutput, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_rayReconstruction.ReleaseFeature();
    m_fsr.Release();
    m_rayRegeneration.Release();
    m_width = width;
    m_height = height;
    m_historyValid = false;
    m_volumetricHistoryValid = false;
    m_lastDenoised = nullptr;
    return ok;
}

bool Lighting::EnsureRadianceCache(bool wanted)
{
    if (wanted == (m_cacheHash != nullptr))
        return wanted;
    WaitForGpu();
    m_descriptors->Free(m_cacheAccumulationUav);
    m_descriptors->Free(m_cacheResolvedUav);
    m_cacheAccumulationUav = DescriptorHeap::kInvalid;
    m_cacheResolvedUav = DescriptorHeap::kInvalid;
    m_cacheHash.Reset();
    m_cacheAccumulation.Reset();
    m_cacheResolved.Reset();
    if (!wanted) {
        log::Info("Lighting: radiance cache freed");
        return false;
    }
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = S_OK;
    for (auto [buffer, stride] : {std::pair(std::addressof(m_cacheHash), 8u), std::pair(std::addressof(m_cacheAccumulation), 16u),
             std::pair(std::addressof(m_cacheResolved), 16u)}) {
        desc.Width = static_cast<UINT64>(kCacheCapacity) * stride;
        if (SUCCEEDED(hr))
            hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(buffer->ReleaseAndGetAddressOf()));
    }
    if (SUCCEEDED(hr)) {
        m_cacheAccumulationUav = m_descriptors->CreateStructuredUav(m_cacheAccumulation.Get(), 16);
        m_cacheResolvedUav = m_descriptors->CreateStructuredUav(m_cacheResolved.Get(), 16);
        if (m_cacheAccumulationUav == DescriptorHeap::kInvalid || m_cacheResolvedUav == DescriptorHeap::kInvalid)
            hr = E_OUTOFMEMORY;
    }
    if (FAILED(hr)) {
        log::Info("Lighting: creating the radiance cache failed ({:#010x}); indirect light is traced without it", Hex(hr));
        m_descriptors->Free(m_cacheAccumulationUav);
        m_descriptors->Free(m_cacheResolvedUav);
        m_cacheAccumulationUav = DescriptorHeap::kInvalid;
        m_cacheResolvedUav = DescriptorHeap::kInvalid;
        m_cacheHash.Reset();
        m_cacheAccumulation.Reset();
        m_cacheResolved.Reset();
        return false;
    }
    m_cacheClear = true;
    log::Info("Lighting: radiance cache of {} entries ({} MiB)", kCacheCapacity, static_cast<UINT64>(kCacheCapacity) * 40 / (1024 * 1024));
    return true;
}

void Lighting::BindCompute(D3D12_GPU_VIRTUAL_ADDRESS frameConstants, D3D12_GPU_VIRTUAL_ADDRESS scene, UINT slot)
{
    ID3D12DescriptorHeap* heaps[] = {m_descriptors->Heap()};
    m_list->SetDescriptorHeaps(1, heaps);
    m_list->SetComputeRootSignature(m_computeRoot.Get());
    m_list->SetComputeRootConstantBufferView(0, frameConstants);
    m_list->SetComputeRootShaderResourceView(1, scene);
    const D3D12_GPU_VIRTUAL_ADDRESS sampling = m_sampling->GetGPUVirtualAddress() + slot * kSamplingStride;
    m_list->SetComputeRootConstantBufferView(3, sampling);
    m_list->SetComputeRootUnorderedAccessView(4, (m_cacheHash ? m_cacheHash : m_unusedRootUav)->GetGPUVirtualAddress());
    m_list->SetComputeRootConstantBufferView(5, sampling + kRestirConstantsOffset);
    m_list->SetComputeRootUnorderedAccessView(6, (m_restirAvailable ? m_restir.RisBuffer() : m_unusedRootUav.Get())->GetGPUVirtualAddress());
    m_list->SetComputeRootUnorderedAccessView(7, (m_restir.Reservoirs() ? m_restir.Reservoirs() : m_unusedRootUav.Get())->GetGPUVirtualAddress());
    m_list->SetComputeRootShaderResourceView(8, (m_restirAvailable ? m_restir.NeighborOffsets() : m_sampling.Get())->GetGPUVirtualAddress());
}

bool Lighting::EnsureNrd(DenoiserType type)
{
    if (m_nrdFailed)
        return false;
    const NrdDenoiser::Method method = type == DenoiserType::Reblur ? NrdDenoiser::Method::Reblur : NrdDenoiser::Method::Relax;
    if (m_nrd.Matches(method, m_width, m_height))
        return true;
    WaitForGpu();
    m_nrdHistory.valid = false;
    if (!m_nrd.Initialize(m_device, *m_descriptors, method, m_width, m_height)) {
        log::Info("NRD: the built-in denoiser is used");
        m_nrdFailed = true;
        return false;
    }
    return true;
}

bool Lighting::EnsureSigma()
{
    if (m_sigmaFailed)
        return false;
    if (m_sigma.Matches(NrdDenoiser::Method::Sigma, m_width, m_height))
        return true;
    WaitForGpu();
    m_sigmaHistory.valid = false;
    if (!m_sigma.Initialize(m_device, *m_descriptors, NrdDenoiser::Method::Sigma, m_width, m_height)) {
        log::Info("NRD: the sun's shadows stay hard");
        m_sigmaFailed = true;
        return false;
    }
    return true;
}

nrd::CommonSettings Lighting::NrdCommonSettings(const FrameConstants& constants, const ViewInputs& inputs, NrdHistory& history)
{
    // The matrices are the ones Ray Reconstruction takes: row-major for row vectors, which is NRD's column-major layout
    // for column vectors. Motion is in pixels; NRD's is in texture coordinates.
    nrd::CommonSettings common = {};
    float worldToView[16];
    float viewToClip[16];
    SplitViewProjection(constants.viewProj, constants.eyePosition, worldToView, viewToClip);
    const bool restart = !history.valid || constants.historyValid == 0;
    std::memcpy(common.worldToViewMatrix, worldToView, sizeof(worldToView));
    std::memcpy(common.viewToClipMatrix, viewToClip, sizeof(viewToClip));
    std::memcpy(common.worldToViewMatrixPrev, restart ? worldToView : history.worldToView, sizeof(worldToView));
    std::memcpy(common.viewToClipMatrixPrev, restart ? viewToClip : history.viewToClip, sizeof(viewToClip));
    common.motionVectorScale[0] = 1.0f / static_cast<float>(m_gridWidth);
    common.motionVectorScale[1] = 1.0f / static_cast<float>(m_gridHeight);
    common.motionVectorScale[2] = 0.0f;
    common.cameraJitter[0] = inputs.jitterX;
    common.cameraJitter[1] = inputs.jitterY;
    common.cameraJitterPrev[0] = restart ? inputs.jitterX : history.jitter[0];
    common.cameraJitterPrev[1] = restart ? inputs.jitterY : history.jitter[1];
    // Under FSR the passes run over the render size's corner of output-sized textures (NRD's dynamic resolution).
    common.resourceSize[0] = common.resourceSizePrev[0] = static_cast<uint16_t>(m_width);
    common.resourceSize[1] = common.resourceSizePrev[1] = static_cast<uint16_t>(m_height);
    common.rectSize[0] = static_cast<uint16_t>(m_gridWidth);
    common.rectSize[1] = static_cast<uint16_t>(m_gridHeight);
    common.rectSizePrev[0] = restart ? common.rectSize[0] : history.rectSize[0];
    common.rectSizePrev[1] = restart ? common.rectSize[1] : history.rectSize[1];
    common.frameIndex = history.frame++;
    common.accumulationMode = restart ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE;
    history.valid = true;
    std::memcpy(history.worldToView, worldToView, sizeof(worldToView));
    std::memcpy(history.viewToClip, viewToClip, sizeof(viewToClip));
    history.jitter[0] = inputs.jitterX;
    history.jitter[1] = inputs.jitterY;
    history.rectSize[0] = common.rectSize[0];
    history.rectSize[1] = common.rectSize[1];
    return common;
}

std::pair<UINT, UINT> Lighting::RecordSunShadow(const FrameConstants& constants, const ViewInputs& inputs, bool denoise, UINT slot,
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants, D3D12_GPU_VIRTUAL_ADDRESS scene)
{
    Texture* written[] = {&m_sunPenumbra, &m_sunTranslucency, &m_nrdViewZ, &m_nrdNormalRoughness};
    for (Texture* texture : written)
        Use(*texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Step 1: SIGMA denoises the shadows and needs each ray's nearest occluder; without it only the light is read.
    Dispatch(Pass::SunShadow, {.output0 = m_sunPenumbra.uav, .output1 = m_sunTranslucency.uav, .output2 = m_nrdViewZ.uav, .output3 = m_nrdNormalRoughness.uav,
        .step = denoise ? 1u : 0u});
    for (Texture* texture : written)
        Use(*texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!denoise) {
        m_sigmaHistory.valid = false;
        return {m_sunTranslucency.srv, 0};
    }
    if (!EnsureSigma())
        return {kInvalidView, 0};
    const nrd::CommonSettings common = NrdCommonSettings(constants, inputs, m_sigmaHistory);
    NrdDenoiser::Resources resources = {};
    const auto provide = [&](nrd::ResourceType type, Texture& texture) { resources[static_cast<size_t>(type)] = {texture.resource.Get(), &texture.state}; };
    provide(nrd::ResourceType::IN_MV, m_motion);
    provide(nrd::ResourceType::IN_NORMAL_ROUGHNESS, m_nrdNormalRoughness);
    provide(nrd::ResourceType::IN_VIEWZ, m_nrdViewZ);
    provide(nrd::ResourceType::IN_PENUMBRA, m_sunPenumbra);
    provide(nrd::ResourceType::IN_TRANSLUCENCY, m_sunTranslucency);
    provide(nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY, m_sunShadow);
    const bool recorded = m_sigma.Denoise(m_list.Get(), slot, common, resources, constants.sunDirection);
    BindCompute(frameConstants, scene, slot);
    if (!recorded) {
        log::Info("NRD: recording SIGMA's dispatches failed; the sun's shadows stay hard");
        m_sigmaHistory.valid = false;
        m_sigmaFailed = true;
        return {kInvalidView, 0};
    }
    Use(m_sunShadow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return {m_sunShadow.srv, 1};
}

bool Lighting::RecordNrd(const FrameConstants& constants, const ViewInputs& inputs, UINT slot, UINT current, D3D12_GPU_VIRTUAL_ADDRESS frameConstants,
    D3D12_GPU_VIRTUAL_ADDRESS scene)
{
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Texture* inputs_[] = {&m_nrdViewZ, &m_nrdNormalRoughness, &m_nrdDiffuse, &m_nrdSpecular};
    for (Texture* texture : inputs_)
        Use(*texture, write);
    for (Texture* texture : {&m_giRaw, &m_reflections, &m_rrRoughness, &m_rrSpecularAlbedo})
        Use(*texture, read);
    const UINT method = m_nrd.CurrentMethod() == NrdDenoiser::Method::Reblur ? 1u : 0u;
    Dispatch(Pass::NrdInputs, {.input0 = m_giRaw.srv, .input1 = m_reflections.srv, .input2 = m_rrRoughness.srv, .input3 = m_rrSpecularAlbedo.srv,
        .output0 = m_nrdViewZ.uav, .output1 = m_nrdNormalRoughness.uav, .output2 = m_nrdDiffuse.uav, .output3 = m_nrdSpecular.uav, .step = method});
    for (Texture* texture : inputs_)
        Use(*texture, read);

    nrd::CommonSettings common = NrdCommonSettings(constants, inputs, m_nrdHistory);
    common.enableValidation = constants.debugView == static_cast<uint32_t>(DebugView::DenoiserValidation);
    NrdDenoiser::Resources resources = {};
    const auto provide = [&](nrd::ResourceType type, Texture& texture) { resources[static_cast<size_t>(type)] = {texture.resource.Get(), &texture.state}; };
    provide(nrd::ResourceType::IN_MV, m_motion);
    provide(nrd::ResourceType::IN_NORMAL_ROUGHNESS, m_nrdNormalRoughness);
    provide(nrd::ResourceType::IN_VIEWZ, m_nrdViewZ);
    provide(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, m_nrdDiffuse);
    provide(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, m_nrdSpecular);
    provide(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, m_nrdDiffuseOut);
    provide(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, m_nrdSpecularOut);
    provide(nrd::ResourceType::OUT_VALIDATION, m_nrdValidation);
    const bool recorded = m_nrd.Denoise(m_list.Get(), slot, common, resources);
    BindCompute(frameConstants, scene, slot);
    if (!recorded) {
        log::Info("NRD: recording its dispatches failed; the built-in denoiser is used");
        m_nrdHistory.valid = false;
        m_nrdFailed = true;
        return false;
    }
    Use(m_nrdDiffuseOut, read);
    Use(m_nrdSpecularOut, read);
    Use(m_giFiltered[0], write);
    Use(m_reflectionHistory[current], write);
    Dispatch(Pass::NrdResolve, {.input0 = m_nrdDiffuseOut.srv, .input1 = m_nrdSpecularOut.srv, .input2 = m_rrSpecularAlbedo.srv, .output0 = m_giFiltered[0].uav,
        .output1 = m_reflectionHistory[current].uav, .step = method});
    Use(m_giFiltered[0], read);
    Use(m_reflectionHistory[current], read);
    if (common.enableValidation)
        Use(m_nrdValidation, read);
    return true;
}

bool Lighting::RecordRayRegeneration(const FrameConstants& constants, const ViewInputs& inputs, ID3D12Resource* albedo, UINT slot, UINT current,
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants, D3D12_GPU_VIRTUAL_ADDRESS scene)
{
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Texture* prepared[] = {&m_nrdViewZ, &m_regenNormals, &m_nrdDiffuse, &m_nrdSpecular, &m_regenMotion};
    for (Texture* texture : prepared)
        Use(*texture, write);
    for (Texture* texture : {&m_giRaw, &m_reflections, &m_rrRoughness, &m_rrSpecularAlbedo, &m_previousDistance, &m_motion})
        Use(*texture, read);
    Dispatch(Pass::RegenInputs, {.input0 = m_giRaw.srv, .input1 = m_reflections.srv, .input2 = m_rrRoughness.srv, .input3 = m_rrSpecularAlbedo.srv,
        .input4 = m_previousDistance.srv, .output0 = m_nrdViewZ.uav, .output1 = m_regenNormals.uav, .output2 = m_nrdDiffuse.uav,
        .output3 = m_nrdSpecular.uav, .output4 = m_regenMotion.uav});
    for (Texture* texture : prepared)
        Use(*texture, read);
    Use(m_nrdDiffuseOut, write);
    Use(m_nrdSpecularOut, write);

    float worldToView[16];
    float viewToClip[16];
    SplitViewProjection(constants.viewProj, constants.eyePosition, worldToView, viewToClip);
    const bool reset = constants.historyValid == 0;
    m_regenFrame = reset ? 0 : m_regenFrame + 1;
    FsrRayRegeneration::Inputs regen = {m_nrdViewZ.resource.Get(), m_regenMotion.resource.Get(), m_regenNormals.resource.Get(), albedo,
        m_rrSpecularAlbedo.resource.Get(), m_nrdDiffuse.resource.Get(), m_nrdSpecular.resource.Get(), m_nrdDiffuseOut.resource.Get(),
        m_nrdSpecularOut.resource.Get(), worldToView, viewToClip, inputs.jitterX, inputs.jitterY,
        {m_prevEye[0] - constants.eyePosition[0], m_prevEye[1] - constants.eyePosition[1], m_prevEye[2] - constants.eyePosition[2]}, 65536.0f, m_gridWidth,
        m_gridHeight, m_width, m_height, m_regenFrame, reset};
    const bool denoised = m_rayRegeneration.Evaluate(m_list.Get(), regen);
    // FSR leaves its own descriptor heaps bound.
    BindCompute(frameConstants, scene, slot);
    if (!denoised)
        return false;
    Use(m_nrdDiffuseOut, read);
    Use(m_nrdSpecularOut, read);
    Use(m_giFiltered[0], write);
    Use(m_reflectionHistory[current], write);
    Dispatch(Pass::RegenResolve, {.input0 = m_nrdDiffuseOut.srv, .input1 = m_nrdSpecularOut.srv, .input2 = m_rrSpecularAlbedo.srv,
        .output0 = m_giFiltered[0].uav, .output1 = m_reflectionHistory[current].uav});
    Use(m_giFiltered[0], read);
    Use(m_reflectionHistory[current], read);
    return true;
}

void Lighting::RecordRestir(RestirMode mode, bool debugImage)
{
    // Root descriptors are not bounds-checked: bind the buffers Prepare may have just made (BindCompute ran before).
    m_list->SetComputeRootUnorderedAccessView(6, m_restir.RisBuffer()->GetGPUVirtualAddress());
    m_list->SetComputeRootUnorderedAccessView(7, m_restir.Reservoirs()->GetGPUVirtualAddress());
    ID3D12Resource* pdf = m_restir.Pdf();
    const D3D12_RESOURCE_BARRIER pdfBarrier = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {pdf}}};
    const D3D12_RESOURCE_BARRIER risBarrier = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_restir.RisBuffer()}}};
    const D3D12_RESOURCE_BARRIER reservoirBarrier = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_restir.Reservoirs()}}};
    D3D12_RESOURCE_STATES& pdfState = m_restir.PdfState();
    if (pdfState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        const D3D12_RESOURCE_BARRIER barrier = Transition(pdf, pdfState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_list->ResourceBarrier(1, &barrier);
        pdfState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    // Each light's power, then the sums of each mip.
    for (UINT mip = 0; mip < m_restir.PdfMips(); ++mip) {
        if (mip > 0)
            m_list->ResourceBarrier(1, &pdfBarrier);
        const UINT width = std::max(m_restir.PdfWidth() >> mip, 1u);
        const UINT height = std::max(m_restir.PdfHeight() >> mip, 1u);
        DispatchGroups(Pass::RestirPdf, {.step = mip}, (width + 7) / 8, (height + 7) / 8, 1);
    }
    const D3D12_RESOURCE_BARRIER readable = Transition(pdf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_list->ResourceBarrier(1, &readable);
    pdfState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_list->ResourceBarrier(1, &risBarrier);
    DispatchGroups(Pass::RestirPresample, {}, Restir::kTileSize * Restir::kTileCount / 256, 1, 1);
    m_list->ResourceBarrier(1, &risBarrier);
    DispatchGroups(Pass::RestirRegir, {}, (m_restir.RegirLightSlots() + 255) / 256, 1, 1);
    m_list->ResourceBarrier(1, &risBarrier);
    m_list->ResourceBarrier(1, &reservoirBarrier);
    Dispatch(Pass::RestirInitial, {});
    m_list->ResourceBarrier(1, &reservoirBarrier);
    if (mode == RestirMode::Reuse) {
        Dispatch(Pass::RestirTemporal, {});
        m_list->ResourceBarrier(1, &reservoirBarrier);
        Dispatch(Pass::RestirSpatial, {});
        m_list->ResourceBarrier(1, &reservoirBarrier);
    }
    if (debugImage)
        Use(m_restirDebug, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Dispatch(Pass::RestirShade, {.output0 = m_directRemainder.uav, .output1 = m_specular.uav, .output2 = debugImage ? m_restirDebug.uav : kInvalidView});
    m_list->ResourceBarrier(1, &reservoirBarrier);
    if (debugImage)
        Use(m_restirDebug, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void Lighting::Use(Texture& texture, D3D12_RESOURCE_STATES state)
{
    if (texture.state == state)
        return;
    D3D12_RESOURCE_BARRIER barrier = Transition(texture.resource.Get(), texture.state, state);
    m_list->ResourceBarrier(1, &barrier);
    texture.state = state;
}

void Lighting::SetPipeline(Pass pass)
{
    ID3D12PipelineState* micromapPipeline = m_micromapPipelines[static_cast<size_t>(pass)].Get();
    m_list->SetPipelineState(m_useMicromaps && micromapPipeline != nullptr ? micromapPipeline : m_pipelines[static_cast<size_t>(pass)].Get());
}

void Lighting::Dispatch(Pass pass, const PassArgs& args)
{
    static_assert(sizeof(PassArgs) == 12 * sizeof(UINT));
    SetPipeline(pass);
    m_list->SetComputeRoot32BitConstants(2, sizeof(PassArgs) / sizeof(UINT), &args, 0);
    if (m_rectDispatch)
        m_list->ExecuteIndirect(m_dispatchSignature.Get(), 1, m_viewRect.resource.Get(), 16, nullptr, 0);
    else
        m_list->Dispatch((m_gridWidth + 7) / 8, (m_gridHeight + 7) / 8, 1);
}

void Lighting::DispatchGroups(Pass pass, const PassArgs& args, UINT x, UINT y, UINT z)
{
    SetPipeline(pass);
    m_list->SetComputeRoot32BitConstants(2, sizeof(PassArgs) / sizeof(UINT), &args, 0);
    m_list->Dispatch(x, y, z);
}

bool Lighting::EnsureDepthPipeline(DXGI_FORMAT format)
{
    if (m_depthPipeline && m_depthPipelineFormat == format)
        return true;
    m_depthPipeline.Reset();
    m_depthPipelineFormat = format;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_compositeRoot.Get();
    desc.VS = {g_compositeVS, sizeof(g_compositeVS)};
    desc.PS = {g_compositeDepthPS, sizeof(g_compositeDepthPS)};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.DSVFormat = format;
    desc.SampleDesc.Count = 1;
    if (FAILED(m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_depthPipeline)))) {
        log::Info("Lighting: creating the depth pipeline for format {} failed; translucent surfaces keep the jittered depth", static_cast<int>(format));
        return false;
    }
    return true;
}

void Lighting::UpdateSway(const Scene& scene, UINT slot, const gbuffer::SwayClock& clock, const float (&eye)[3], D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    if (m_sway.Empty() || !clock.known)
        return;
    // The game's sway time runs with real time; it is extrapolated for frames that drew no swaying model.
    LARGE_INTEGER now;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float time = clock.time + static_cast<float>(static_cast<double>(now.QuadPart - clock.ticks) / static_cast<double>(frequency.QuadPart));
    if (!m_swayKnown) {
        m_swayTime = time;
        std::copy_n(clock.wind, 2, m_swayWind);
        log::Info("Lighting: swaying props follow the game's wind ({:.2f} {:.2f}) from time {:.2f}", clock.wind[0], clock.wind[1], time);
    }
    if (const std::optional<SwayingProps::Jobs> jobs = m_sway.BeginUpdate(scene, slot, eye, time, m_swayTime, clock.wind, m_swayWind)) {
        // The pass traces nothing, so the scene's root slot holds any valid address.
        BindCompute(frameConstants, frameConstants, slot);
        DispatchGroups(Pass::Sway, {.input0 = jobs->jobsView, .input1 = jobs->materialsView, .output0 = jobs->currentUav, .output1 = jobs->previousUav},
            jobs->groupsX, jobs->count, 1);
        m_sway.FinishUpdate(m_list.Get(), scene);
    }
    m_swayKnown = true;
    m_swayTime = time;
    std::copy_n(clock.wind, 2, m_swayWind);
}

void Lighting::UpdateSkinned(const Scene& scene, const std::vector<SkinnedEntity>& skinned, UINT slot, D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    if (const std::optional<SkinnedCasters::Jobs> jobs = m_skinned.BeginUpdate(m_list.Get(), scene, skinned)) {
        // The pass traces nothing, so the scene's root slot holds any valid address.
        BindCompute(frameConstants, frameConstants, slot);
        DispatchGroups(Pass::Skin, {.input0 = jobs->jobsView}, jobs->groupsX, jobs->count, 1);
    }
    m_skinned.FinishUpdate(m_list.Get(), scene, m_frameInstances);
}

void Lighting::PublishVolume(UINT slot)
{
    const UINT* grid = gbuffer::kProbeGrid;
    const size_t texels = static_cast<size_t>(grid[0]) * grid[1] * grid[2];
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, static_cast<SIZE_T>(m_volumeReadback[slot]->GetDesc().Width)};
    if (FAILED(m_volumeReadback[slot]->Map(0, &range, &mapped)))
        return;
    // Interleaved per texel: red, green and blue RGBA16F texels, each four halves.
    m_volumeTexels.resize(texels * 12);
    for (size_t channel = 0; channel < m_volumeFootprints.size(); ++channel) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = m_volumeFootprints[channel];
        const auto* base = static_cast<const std::byte*>(mapped) + footprint.Offset;
        for (UINT z = 0; z < grid[2]; ++z) {
            for (UINT y = 0; y < grid[1]; ++y) {
                const std::byte* row = base + (static_cast<size_t>(z) * footprint.Footprint.Height + y) * footprint.Footprint.RowPitch;
                for (UINT x = 0; x < grid[0]; ++x)
                    std::memcpy(m_volumeTexels.data() + ((static_cast<size_t>(z) * grid[1] + y) * grid[0] + x) * 12 + channel * 4, row + x * 8, 8);
            }
        }
    }
    const D3D12_RANGE written = {0, 0};
    m_volumeReadback[slot]->Unmap(0, &written);
    light_queries::Publish(m_volumeReadbackOrigin[slot], {grid[0], grid[1], grid[2]}, gbuffer::kProbeSpacing, m_volumeTexels);
}

void Lighting::WaitForGpu()
{
    if (!m_fence)
        return;
    m_queue->Signal(m_fence.Get(), ++m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void Lighting::FillSceneConstants(FrameConstants& constants, const Scene& scene)
{
    const bsp::SunLight& sun = scene.Sun();
    constants.sunEnabled = sun.present ? 1.0f : 0.0f;
    float length = std::sqrt(sun.direction.x * sun.direction.x + sun.direction.y * sun.direction.y + sun.direction.z * sun.direction.z);
    if (length > 0.0f) {
        constants.sunDirection[0] = sun.direction.x / length;
        constants.sunDirection[1] = sun.direction.y / length;
        constants.sunDirection[2] = sun.direction.z / length;
    }
    constants.sunColor[0] = sun.color.x;
    constants.sunColor[1] = sun.color.y;
    constants.sunColor[2] = sun.color.z;
    constants.skyColor[0] = sun.ambient.x;
    constants.skyColor[1] = sun.ambient.y;
    constants.skyColor[2] = sun.ambient.z;
    for (int i = 0; i < 3; ++i) {
        constants.lightTint[i] = 1.0f;
        constants.bakedSkyTint[i] = 1.0f;
    }
    constants.emissiveScale = 1.0f;
    constants.sunAngularRadius = kRealSunRadius;
    constants.sunConeSpan = ConeSpan(kRealSunRadius);
    constants.skyCameraX = sun.skyCameraOrigin.x;
    constants.skyCameraY = sun.skyCameraOrigin.y;
    constants.skyCameraZ = sun.skyCameraOrigin.z;
    constants.worldToSky = sun.skyCameraScale > 0.0f ? 1.0f / sun.skyCameraScale : 0.0f;
    constants.materialsView = scene.MaterialsView();
    constants.lightsView = scene.LightsView();
    const Scene::LightLists& lists = scene.Lights();
    constants.lightRangesView = lists.rangesView;
    constants.lightIndicesView = lists.indicesView;
    constants.bspNodesView = lists.nodesView;
    constants.leafClustersView = lists.leafClustersView;
    constants.clusterCount = lists.clusterCount;
    std::memcpy(constants.gridOrigin, &lists.gridOrigin, sizeof(constants.gridOrigin));
    constants.gridCellSize = lists.gridCellSize;
    std::copy_n(lists.gridDims, 3, constants.gridDims);
    constants.gridCellsView = lists.gridCellsView;
    constants.gridIndicesView = lists.gridIndicesView;
    constants.gridTailView = lists.gridTailView;
    constants.emittersView = scene.EmittersView();
    const WaterViews& water = scene.Water();
    constants.waterBodiesView = water.bodiesView;
    constants.waterBodyCount = water.bodyCount;
    constants.brushWaterFirst = water.brushFirst;
    constants.leafWaterView = water.leafWaterView;
    constants.waterFramesView = water.framesView;
    constants.waterTrianglesView = water.trianglesView;
    constants.waterTop = water.top;
    constants.eyeWater = kInvalidView;
    constants.waterTime = gbuffer::WaterClock();
    constants.causticsView = kInvalidView;
    constants.waterDrawsView = kInvalidView;
    constants.waterSunView = kInvalidView;
    constants.coarseWaterSunView = kInvalidView;
}

bool Lighting::FillConstants(FrameConstants& constants, const Scene& scene, const ViewInputs& inputs)
{
    constants = {};
    std::memcpy(constants.viewProj, inputs.viewProj.data(), sizeof(constants.viewProj));
    // The depth was drawn with the jittered projection; reconstructing with the plain one shifts positions by up to
    // half a pixel, which puts points by a crease behind the other surface.
    float jittered[4][4];
    std::memcpy(jittered, constants.viewProj, sizeof(jittered));
    const std::array<float, 2>& jitter = inputs.jitter;
    for (int c = 0; c < 4; ++c) {
        jittered[0][c] += jitter[0] * constants.viewProj[3][c];
        jittered[1][c] += jitter[1] * constants.viewProj[3][c];
    }
    if (!ClipToEye(jittered, constants.clipToEye, constants.eyePosition))
        return false;
    FillSceneConstants(constants, scene);
    // The map's own sun and sky, which its fog's colour shows (LitFog in shaders/scene.hlsli).
    float mapSun[3];
    float mapSky[3];
    std::copy_n(constants.sunColor, 3, mapSun);
    std::copy_n(constants.skyColor, 3, mapSky);
    const bool mapSunShines = constants.sunEnabled != 0.0f;
    // The 3D skybox's scaled-down scene has no water of its own.
    if (inputs.kind == gbuffer::ViewKind::Skybox)
        constants.waterBodyCount = 0;
    else
        constants.eyeWater = scene.WaterAt(constants.eyePosition);
    const bsp::SunLight& sun = scene.Sun();
    const Settings& settings = GetSettings();
    // The map's sun shines from where env_sun shows it, so the sun editor moves it; below the horizon it lights nothing.
    if (settings.sunFollowsEnvSun && constants.sunEnabled != 0.0f && inputs.envSun) {
        std::copy(inputs.envSun->direction.begin(), inputs.envSun->direction.end(), constants.sunDirection);
        if (constants.sunDirection[2] <= 0.0f)
            constants.sunEnabled = 0.0f;
    }
    if (inputs.kind == gbuffer::ViewKind::Main) {
        Statistics& statistics = GetStatistics();
        std::copy_n(constants.sunDirection, 3, statistics.sunDirection.begin());
        std::copy_n(constants.sunColor, 3, statistics.mapSunColor.begin());
        std::copy_n(constants.skyColor, 3, statistics.mapSkyColor.begin());
        statistics.mapHasSun = sun.present;
    }
    // The menu's overrides of the map's light. A sun put below the horizon lights nothing, as env_sun's does.
    if (settings.sunDirectionOverride && sun.present) {
        const std::array<float, 3> direction = OverrideSunDirection(settings);
        std::copy(direction.begin(), direction.end(), constants.sunDirection);
        constants.sunEnabled = direction[2] > 0.0f ? 1.0f : 0.0f;
    }
    // Baked bounce light holds the map's colours; these carry it to the overrides' colours channel by channel.
    std::array<float, 3> sunRecolor = {1.0f, 1.0f, 1.0f};
    if (settings.sunColorOverride) {
        const std::array<float, 3> hue = LinearColor(settings.sunColor);
        const float hueLuminance = Luminance(hue.data());
        const float brightness = Luminance(constants.sunColor);
        for (size_t i = 0; i < 3; ++i) {
            const float color = hueLuminance > 0.0f ? hue[i] * brightness / hueLuminance : 0.0f;
            sunRecolor[i] = constants.sunColor[i] > 0.0f ? color / constants.sunColor[i] : 0.0f;
            constants.sunColor[i] = color;
        }
    }
    if (settings.skyLightOverride) {
        const std::array<float, 3> color = LinearColor(settings.skyLightColor);
        for (size_t i = 0; i < 3; ++i) {
            const float sky = color[i] * settings.skyLightBrightness / 255.0f;
            constants.bakedSkyTint[i] = constants.skyColor[i] > 0.0f ? sky / constants.skyColor[i] : 0.0f;
            constants.skyColor[i] = sky;
        }
    }
    const std::array<float, 3> lightTint = LinearColor(settings.lightTint);
    std::copy(lightTint.begin(), lightTint.end(), constants.lightTint);
    constants.emissiveScale = settings.emissiveIntensity;
    constants.sunAngularRadius = kRealSunRadius * settings.sunShadowSoftness;
    constants.sunConeSpan = ConeSpan(constants.sunAngularRadius);
    const float sunStyle = scene.LightStyle(sun.style);
    const float skyStyle = scene.LightStyle(sun.ambientStyle);
    const Daylight daylight = DaylightFor(constants, inputs, scene).value_or(Daylight{});
    if (daylight.moonlight) {
        std::copy(daylight.moonlight->begin(), daylight.moonlight->end(), constants.sunDirection);
        constants.sunEnabled = 1.0f;
    }
    // The clouds take their share of the sky's light, or add to it, as the cloudy sky lights a surface facing up against
    // the clear sky; their shadows take the sun's (CloudShadowAt in shaders/scene.hlsli).
    const float cloudSky = CloudSkyFactor(inputs, scene);
    for (int i = 0; i < 3; ++i) {
        constants.sunColor[i] *= settings.sun ? settings.sunIntensity * sunStyle * daylight.sun[i] : 0.0f;
        constants.skyColor[i] *= settings.skyIntensity * skyStyle * daylight.sky * cloudSky;
        // BakeStep bakes the map's sun and sky as they are in the map.
        constants.bakedSunScale[i] = constants.sunEnabled != 0.0f && settings.sun ? settings.sunIntensity * sunStyle * daylight.sun[i] * sunRecolor[i] : 0.0f;
    }
    constants.bakedSkyScale = settings.skyIntensity * skyStyle * daylight.sky * cloudSky;
    // The fog's light: in open air a quarter of the sun's irradiance and half the sky's, the map's own showing the fog's
    // colour. The sun's share follows the sun's colour channel by channel, so sunsets colour the fog; a channel the map's
    // sun nearly lacks is compared with a tenth of its brightest.
    const float mapSunLight = mapSunShines ? 0.25f * Luminance(mapSun) : 0.0f;
    const float mapSkyLight = 0.5f * Luminance(mapSky);
    constants.fogAirLight = mapSunLight + mapSkyLight;
    constants.fogSunShare = constants.fogAirLight > 0.0f ? mapSunLight / constants.fogAirLight : 0.0f;
    const float brightestSun = std::max({mapSun[0], mapSun[1], mapSun[2]});
    for (int i = 0; i < 3; ++i)
        constants.fogSunScale[i] = constants.sunEnabled != 0.0f && brightestSun > 0.0f ? constants.sunColor[i] / std::max(mapSun[i], 0.1f * brightestSun) : 0.0f;
    constants.fogSkyScale = mapSkyLight > 0.0f ? Luminance(constants.skyColor) / Luminance(mapSky) : 1.0f;
    if (inputs.kind == gbuffer::ViewKind::Main) {
        for (int i = 0; i < 3; ++i)
            m_fogDaylight[i] = constants.fogSunShare * constants.fogSunScale[i] + (1.0f - constants.fogSunShare) * constants.fogSkyScale;
    }
    constants.lightScale = settings.lightIntensity;
    constants.indirectScale = settings.indirectLight ? settings.indirectIntensity : 0.0f;
    constants.taaHistoryWeight = settings.taaHistoryWeight;
    constants.sharpening = settings.sharpening;
    constants.tonemapScale = inputs.tonemapScale;
    constants.outputSrgb = inputs.outputSrgb ? 1u : 0u;
    constants.toneCurve = static_cast<uint32_t>(settings.toneCurve);
    constants.highlightRolloff = settings.highlightRolloff;
    constants.invSize[0] = 1.0f / static_cast<float>(m_width);
    constants.invSize[1] = 1.0f / static_cast<float>(m_height);
    constants.size[0] = m_width;
    constants.size[1] = m_height;
    constants.normalBias = 2.0f;
    constants.frameIndex = static_cast<uint32_t>(inputs.frame);
    constants.debugView = static_cast<uint32_t>(settings.debugView);
    constants.reflectionScale = settings.reflections ? settings.reflectionIntensity : 0.0f;
    constants.exactLights = static_cast<uint32_t>(settings.exactLights);
    constants.sampledLightRays = static_cast<uint32_t>(settings.sampledLightRays);
    constants.viewmodelShadowRays = static_cast<uint32_t>(settings.viewmodelShadowRays);
    constants.indirectBounces = static_cast<uint32_t>(settings.indirectBounces);
    constants.aoRays = static_cast<uint32_t>(settings.aoRays);
    constants.volumeRays = static_cast<uint32_t>(settings.volumeRays);
    constants.indirectRayFlags = m_useMicromaps && settings.fastAlphaTests ? D3D12_RAY_FLAG_FORCE_OMM_2_STATE : D3D12_RAY_FLAG_NONE;
    constants.dispatchOriginView = inputs.kind != gbuffer::ViewKind::Main ? m_viewRect.srv : kInvalidView;
    constants.restirDebugView = kInvalidView;
    constants.selfIllumView = kInvalidView;
    constants.nrdValidationView = kInvalidView;
    constants.glossyReflections = settings.glossyReflections ? 1u : 0u;
    constants.bakedTrianglesView = kInvalidView;
    constants.bakedFacesView = kInvalidView;
    constants.bakedLightView = kInvalidView;
    constants.leafAmbientView = kInvalidView;
    constants.ambientSamplesView = kInvalidView;
    // The viewmodel's run reads the main view's indirect light volume (the main view's own sets it once filled).
    const bool volumeLight = inputs.kind == gbuffer::ViewKind::Viewmodel && m_volumeReady && m_probeIndirect[0].resource;
    constants.probeRedView = volumeLight ? m_probeIndirect[0].srv : kInvalidView;
    constants.probeGreenView = volumeLight ? m_probeIndirect[1].srv : kInvalidView;
    constants.probeBlueView = volumeLight ? m_probeIndirect[2].srv : kInvalidView;
    constants.depthMin = inputs.depthMin;
    constants.depthScale = inputs.depthMax - inputs.depthMin;
    const gbuffer::Fog& fog = inputs.fog;
    constants.fogEnabled = fog.known && settings.fog ? 1u : 0u;
    std::memcpy(constants.fogParams, fog.params.data(), sizeof(constants.fogParams));
    std::memcpy(constants.fogColor, fog.color.data(), sizeof(constants.fogColor));
    const gbuffer::Fog& skyboxFog = inputs.skyboxFog;
    constants.skyboxFogEnabled = skyboxFog.known && settings.fog ? 1u : 0u;
    std::memcpy(constants.skyboxFogParams, skyboxFog.params.data(), sizeof(constants.skyboxFogParams));
    std::memcpy(constants.skyboxFogColor, skyboxFog.color.data(), sizeof(constants.skyboxFogColor));
    // The procedural sky the last main view made replaces the game's (RecordSky decides again for the main view).
    constants.skyCubeView = inputs.skyCube == nullptr ? kInvalidView : m_proceduralSkyReady ? m_proceduralSky.srv : inputs.skyCubeView;
    constants.nightSkyView = m_nightSkyView;
    for (int row = 0; row < 3; ++row)
        constants.skyMotion[row][row] = 1.0f;
    constants.skyMotionDisc[3] = 2.0f;
    constants.skyLightView = m_skyLightReady ? m_skyLight.srv : kInvalidView;
    SetCloudConstants(constants);
    // The 3D skybox is a scaled-down scene elsewhere in the map: the map's lights do not belong to it.
    const bool skybox = inputs.kind == gbuffer::ViewKind::Skybox;
    if (m_causticsReady && !skybox)
        SetCausticsConstants(constants);
    constants.lightCount = settings.mapLights && !skybox ? scene.LightCount() : 0u;
    constants.lightWarpCount = static_cast<uint32_t>(std::min(inputs.lightWarps.size(), std::size(constants.lightWarpViews)));
    std::copy_n(inputs.lightWarps.begin(), constants.lightWarpCount, constants.lightWarpViews);
    constants.emissiveLighting = static_cast<uint32_t>(settings.emissiveLighting);
    constants.emitterCount = settings.emissiveLighting == EmissiveLighting::Full && !skybox ? scene.EmitterCount() : 0u;
    constants.viewmodel = inputs.kind == gbuffer::ViewKind::Viewmodel ? 1u : 0u;
    // The skybox view shares the main view's orientation, so it finds last frame's sky (reflections.hlsl).
    if (inputs.kind != gbuffer::ViewKind::Viewmodel) {
        constants.historyValid = m_historyValid && inputs.frame - m_lastMainFrame <= 2 ? 1u : 0u;
        std::memcpy(constants.prevViewProj, m_prevViewProj, sizeof(constants.prevViewProj));
        std::memcpy(constants.prevEyePosition, m_prevEye, sizeof(constants.prevEyePosition));
    }
    return true;
}

UINT Lighting::WriteProjectors(UINT slot, std::span<const gbuffer::ProjectedLight> projected, std::span<ID3D12Resource* const> cookies, UINT cookieViews,
    const FrameConstants& constants, const std::optional<std::array<float, 3>>& localPlayer)
{
    const float scale = GetSettings().projectedIntensity;
    const float* forwardRow = constants.viewProj[3];
    const float forwardLength = std::sqrt(Dot(forwardRow, forwardRow));
    const float viewForward[3] = {forwardRow[0] / forwardLength, forwardRow[1] / forwardLength, forwardRow[2] / forwardLength};
    auto* out = reinterpret_cast<GpuProjector*>(m_projectorsMapped) + slot * kMaxProjectors;
    UINT count = 0;
    for (size_t i = 0; i < projected.size() && count < kMaxProjectors; ++i) {
        const gbuffer::ProjectedLight& light = projected[i];
        GpuProjector& gpu = out[count];
        std::memcpy(gpu.worldToTexture, light.worldToTexture.data(), sizeof(gpu.worldToTexture));
        std::memcpy(gpu.origin, light.origin.data(), sizeof(gpu.origin));
        gpu.farZ = light.farZ;
        for (int c = 0; c < 3; ++c)
            gpu.color[c] = light.color[c] * scale;
        std::memcpy(gpu.attenuation, light.attenuation.data(), sizeof(gpu.attenuation));
        gpu.fullDistance = FullStrengthDistance(light.attenuation);
        gpu.flags = 0;
        if (forwardLength > 0.0f) {
            if (const std::optional<float> pullBack = FlashlightPullBack(gpu, constants.eyePosition, viewForward)) {
                AdvanceApex(gpu, *pullBack);
                gpu.flags = kProjectorLocalFlashlight;
            }
        }
        if (gpu.flags == 0 && localPlayer) {
            const float centre[3] = {(*localPlayer)[0], (*localPlayer)[1], (*localPlayer)[2] + kPlayerCentreHeight};
            const float offset[3] = {gpu.origin[0] - centre[0], gpu.origin[1] - centre[1], gpu.origin[2] - centre[2]};
            if (Dot(offset, offset) <= kThirdPersonFlashlightReach * kThirdPersonFlashlightReach)
                gpu.flags = kProjectorLocalFlashlight;
        }
        gpu.cookieView = kInvalidView;
        if (i < cookies.size() && cookies[i] != nullptr) {
            const D3D12_RESOURCE_DESC desc = cookies[i]->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = light.cookieSrgb ? SrgbViewFormat(desc.Format) : ColorViewFormat(desc.Format);
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = desc.MipLevels;
            m_device->CreateShaderResourceView(cookies[i], &srv, m_descriptors->Cpu(cookieViews + count));
            gpu.cookieView = cookieViews + count;
        }
        ++count;
    }
    return count;
}

void Lighting::BakeStep(Scene& scene, const SceneFrame& frame, D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    BakedBuffers& baked = scene.Baked();
    // The map's static scene with its lights at their baked strength, whatever the settings: style-0 lights and sun,
    // and glowing surfaces in full. Every static instance blocks, as foliage_shadows' "All light" has it.
    FrameConstants& bake = *reinterpret_cast<FrameConstants*>(m_bakeConstantsMapped);
    bake = {};
    FillSceneConstants(bake, scene);
    const bsp::SunLight& sun = scene.Sun();
    bake.sunEnabled = sun.present && sun.style == 0 ? 1.0f : 0.0f;
    bake.lightScale = 1.0f;
    bake.indirectScale = 1.0f;
    bake.lightCount = scene.LightCount();
    bake.lightStylesView = m_bakeStylesView;
    bake.emissiveLighting = static_cast<uint32_t>(EmissiveLighting::Full);
    bake.emitterCount = scene.EmitterCount();
    bake.instancesView = frame.instancesView;
    bake.dynamicLightsView = frame.dynamicLightsView;
    constexpr UINT kStaticMask = kTraceMask & ~(kDynamicMask | kLocalPlayerMask);
    bake.traceMask = bake.sunShadowMask = bake.indirectMask = bake.bounceMask = bake.lightShadowMask = kStaticMask;
    for (uint32_t* view : {&bake.skyCubeView, &bake.skyLightView, &bake.projectorsView, &bake.probeRedView, &bake.probeGreenView, &bake.probeBlueView,
             &bake.aoView, &bake.volumetricView, &bake.dispatchOriginView, &bake.restirDebugView, &bake.selfIllumView, &bake.nrdValidationView,
             &bake.probeView, &bake.bakedTrianglesView, &bake.bakedFacesView, &bake.bakedLightView, &bake.leafAmbientView, &bake.ambientSamplesView,
             &bake.nightSkyView, &bake.cloudCubeView, &bake.cloudShadowView})
        *view = kInvalidView;
    m_list->SetComputeRootConstantBufferView(0, m_bakeConstants->GetGPUVirtualAddress());
    if (baked.tracedLuxels < baked.luxelCount) {
        const UINT count = std::min(kBakeLuxelsPerView, baked.luxelCount - baked.tracedLuxels);
        DispatchGroups(Pass::Baked, {.input0 = baked.luxelsView, .input2 = baked.facesView, .input3 = baked.tracedLuxels, .input4 = count, .output0 = baked.rawUav,
            .step = 0}, (count + 63) / 64, 1, 1);
        baked.tracedLuxels += count;
    } else {
        const D3D12_RESOURCE_BARRIER traced = Transition(scene.BakedRaw(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_list->ResourceBarrier(1, &traced);
        for (UINT first = 0; first < baked.luxelCount; first += kMaxGroups * 64) {
            const UINT count = std::min(kMaxGroups * 64, baked.luxelCount - first);
            DispatchGroups(Pass::Baked, {.input0 = baked.luxelsView, .input1 = baked.rawView, .input2 = baked.facesView, .input3 = first, .input4 = count,
                .output0 = baked.filteredUav, .step = 1}, (count + 63) / 64, 1, 1);
        }
        ID3D12Resource* ambient = scene.BakedAmbient();
        if (ambient != nullptr) {
            for (UINT first = 0; first < baked.ambientSampleCount; first += kMaxGroups * 64) {
                const UINT count = std::min(kMaxGroups * 64, baked.ambientSampleCount - first);
                DispatchGroups(Pass::Baked, {.input3 = first, .input4 = count, .output0 = baked.ambientSamplesUav, .step = 2}, (count + 63) / 64, 1, 1);
            }
        }
        D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(scene.BakedFiltered(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(ambient, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        m_list->ResourceBarrier(ambient != nullptr ? 2 : 1, barriers);
        if (!baked.cachePath.empty()) {
            const UINT64 lightBytes = scene.BakedFiltered()->GetDesc().Width;
            const UINT64 ambientBytes = ambient != nullptr ? ambient->GetDesc().Width : 0;
            m_bakeCacheReadback = CreateBuffer(m_device, D3D12_HEAP_TYPE_READBACK, lightBytes + ambientBytes, D3D12_RESOURCE_STATE_COPY_DEST);
            if (m_bakeCacheReadback) {
                m_list->CopyBufferRegion(m_bakeCacheReadback.Get(), 0, scene.BakedFiltered(), 0, lightBytes);
                if (ambient != nullptr)
                    m_list->CopyBufferRegion(m_bakeCacheReadback.Get(), lightBytes, ambient, 0, ambientBytes);
            }
        }
        D3D12_RESOURCE_BARRIER readable[] = {
            Transition(scene.BakedFiltered(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(ambient, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        m_list->ResourceBarrier(ambient != nullptr ? 2 : 1, readable);
        baked.filtered = true;
        m_bakeLogPending = true;
    }
    m_list->SetComputeRootConstantBufferView(0, frameConstants);
}

float Lighting::BakeProgress(Scene& scene) const
{
    const BakedBuffers& baked = scene.Baked();
    if (baked.Ready())
        return 1.0f;
    return static_cast<float>(baked.tracedLuxels) / static_cast<float>(std::max(baked.luxelCount, 1u));
}

void Lighting::Bake(Scene& scene, const std::function<void(float)>& progress)
{
    if (!WaitForPipelines() || !scene.Ready() || !scene.HasBaked() || scene.Baked().Ready())
        return;
    m_useMicromaps = m_opacityMicromaps && GetSettings().opacityMicromaps && scene.HasMicromaps();
    WaitForGpu();
    const UINT slot = static_cast<UINT>(m_run % kSlots);
    const D3D12_GPU_VIRTUAL_ADDRESS bakeConstants = m_bakeConstants->GetGPUVirtualAddress();
    while (!scene.Baked().Ready()) {
        m_allocators[slot]->Reset();
        m_list->Reset(m_allocators[slot].Get(), nullptr);
        const SceneFrame frame = scene.RecordFrame(m_list.Get(), slot, {}, m_useMicromaps);
        BindCompute(bakeConstants, frame.tlas, slot);
        BakeStep(scene, frame, bakeConstants);
        const HRESULT hr = m_list->Close();
        if (FAILED(hr)) {
            log::Info("Lighting: closing the bake's command list failed ({:#010x})", Hex(hr));
            m_failed = true;
            return;
        }
        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        WaitForGpu();
        if (std::exchange(m_bakeLogPending, false))
            FinishBake(scene);
        progress(BakeProgress(scene));
    }
}

void Lighting::FinishBake(Scene& scene)
{
    log::Info("Baked light: {} luxels and {} leaf ambient samples baked", scene.Baked().luxelCount, scene.Baked().ambientSampleCount);
    if (!m_bakeCacheReadback)
        return;
    WaitForGpu();
    const std::filesystem::path& path = scene.Baked().cachePath;
    const UINT64 bytes = m_bakeCacheReadback->GetDesc().Width;
    const D3D12_RANGE whole = {0, static_cast<SIZE_T>(bytes)};
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary);
    void* mapped = nullptr;
    if (file && SUCCEEDED(m_bakeCacheReadback->Map(0, &whole, &mapped))) {
        file.write(reinterpret_cast<const char*>(&kBakeVersion), sizeof(kBakeVersion));
        file.write(static_cast<const char*>(mapped), static_cast<std::streamsize>(bytes));
        const D3D12_RANGE written = {0, 0};
        m_bakeCacheReadback->Unmap(0, &written);
    }
    if (!file)
        log::Info("Baked light: writing {} failed", path.string());
    m_bakeCacheReadback.Reset();
}

AntiAliasing Lighting::AntiAliasingMode() const
{
    const AntiAliasing chosen = GetSettings().antiAliasing;
    if ((chosen == AntiAliasing::RayReconstruction && !m_rayReconstruction.Usable()) || (chosen == AntiAliasing::Fsr && !m_fsr.Usable()))
        return AntiAliasing::Temporal;
    return chosen;
}

Lighting::Upscaler Lighting::RenderSize(UINT outputWidth, UINT outputHeight, UINT& width, UINT& height)
{
    const Settings& settings = GetSettings();
    if (settings.debugView != DebugView::Final)
        return Upscaler::None;
    const AntiAliasing mode = AntiAliasingMode();
    const bool dlss = mode == AntiAliasing::RayReconstruction;
    if (!dlss && mode != AntiAliasing::Fsr)
        return Upscaler::None;
    UpscalingMode& sizeMode = dlss ? m_rrSizeMode : m_fsrSizeMode;
    UINT* size = dlss ? m_rrSize : m_fsrSize;
    if (sizeMode != settings.upscaling || size[0] != outputWidth || size[1] != outputHeight) {
        const bool found = dlss ? m_rayReconstruction.RenderSize(settings.upscaling, outputWidth, outputHeight, size[2], size[3])
                                : m_fsr.RenderSize(settings.upscaling, outputWidth, outputHeight, size[2], size[3]);
        if (!found)
            return Upscaler::None;
        sizeMode = settings.upscaling;
        size[0] = outputWidth;
        size[1] = outputHeight;
    }
    width = size[2];
    height = size[3];
    return dlss ? Upscaler::Dlss : Upscaler::Fsr;
}

void Lighting::Run(const ViewInputs& inputs, Scene& scene, std::span<const SceneInstance> dynamic, const std::vector<SkinnedEntity>& skinned)
{
    // A main view that stops before RecordSky leaves the game's sky.
    if (inputs.kind == gbuffer::ViewKind::Main) {
        m_skyDraw = {};
        m_proceduralSkyReady = false;
        m_nightSkyView = DescriptorHeap::kInvalid;
    }
    if (!WaitForPipelines())
        return;
    const D3D12_RESOURCE_DESC colorDesc = inputs.color->GetDesc();
    if (!EnsureTargets(static_cast<UINT>(colorDesc.Width), colorDesc.Height)) {
        log::Info("Lighting: creating render targets failed; lighting disabled");
        m_failed = true;
        return;
    }

    const UINT slot = static_cast<UINT>(m_run % kSlots);
    if (m_fence->GetCompletedValue() < m_slotFenceValues[slot]) {
        m_fence->SetEventOnCompletion(m_slotFenceValues[slot], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    if (m_volumeReadbackPending[slot]) {
        m_volumeReadbackPending[slot] = false;
        PublishVolume(slot);
    }
    if (m_mapSkyReadbackPending[slot]) {
        m_mapSkyReadbackPending[slot] = false;
        ReadMapSky(slot);
    }
    if (m_cloudReadbackPending[slot]) {
        m_cloudReadbackPending[slot] = false;
        ReadCloudLight(slot);
    }
    // The stars' frame (and the Moon among the stars) turns with the sun the sky shows, once a frame.
    if (inputs.kind == gbuffer::ViewKind::Main) {
        if (const std::optional<std::array<float, 3>> sun = SkySun(inputs, scene)) {
            m_nightSky.FollowSun(*sun);
            m_nightSky.PlaceMoon(*sun, GetSettings().moonPhase);
        }
    }
    // Timings of the view that last used this slot, per pass: the main view's for the menu, the 3D skybox's and the
    // viewmodel's for the developer log.
    if (m_slotFenceValues[slot] != 0 && !m_slotWasMain[slot]) {
        const uint64_t* markers = m_timestampValues + slot * kMarkers;
        if (markers[kPassCount] > markers[0]) {
            const double ms = static_cast<double>(markers[kPassCount] - markers[0]) * m_timestampPeriodMs;
            m_otherViewsMsTotal += ms;
            if (m_slotWasSkybox[slot])
                m_skyboxMsTotal += ms;
            std::array<double, kPassCount>& passes = m_slotWasSkybox[slot] ? m_skyboxPassMsTotal : m_viewmodelPassMsTotal;
            for (UINT pass = 0; pass < kPassCount; ++pass)
                passes[pass] += static_cast<double>(markers[pass + 1] - markers[pass]) * m_timestampPeriodMs;
        }
    }
    if (m_slotFenceValues[slot] != 0 && m_slotWasMain[slot]) {
        const uint64_t* markers = m_timestampValues + slot * kMarkers;
        if (markers[kPassCount] > markers[0]) {
            for (UINT pass = 0; pass < kPassCount; ++pass)
                m_passMsTotal[pass] += static_cast<double>(markers[pass + 1] - markers[pass]) * m_timestampPeriodMs;
            if (++m_gpuSamples == kTimingSamples) {
                Statistics& statistics = GetStatistics();
                statistics.lightingMs = 0.0;
                for (UINT pass = 0; pass < kPassCount; ++pass) {
                    statistics.passMs[pass] = m_passMsTotal[pass] / m_gpuSamples;
                    statistics.lightingMs += statistics.passMs[pass];
                }
                statistics.otherViewsMs = m_otherViewsMsTotal / m_gpuSamples;
                statistics.lightingMs += statistics.otherViewsMs;
                const double skyboxMs = m_skyboxMsTotal / m_gpuSamples;
                m_otherViewsMsTotal = 0.0;
                m_skyboxMsTotal = 0.0;
                m_passMsTotal = {};
                m_gpuSamples = 0;
                if (++m_timingReports % (GetOptions().devMode ? 1 : 10) == 0) {
                    constexpr size_t kRecentFrames = 240;
                    const size_t frames = std::min(statistics.frameCount, kRecentFrames);
                    double frameMs = 0.0;
                    for (size_t i = 1; i <= frames; ++i)
                        frameMs += statistics.frameMs[(statistics.nextFrame + statistics.frameMs.size() - i) % statistics.frameMs.size()];
                    const auto& p = statistics.passMs;
                    log::Info("Lighting: {:.3f} ms GPU per frame ({}x{} traced at {}x{}; scene {:.2f}, surface {:.2f}, radiance cache {:.2f}, volume {:.2f}, "
                              "direct {:.2f}, light sampling {:.2f}, ao {:.2f}, indirect {:.2f}, reflections {:.2f}, accumulation {:.2f}, denoiser {:.2f}, "
                              "lighting {:.2f}, resolve {:.2f}; 3D skybox and viewmodel {:.2f}, of which 3D skybox {:.2f}); frame time {:.2f} ms, tonemap scale {:.3f}",
                        statistics.lightingMs, m_width, m_height, statistics.renderWidth, statistics.renderHeight, p[0], p[1], p[2], p[3], p[4],
                        p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], statistics.otherViewsMs, skyboxMs, frames ? frameMs / frames : 0.0, inputs.tonemapScale);
                    if (GetOptions().devMode) {
                        const auto passes = [&](const std::array<double, kPassCount>& totals) {
                            std::string text;
                            for (UINT pass = 0; pass < kPassCount; ++pass)
                                text += std::format("{}{:.2f}", pass ? " " : "", totals[pass] / kTimingSamples);
                            return text;
                        };
                        log::Info("Lighting: per pass as above, 3D skybox ({}), viewmodel ({})", passes(m_skyboxPassMsTotal), passes(m_viewmodelPassMsTotal));
                    }
                }
                m_skyboxPassMsTotal = {};
                m_viewmodelPassMsTotal = {};
            }
        }
    }
    m_slotWasMain[slot] = inputs.kind == gbuffer::ViewKind::Main;
    m_slotWasSkybox[slot] = inputs.kind == gbuffer::ViewKind::Skybox;
    const bool main = inputs.kind == gbuffer::ViewKind::Main;
    UINT renderWidth = m_width;
    UINT renderHeight = m_height;
    const Upscaler upscaler = main && scene.Ready() ? RenderSize(m_width, m_height, renderWidth, renderHeight) : Upscaler::None;
    // Ray Reconstruction denoises and upscales; FSR upscales what the usual denoisers left at render resolution.
    const bool rayReconstruction = upscaler == Upscaler::Dlss;
    const bool upscaled = upscaler != Upscaler::None;
    if (upscaler == Upscaler::None) {
        renderWidth = m_width;
        renderHeight = m_height;
    }
    if (main) {
        // Histories made at another resolution or by another pipeline cannot be reused.
        if (upscaler != m_lastMainUpscaler) {
            m_historyValid = false;
            m_taaHistoryValid = false;
        }
        m_lastMainUpscaler = upscaler;
        Statistics& statistics = GetStatistics();
        statistics.renderWidth = renderWidth;
        statistics.renderHeight = renderHeight;
    }
    if (rayReconstruction && m_rayReconstruction.NeedsNewFeature(GetSettings().upscaling, renderWidth, renderHeight, m_width, m_height)) {
        WaitForGpu();
        m_rayReconstruction.ReleaseFeature();
        m_historyValid = false;
    }

    std::byte* slotConstants = m_constantsMapped + slot * kConstantsPerSlot * kConstantsStride;
    const D3D12_GPU_VIRTUAL_ADDRESS slotConstantsGpu = m_constants->GetGPUVirtualAddress() + slot * kConstantsPerSlot * kConstantsStride;
    FrameConstants& constants = *reinterpret_cast<FrameConstants*>(slotConstants);
    // Ray queries that allow opacity micromaps cost a little even where there are none, so maps without them, and the
    // setting that turns them off, use the plain passes; instances then ignore their micromaps (Scene::RecordFrame).
    m_useMicromaps = m_opacityMicromaps && GetSettings().opacityMicromaps && scene.HasMicromaps();
    if ((GetSettings().physicalSky || GetSettings().sunDisc) && !m_atmosphereTables && !m_atmosphereFailed)
        MakeAtmosphereTables(slot, slotConstantsGpu);
    if (!FillConstants(constants, scene, inputs)) {
        if (++m_cameraFailures % 300 == 1)
            log::Info("Lighting: the camera of view kind {} cannot be inverted; view left unlit ({} times)", static_cast<int>(inputs.kind), m_cameraFailures);
        return;
    }
    const bool volumetricSetting = GetSettings().volumetricLight && GetSettings().volumetricIntensity > 0.0f;
    // The map's fog above water is drawn lit by the volumetric pass, in the main view and the 3D skybox; Source's water
    // fog stays flat.
    const bool litFog = volumetricSetting && constants.fogEnabled != 0 && constants.fogParams[1] <= 0.0f;
    // The irradiance volume follows the main view's camera in whole cells. Lit fog reads its indirect light even when
    // translucent surfaces do not use it.
    const bool volume = main && scene.Ready() && (GetSettings().translucentLighting || litFog) && m_probeVolume[0].resource;
    if (main)
        m_volumeReady = false;
    for (int i = 0; i < 3; ++i) {
        if (volume)
            m_probeOrigin[i] = static_cast<int>(std::floor(constants.eyePosition[i] / gbuffer::kProbeSpacing)) - static_cast<int>(gbuffer::kProbeGrid[i] / 2);
        constants.probeOrigin[i] = m_probeOrigin[i];
    }
    constants.probeSpacing = gbuffer::kProbeSpacing;
    constants.aoView = kInvalidView;
    constants.aoIntensity = GetSettings().aoIntensity;
    constants.volumetricView = kInvalidView;
    constants.volumetricFog = 0;
    // Haze thin enough to show only as shafts against the light at the default intensity (shaders/volumetrics.hlsl).
    // 1/6000 per unit washed every distant surface out in the sun (gm_construct), and 1/20000 still hazed maps over
    // (2026-10-31).
    constexpr float kVolumetricDensity = 1.0f / 60000.0f;
    constants.volumetricDensity = kVolumetricDensity * GetSettings().volumetricIntensity;
    const bool volumetrics = (main && volumetricSetting) || (inputs.kind == gbuffer::ViewKind::Skybox && litFog);
    if (main)
        m_litFogScale = 0.0f;
    // indirect_lighting = 1: the baked bounce light replaces traced indirect light once the map's is baked.
    const bool bakedIndirect = GetSettings().indirectLighting == IndirectLighting::Baked && scene.HasBaked();
    // The radiance cache serves traced indirect light. The main view decides whether it exists; its eye sizes the
    // voxels for every view. The 3D skybox's scaled-down space stays out of it.
    if (main) {
        const Settings& settings = GetSettings();
        const bool wanted = m_radianceCacheSupported && !m_cacheFailed && settings.radianceCache && settings.indirectLight && !bakedIndirect && scene.Ready();
        m_cacheWanted = EnsureRadianceCache(wanted);
        if (wanted && !m_cacheWanted)
            m_cacheFailed = true;
        std::copy_n(constants.eyePosition, 3, m_cacheCamera);
    }
    SamplingConstants& sampling = *reinterpret_cast<SamplingConstants*>(m_samplingMapped + slot * kSamplingStride);
    sampling = {};
    std::copy_n(m_cacheCamera, 3, sampling.cacheCamera);
    std::copy_n(m_cacheCameraPrevious, 3, sampling.cacheCameraPrevious);
    sampling.sceneScale = kCacheSceneScale;
    sampling.cacheEnabled = m_cacheWanted && inputs.kind != gbuffer::ViewKind::Skybox ? 1u : 0u;
    sampling.capacity = kCacheCapacity;
    sampling.accumulationView = m_cacheAccumulationUav;
    sampling.resolvedView = m_cacheResolvedUav;
    sampling.accumulationFrames = kCacheAccumulationFrames;
    sampling.staleFrames = kCacheStaleFrames;
    sampling.radianceScale = kCacheRadianceScale;
    if (bakedIndirect && scene.Baked().Ready()) {
        const BakedBuffers& baked = scene.Baked();
        constants.bakedTrianglesView = baked.trianglesView;
        constants.bakedFacesView = baked.facesView;
        constants.bakedLightView = baked.filteredView;
        constants.leafAmbientView = baked.leafAmbientView;
        constants.ambientSamplesView = baked.ambientSamplesView;
    }
    // Ambient occlusion darkens only the baked indirect light (shaders/deferred_lighting.hlsl): traced light holds its
    // own occlusion. The 3D skybox's scaled-down scene has no contact shadows worth tracing.
    const bool ambientOcclusion = GetSettings().ambientOcclusion && inputs.kind != gbuffer::ViewKind::Skybox && constants.bakedTrianglesView != kInvalidView;
    // RTXDI samples the main view's local lights (the direct pass then lights with the sun alone). Its resources are made
    // here, before any command refers to the ones they replace; the light PDF leaves room for the frame's dynamic and
    // projected lights. Auto keeps the direct pass with Ray Reconstruction, which needs samples uncorrelated across the
    // screen (DLSS-RR Integration Guide 3.5): RTXDI's candidates are shared by screen tiles and ReGIR cells, which it
    // turned into blotches, and the strongest lights and the flashlight got no exact shadow ray.
    const LightSampling lightSampling = GetSettings().lightSampling;
    const RestirMode restirMode = lightSampling == LightSampling::Ris ? RestirMode::Ris : RestirMode::Reuse;
    const bool restirWanted = lightSampling == LightSampling::Ris || lightSampling == LightSampling::Restir ||
        (lightSampling == LightSampling::Auto && !rayReconstruction);
    bool restir = false;
    if (main && m_restirAvailable && restirWanted && scene.Ready()) {
        const RestirLights capacity{constants.lightCount, constants.emitterCount, 0, 0};
        if (m_restir.NeedsResources(renderWidth, renderHeight, capacity))
            WaitForGpu();
        restir = m_restir.Prepare(renderWidth, renderHeight, capacity);
    }

    ID3D12CommandQueue* queue = m_queue;
    const bool projectedLights = inputs.kind != gbuffer::ViewKind::Skybox && GetSettings().projectedLights;
    std::vector<ID3D12Resource*> resources = {inputs.color, inputs.depth, inputs.normal, inputs.flags, inputs.light, inputs.albedo, inputs.emissive};
    // Cookie textures of the projected lights, each transitioned once.
    std::vector<ID3D12Resource*> cookies;
    if (projectedLights) {
        for (size_t i = 0; i < inputs.projected.size() && i < kMaxProjectors; ++i) {
            ID3D12Resource* cookie = i < inputs.cookies.size() ? inputs.cookies[i] : nullptr;
            if (cookie != nullptr && std::find(resources.begin() + kInputViews, resources.end(), cookie) == resources.end())
                resources.push_back(cookie);
            cookies.push_back(cookie);
        }
    }
    if (inputs.skyCube != nullptr)
        resources.push_back(inputs.skyCube);
    // The water surfaces' normal maps as the game drew them (WriteWaterDraws).
    for (size_t i = 0; i < inputs.waterNormalMaps.size() && i < gbuffer::kMaxWaterDraws; ++i) {
        ID3D12Resource* normalMap = inputs.waterNormalMaps[i];
        if (normalMap != nullptr && std::find(resources.begin() + kInputViews, resources.end(), normalMap) == resources.end())
            resources.push_back(normalMap);
    }
    // The game's probe volume textures, filled by copies at the end of the irradiance volume passes.
    const std::array<ID3D12Resource*, 3>& probeTargets = inputs.probes;
    const bool copyVolume = volume && GetSettings().translucentLighting && probeTargets[0] != nullptr && probeTargets[1] != nullptr &&
        probeTargets[2] != nullptr;
    bool volumeFilled = false;
    ID3D12Resource* color = inputs.color;
    ID3D12Resource* depth = inputs.depth;
    ID3D12Resource* normal = inputs.normal;
    ID3D12Resource* flags = inputs.flags;
    ID3D12Resource* light = inputs.light;
    ID3D12Resource* albedo = inputs.albedo;
    ID3D12Resource* emissive = inputs.emissive;
    if (!m_loggedFormats) {
        m_loggedFormats = true;
        log::Info("Lighting: color format {}, depth format {}, normal format {}, flags format {}", static_cast<int>(colorDesc.Format),
            static_cast<int>(depth->GetDesc().Format), static_cast<int>(normal->GetDesc().Format), static_cast<int>(flags->GetDesc().Format));
    }

    const UINT views = m_slotViews + slot * kViewsPerSlot;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    m_device->CreateShaderResourceView(m_colorCopy.Get(), &srv, m_descriptors->Cpu(views));
    srv.Format = DepthViewFormat(depth->GetDesc().Format);
    m_device->CreateShaderResourceView(depth, &srv, m_descriptors->Cpu(views + 1));
    srv.Format = ColorViewFormat(normal->GetDesc().Format);
    m_device->CreateShaderResourceView(normal, &srv, m_descriptors->Cpu(views + 2));
    srv.Format = ColorViewFormat(flags->GetDesc().Format);
    m_device->CreateShaderResourceView(flags, &srv, m_descriptors->Cpu(views + 3));
    srv.Format = ColorViewFormat(light->GetDesc().Format);
    m_device->CreateShaderResourceView(light, &srv, m_descriptors->Cpu(views + 4));
    srv.Format = ColorViewFormat(albedo->GetDesc().Format);
    m_device->CreateShaderResourceView(albedo, &srv, m_descriptors->Cpu(views + 5));
    srv.Format = ColorViewFormat(emissive->GetDesc().Format);
    m_device->CreateShaderResourceView(emissive, &srv, m_descriptors->Cpu(views + 6));
    constants.colorView = views;
    constants.depthView = views + 1;
    constants.gbufferNormalView = views + 2;
    constants.flagsView = views + 3;
    constants.lightView = views + 4;
    constants.albedoView = views + 5;
    constants.emissiveView = views + 6;
    if (projectedLights) {
        constants.projectorCount = WriteProjectors(slot, inputs.projected, cookies, views + kInputViews, constants, inputs.localPlayer);
        constants.projectorsView = m_projectorViews[slot];
    }
    WriteWaterDraws(slot, inputs, scene, views + kInputViews + kMaxProjectors, constants);
    // The 3D skybox is drawn before the main view of the same frame, so it may use the surface the main
    // view is about to overwrite; the previous frame's main-view data stays intact.
    const UINT current = static_cast<UINT>(m_mainRuns % 2);
    const UINT previous = 1 - current;
    constants.surfaceView = m_surface[current].srv;
    constants.prevSurfaceView = m_surface[previous].srv;
    constants.visibilityView = m_visibility.srv;
    constants.motionView = m_motion.srv;
    constants.previousDistanceView = m_previousDistance.srv;
    constants.pixelFlagsView = m_pixelFlags.srv;
    constants.geometricNormalView = m_geometricNormal.srv;
    constants.specularView = m_specular.srv;
    const bool probe = main && m_probeRequested && m_probeBuffer;
    constants.probeView = probe ? m_probeUav : kInvalidView;
    constants.probePixel = m_probePixel;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(slot) * m_rtvIncrement;
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = ColorViewFormat(colorDesc.Format);
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_device->CreateRenderTargetView(color, &rtvDesc, rtv);

    m_allocators[slot]->Reset();
    m_list->Reset(m_allocators[slot].Get(), nullptr);
    UINT marker = slot * kMarkers;
    m_list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, marker++);
    {
        D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(normal, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(flags, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(light, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(albedo, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(emissive, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        m_list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
        for (size_t i = kInputViews; i < resources.size(); ++i) {
            D3D12_RESOURCE_BARRIER barrier = Transition(resources[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_list->ResourceBarrier(1, &barrier);
        }
    }
    m_list->CopyResource(m_colorCopy.Get(), color);
    {
        D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(m_colorCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        m_list->ResourceBarrier(2, barriers);
    }

    m_frameInstances.assign(dynamic.begin(), dynamic.end());
    UpdateSkinned(scene, skinned, slot, slotConstantsGpu);
    if (main)
        UpdateSway(scene, slot, inputs.sway, constants.eyePosition, slotConstantsGpu);
    m_sway.Append(m_frameInstances);
    // The viewmodel, drawn after the main view of its frame, traces the same scene with its own mask
    // (kViewmodelTraceMask): it reuses the main view's acceleration structure and records, characters included, and
    // keeps the main view's slot from being rebuilt until it is done (m_slotFenceValues below).
    const bool reuseScene =
        inputs.kind == gbuffer::ViewKind::Viewmodel && m_mainScene.tlas != 0 && m_mainSceneFrame == inputs.frame && m_mainSceneRun + 1 == m_run;
    const SceneFrame frame = reuseScene ? m_mainScene : scene.RecordFrame(m_list.Get(), slot, m_frameInstances, m_useMicromaps);
    if (main) {
        m_mainScene = frame;
        m_mainSceneFrame = inputs.frame;
        m_mainSceneRun = m_run;
        m_mainSceneSlot = slot;
    }
    constants.instancesView = frame.instancesView;
    constants.dynamicLightsView = frame.dynamicLightsView;
    constants.lightStylesView = frame.lightStylesView;
    if (frame.waterBodiesView != kInvalidView)
        constants.waterBodiesView = frame.waterBodiesView;
    constants.waterTop = frame.waterTop;
    constants.dynamicLightCount = GetSettings().dynamicLights && inputs.kind != gbuffer::ViewKind::Skybox ? frame.dynamicLightCount : 0u;
    Statistics& statistics = GetStatistics();
    if (main) {
        statistics.mapLights = constants.lightCount;
        statistics.dynamicLights = constants.dynamicLightCount;
        statistics.projectedLights = constants.projectorCount;
        statistics.sceneInstances = static_cast<uint32_t>(1 + m_frameInstances.size());
        std::tie(statistics.micromapTriangles, statistics.micromaps) = scene.MicromapCounts();
    }
    if (frame.tlas == 0 || (inputs.kind == gbuffer::ViewKind::Skybox && !scene.HasSkybox()))
        constants.traceMask = 0;
    else if (inputs.kind == gbuffer::ViewKind::Skybox)
        constants.traceMask = kSkyboxMask;
    else
        constants.traceMask = inputs.kind == gbuffer::ViewKind::Viewmodel ? kViewmodelTraceMask : kTraceMask;
    const FoliageShadows foliage = GetSettings().foliageShadows;
    const auto withFoliage = [&](UINT mask, FoliageShadows from) { return foliage >= from ? mask : mask & ~kShadowlessPropMask; };
    constants.sunShadowMask = withFoliage(constants.traceMask, FoliageShadows::Sun);
    const UINT indirectMask = constants.traceMask & ~kLocalPlayerMask;
    constants.indirectMask = withFoliage(indirectMask, FoliageShadows::SunAndIndirect);
    constants.bounceMask = withFoliage(indirectMask, FoliageShadows::All);
    constants.lightShadowMask = withFoliage(constants.traceMask, FoliageShadows::All);
    // The baked light holds the occlusion by static geometry, so ambient occlusion counts only what moves.
    if (constants.bakedTrianglesView != kInvalidView)
        constants.indirectMask = constants.traceMask & (kDynamicMask | kLocalPlayerMask);

    BindCompute(slotConstantsGpu, frame.tlas, slot);
    // The clouds' shadow map comes before everything the sun lights, the caustics' sun maps included.
    if (main) {
        RecordSky(constants, inputs, scene, slot);
        RecordClouds(constants, inputs, scene, slot);
        RecordCaustics(constants, scene);
    }
    if (bakedIndirect && main && constants.traceMask != 0 && !scene.Baked().Ready())
        BakeStep(scene, frame, slotConstantsGpu);
    if (main && inputs.skyCube != nullptr && m_skyLight.resource) {
        Use(m_skyLight, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const UINT faces = m_proceduralSkyReady ? m_proceduralSkyFaces : inputs.skyFacesView;
        DispatchGroups(Pass::Sky, {.input0 = faces, .input1 = m_cloudsReady ? m_cloudCubeFaces : kInvalidView, .output0 = m_skyLight.uav, .step = 0},
            kSkyLightSize, kSkyLightSize, 6);
        const D3D12_RESOURCE_BARRIER between = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_skyLight.resource.Get()}}};
        m_list->ResourceBarrier(1, &between);
        DispatchGroups(Pass::Sky, {.output0 = m_skyLight.uav, .step = 1}, 1, 1, 1);
        // Forward twins read it in pixel shaders (SkyLight).
        Use(m_skyLight, kVolumeRead);
        m_skyLightReady = true;
        constants.skyLightView = m_skyLight.srv;
    } else if (main) {
        // Another map's sky, until this one's is drawn.
        m_skyLightReady = false;
        constants.skyLightView = kInvalidView;
    }
    // The lens flare's light: the disc the sky shows, which sunlight need not come from (SkySun: a painted sky's sun on
    // a map without env_sun), while it is up, else the Moon of day maps; as bright as the sun's slot.
    std::optional<std::array<float, 3>> flareDirection = main ? SkySun(inputs, scene) : std::nullopt;
    if (flareDirection && (*flareDirection)[2] <= 0.0f) {
        const std::optional<MoonState> moon = MoonFor(inputs, scene);
        flareDirection = moon && moon->direction[2] > 0.0f ? std::optional(moon->direction) : std::nullopt;
    }
    const bool flareLight = flareDirection && m_flareVisibility.resource && constants.traceMask != 0 && constants.sunEnabled != 0.0f;
    const float flareRadius = m_skyDraw.sunDisc ? m_skyDraw.sunRadius : kRealSunRadius;
    if (flareLight) {
        Use(m_flareVisibility, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        DispatchGroups(Pass::FlareVisibility,
            {.input0 = std::bit_cast<UINT>(flareRadius), .input1 = std::bit_cast<UINT>((*flareDirection)[0]), .input2 = std::bit_cast<UINT>((*flareDirection)[1]),
                .input3 = std::bit_cast<UINT>((*flareDirection)[2]), .output0 = m_flareVisibility.uav},
            1, 1, 1);
        Use(m_flareVisibility, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    m_gridWidth = m_width;
    m_gridHeight = m_height;
    // Additive draws (glows, sprites) skip the temporal resolve, which would jitter them and, with Ray Reconstruction,
    // denoise them at render resolution as if they were lit surfaces.
    const bool temporalResolve = upscaled || (main && AntiAliasingMode() == AntiAliasing::Temporal);
    constants.additiveAfterResolve = temporalResolve ? 1u : 0u;
    // The jitter's image shift in output pixels.
    const float jitterScale = upscaled ? static_cast<float>(m_width) / static_cast<float>(renderWidth) : 1.0f;
    const UINT shiftX = std::bit_cast<UINT>(inputs.jitterX * jitterScale);
    const UINT shiftY = std::bit_cast<UINT>(inputs.jitterY * jitterScale);
    // The upscalers' resolve tells the 3D skybox's pixels from the open sky's by its distances (shaders/dlss_resolve.hlsl).
    const bool resolveSkyboxDistance = main && m_skyboxDistanceFrame == inputs.frame && m_skyboxDistance.resource;
    const UINT resolveSkybox = resolveSkyboxDistance ? m_skyboxDistance.srv : kInvalidView;

    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto mark = [&] { m_list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, marker++); };
    // The frame constants the passes bind: the render size's under an upscaler.
    const D3D12_GPU_VIRTUAL_ADDRESS passConstantsGpu = upscaled ? slotConstantsGpu + kConstantsStride : slotConstantsGpu;
    if (upscaled) {
        // The passes before the upscaler run at render resolution on copies of the G-buffer.
        for (Texture& texture : m_rrInputs)
            Use(texture, write);
        Dispatch(Pass::DlssInputs, {.input0 = renderWidth, .input1 = renderHeight, .output0 = m_rrInputUavs});
        for (Texture& texture : m_rrInputs)
            Use(texture, read);
        FrameConstants& render = *reinterpret_cast<FrameConstants*>(slotConstants + kConstantsStride);
        render = constants;
        render.size[0] = renderWidth;
        render.size[1] = renderHeight;
        render.invSize[0] = 1.0f / static_cast<float>(renderWidth);
        render.invSize[1] = 1.0f / static_cast<float>(renderHeight);
        render.colorView = m_rrInputs[RrColor].srv;
        render.depthView = m_rrInputs[RrDepth].srv;
        render.gbufferNormalView = m_rrInputs[RrNormal].srv;
        render.flagsView = m_rrInputs[RrFlags].srv;
        render.lightView = m_rrInputs[RrLight].srv;
        render.albedoView = m_rrInputs[RrAlbedo].srv;
        render.emissiveView = m_rrInputs[RrEmissive].srv;
        render.outputLinear = 1;
        m_list->SetComputeRootConstantBufferView(0, passConstantsGpu);
        m_gridWidth = renderWidth;
        m_gridHeight = renderHeight;
    }
    // The viewmodel and the 3D skybox cover part of the screen; their passes run over a rectangle around what they drew
    // (the 3D skybox's passes over the whole screen cost 2.4 ms on gm_construct at 2560x1440, most of it on pixels the
    // main view draws over, 2026-11-03). The 3D skybox's distances are cleared outside it.
    if (inputs.kind != gbuffer::ViewKind::Main) {
        const bool skyboxView = inputs.kind == gbuffer::ViewKind::Skybox;
        Use(m_viewRect, write);
        if (skyboxView)
            Use(m_skyboxDistance, write);
        const D3D12_RESOURCE_BARRIER between = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_viewRect.resource.Get()}}};
        const D3D12_RESOURCE_BARRIER cleared = {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_skyboxDistance.resource.Get()}}};
        DispatchGroups(Pass::ViewRect, {.output0 = m_viewRect.uav, .step = 0}, 1, 1, 1);
        m_list->ResourceBarrier(1, &between);
        Dispatch(Pass::ViewRect, {.input0 = skyboxView ? m_skyboxDistance.uav : kInvalidView, .output0 = m_viewRect.uav, .step = 1});
        m_list->ResourceBarrier(1, &between);
        if (skyboxView)
            m_list->ResourceBarrier(1, &cleared);
        DispatchGroups(Pass::ViewRect, {.output0 = m_viewRect.uav, .step = 2}, 1, 1, 1);
        Use(m_viewRect, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_rectDispatch = true;
    }
    mark();
    Use(m_surface[current], write);
    Dispatch(Pass::Surface, {.output0 = m_surface[current].uav});
    Use(m_surface[current], read);
    Use(m_surface[previous], read);
    for (Texture* texture : {&m_visibility, &m_motion, &m_previousDistance, &m_pixelFlags, &m_geometricNormal})
        Use(*texture, write);
    // The 3D skybox view records its surfaces' distances; the main view of the same frame moves them with them, as they
    // lie far beyond the far plane its empty depth puts them at.
    const bool skyboxView = inputs.kind == gbuffer::ViewKind::Skybox;
    const bool skyboxDistance = skyboxView || (main && m_skyboxDistanceFrame == inputs.frame);
    if (skyboxDistance)
        Use(m_skyboxDistance, skyboxView ? write : read);
    Dispatch(Pass::Primary, {.input0 = skyboxDistance ? (skyboxView ? m_skyboxDistance.uav : m_skyboxDistance.srv) : kInvalidView,
        .output0 = m_visibility.uav, .output1 = m_motion.uav, .output2 = m_previousDistance.uav, .output3 = m_pixelFlags.uav,
        .step = skyboxView ? 1u : 0u, .output4 = m_geometricNormal.uav});
    if (skyboxView)
        m_skyboxDistanceFrame = inputs.frame;
    for (Texture* texture : {&m_visibility, &m_motion, &m_previousDistance, &m_pixelFlags, &m_geometricNormal})
        Use(*texture, read);
    mark();
    if (main && m_cacheWanted && constants.traceMask != 0) {
        const D3D12_RESOURCE_BARRIER cacheBarriers[] = {
            {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_cacheHash.Get()}}},
            {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_cacheAccumulation.Get()}}},
            {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_cacheResolved.Get()}}},
        };
        constexpr UINT resolveGroups = kCacheCapacity / 256;
        m_list->ResourceBarrier(3, cacheBarriers);
        if (m_cacheClear) {
            DispatchGroups(Pass::SharcResolve, {.step = 1}, resolveGroups, 1, 1);
            m_list->ResourceBarrier(3, cacheBarriers);
            m_cacheClear = false;
        }
        const UINT blocksX = (m_gridWidth + kCacheBlock - 1) / kCacheBlock;
        const UINT blocksY = (m_gridHeight + kCacheBlock - 1) / kCacheBlock;
        DispatchGroups(Pass::SharcUpdate, {}, (blocksX + 7) / 8, (blocksY + 7) / 8, 1);
        m_list->ResourceBarrier(3, cacheBarriers);
        DispatchGroups(Pass::SharcResolve, {.step = 0}, resolveGroups, 1, 1);
        m_list->ResourceBarrier(3, cacheBarriers);
        std::copy_n(constants.eyePosition, 3, m_cacheCameraPrevious);
    }
    mark();
    if (volume && constants.traceMask != 0) {
        auto& last = m_probeHistory[previous];
        auto& next = m_probeHistory[current];
        for (Texture& texture : last)
            Use(texture, read);
        for (Texture& texture : next)
            Use(texture, write);
        // A thread group of 32 per probe to trace, a thread per probe to fill (shaders/probes.hlsl).
        const UINT probes = gbuffer::kProbeGrid[0] * gbuffer::kProbeGrid[1] * gbuffer::kProbeGrid[2];
        if (constants.bakedTrianglesView != kInvalidView) {
            for (Texture& texture : m_probeVolume)
                Use(texture, write);
            for (Texture& texture : m_probeIndirect)
                if (texture.resource)
                    Use(texture, write);
            DispatchGroups(Pass::Probes, {.output0 = m_probeVolume[0].uav, .output1 = m_probeVolume[1].uav, .output2 = m_probeVolume[2].uav,
                .output3 = m_probeIndirect[0].resource ? m_probeIndirect[0].uav : kInvalidView, .step = 2},
                probes / 32, 1, 1);
        } else {
            DispatchGroups(Pass::Probes, {.input0 = last[0].srv, .input1 = last[1].srv, .input2 = last[2].srv, .input3 = last[3].srv,
                .input4 = m_probeHistoryValid ? 1u : 0u, .output0 = next[0].uav, .output1 = next[1].uav, .output2 = next[2].uav, .output3 = next[3].uav,
                .step = 0}, probes, 1, 1);
            for (Texture& texture : next)
                Use(texture, read);
            for (Texture& texture : m_probeVolume)
                Use(texture, write);
            for (Texture& texture : m_probeIndirect)
                if (texture.resource)
                    Use(texture, write);
            DispatchGroups(Pass::Probes, {.input0 = next[0].srv, .input1 = next[1].srv, .input2 = next[2].srv, .input3 = next[3].srv,
                .output0 = m_probeVolume[0].uav, .output1 = m_probeVolume[1].uav, .output2 = m_probeVolume[2].uav,
                .output3 = m_probeIndirect[0].resource ? m_probeIndirect[0].uav : kInvalidView, .step = 1}, probes / 32, 1, 1);
        }
        for (size_t i = 0; i < probeTargets.size() && copyVolume; ++i) {
            Use(m_probeVolume[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_RESOURCE_BARRIER barrier = Transition(probeTargets[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            m_list->ResourceBarrier(1, &barrier);
            m_list->CopyResource(probeTargets[i], m_probeVolume[i].resource.Get());
            barrier = Transition(probeTargets[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            m_list->ResourceBarrier(1, &barrier);
            if (m_volumeReadback[slot]) {
                D3D12_TEXTURE_COPY_LOCATION destination = {m_volumeReadback[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                destination.PlacedFootprint = m_volumeFootprints[i];
                D3D12_TEXTURE_COPY_LOCATION source = {m_probeVolume[i].resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                m_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
        }
        m_volumeReadbackPending[slot] = copyVolume && m_volumeReadback[slot] != nullptr;
        m_volumeReadbackOrigin[slot] = m_probeOrigin;
        // The baked fill leaves the traced history behind.
        m_probeHistoryValid = constants.bakedTrianglesView == kInvalidView;
        volumeFilled = true;
        m_volumeReady = copyVolume;
        // Reflected hits take their indirect light from it where the radiance cache has none (shaders/reflection.hlsli).
        if (m_probeIndirect[0].resource) {
            for (Texture& texture : m_probeIndirect)
                Use(texture, kVolumeRead);
            constants.probeRedView = m_probeIndirect[0].srv;
            constants.probeGreenView = m_probeIndirect[1].srv;
            constants.probeBlueView = m_probeIndirect[2].srv;
            if (upscaled) {
                FrameConstants& render = *reinterpret_cast<FrameConstants*>(slotConstants + kConstantsStride);
                render.probeRedView = constants.probeRedView;
                render.probeGreenView = constants.probeGreenView;
                render.probeBlueView = constants.probeBlueView;
            }
        }
    }
    mark();
    Texture& direct = m_direct[current];
    Use(direct, write);
    Use(m_directRemainder, write);
    Use(m_specular, write);
    const RestirLights restirLights{constants.lightCount, constants.emitterCount, constants.dynamicLightCount, constants.projectorCount};
    restir = restir && constants.traceMask != 0;
    if (restir) {
        float previousInvViewProj[4][4] = {};
        const bool previousCamera = Invert(m_prevViewProj, previousInvViewProj);
        const bool restirHistory = previousCamera && constants.historyValid != 0 && m_restirLastMode == restirMode && m_restirLastSize[0] == m_gridWidth &&
            m_restirLastSize[1] == m_gridHeight;
        RestirConstants& restirConstants = *reinterpret_cast<RestirConstants*>(m_samplingMapped + slot * kSamplingStride + kRestirConstantsOffset);
        const UINT previousStyles = m_restir.UploadPreviousStyles(slot, m_lastLightStyles.data(), m_lastLightStyles.size());
        m_restir.FillConstants(restirConstants, static_cast<uint32_t>(inputs.frame), restirMode, rayReconstruction, constants.eyePosition, previousInvViewProj,
            restirLights, scene.EmitterPower(), previousStyles, restirHistory);
        m_restirLastMode = restirMode;
        m_restirLastSize[0] = m_gridWidth;
        m_restirLastSize[1] = m_gridHeight;
        m_lastLightStyles = scene.LightStyles();
    } else if (main) {
        m_restirLastMode.reset();
    }
    // The main view's sun shadows are soft, denoised by Ray Reconstruction or else by SIGMA; other views trace the sun's
    // centre in the direct pass.
    std::pair<UINT, UINT> sunShadow = {kInvalidView, 0};
    if (main && constants.traceMask != 0 && constants.sunEnabled != 0.0f)
        sunShadow = RecordSunShadow(constants, inputs, !rayReconstruction, slot, passConstantsGpu, frame.tlas);
    else if (main)
        m_sigmaHistory.valid = false;
    const bool wideLightSets = constants.exactLights > 4 || constants.sampledLightRays > 2;
    Dispatch(restir ? Pass::DirectSun : wideLightSets ? Pass::DirectWide : Pass::Direct,
        {.input0 = sunShadow.first, .input1 = sunShadow.second, .output0 = direct.uav, .output1 = m_directRemainder.uav, .output2 = m_specular.uav});
    if (probe) {
        D3D12_RESOURCE_BARRIER barrier = Transition(m_probeBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_list->ResourceBarrier(1, &barrier);
        m_list->CopyResource(m_probeReadback.Get(), m_probeBuffer.Get());
        barrier = Transition(m_probeBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_list->ResourceBarrier(1, &barrier);
    }
    mark();
    const bool restirDebug = restir && (constants.debugView == static_cast<uint32_t>(DebugView::LightSample) ||
                                           constants.debugView == static_cast<uint32_t>(DebugView::ReservoirM));
    if (restir && restirLights.Total() != 0) {
        const D3D12_RESOURCE_BARRIER written[] = {
            {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_directRemainder.resource.Get()}}},
            {D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_BARRIER_FLAG_NONE, {.UAV = {m_specular.resource.Get()}}},
        };
        m_list->ResourceBarrier(2, written);
        RecordRestir(restirMode, restirDebug);
    }
    constants.restirDebugView = restirDebug && restirLights.Total() != 0 ? m_restirDebug.srv : kInvalidView;
    if (upscaled)
        reinterpret_cast<FrameConstants*>(slotConstants + kConstantsStride)->restirDebugView = constants.restirDebugView;
    Use(m_specular, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Use(direct, read);
    Use(m_directRemainder, read);
    Use(m_direct[previous], read);
    mark();
    if (ambientOcclusion && constants.traceMask != 0) {
        Use(m_aoHistory[previous], read);
        Use(m_aoHistory[current], write);
        Dispatch(Pass::Ao, {.input0 = main ? m_aoHistory[previous].srv : kInvalidView, .output0 = m_aoHistory[current].uav, .step = 0});
        Use(m_aoHistory[current], read);
        Use(m_ao, write);
        Dispatch(Pass::Ao, {.input0 = m_aoHistory[current].srv, .output0 = m_ao.uav, .step = 1});
        Use(m_ao, read);
        constants.aoView = m_ao.srv;
        if (upscaled)
            reinterpret_cast<FrameConstants*>(slotConstants + kConstantsStride)->aoView = m_ao.srv;
    }
    mark();
    PassArgs trace = {.input1 = m_directRemainder.srv, .input2 = m_direct[previous].srv, .output0 = m_giRaw.uav};
    // Untraced views and the distant 3D skybox (traced for reflections and sun shadows only) take the sky's
    // light as their indirect light.
    const bool traced = constants.traceMask != 0 && inputs.kind != gbuffer::ViewKind::Skybox;
    // Without Ray Reconstruction NVIDIA NRD denoises the main view's indirect light and reflections, unless the built-in
    // denoiser is chosen.
    // AMD FSR Ray Regeneration where chosen and it runs, otherwise NRD RELAX in its place.
    const DenoiserType denoiserType = GetSettings().denoiserType;
    const bool regen = main && traced && !rayReconstruction && GetSettings().denoiser && denoiserType == DenoiserType::RayRegeneration &&
        m_rayRegeneration.Usable();
    const bool nrd = main && traced && !rayReconstruction && !regen && GetSettings().denoiser && denoiserType != DenoiserType::BuiltIn &&
        EnsureNrd(denoiserType == DenoiserType::RayRegeneration ? DenoiserType::Relax : denoiserType);
    if (traced) {
        if (!main) {
            if (constants.probeRedView != kInvalidView) {
                for (Texture& texture : m_probeIndirect)
                    Use(texture, kVolumeRead);
            }
            trace.step = kViewmodelIndirectSamples;
        } else {
            if (m_lastDenoised != nullptr) {
                Use(*m_lastDenoised, read);
                trace.input0 = m_lastDenoised->srv;
            }
            trace.step = static_cast<UINT>(GetSettings().indirectRays);
        }
        Use(m_giRaw, write);
        Dispatch(Pass::GiTrace, trace);
        Use(m_giRaw, read);
    }
    mark();
    // Reflections and their guides (Ray Reconstruction's and NRD's). Their fade by the pixel's light takes the main view's
    // denoised light of last frame, as this frame's is denoised with the reflections.
    UINT reflections = kInvalidView;
    const bool reflectionGuides = rayReconstruction || nrd || regen;
    if (constants.traceMask != 0) {
        Use(m_reflections, write);
        Use(m_selfIllum, write);
        Use(m_direct[current], read);
        PassArgs args = {.input2 = m_direct[current].srv, .output0 = m_reflections.uav, .output4 = m_selfIllum.uav};
        if (main && m_lastDenoised != nullptr) {
            Use(*m_lastDenoised, read);
            args.input1 = m_lastDenoised->srv;
        }
        if (reflectionGuides) {
            Use(m_rrSpecularAlbedo, write);
            Use(m_rrRoughness, write);
            Use(m_rrHitDistance, write);
            args.output1 = m_rrSpecularAlbedo.uav;
            args.output2 = m_rrRoughness.uav;
            args.output3 = m_rrHitDistance.uav;
        }
        Dispatch(Pass::Reflections, args);
        Use(m_reflections, read);
        Use(m_selfIllum, read);
        reflections = m_reflections.srv;
        constants.selfIllumView = m_selfIllum.srv;
        // Ray Reconstruction takes the raw reflections with their guides, and NRD denoises them below.
        if (main && !rayReconstruction && !nrd && !regen) {
            Use(m_reflectionHistory[previous], read);
            Use(m_reflectionHistory[current], write);
            Dispatch(Pass::ReflectionTemporal, {.input0 = m_reflections.srv, .input1 = m_reflectionHistory[previous].srv,
                .output0 = m_reflectionHistory[current].uav, .step = m_reflectionHistoryValid ? 1u : 0u});
            Use(m_reflectionHistory[current], read);
            reflections = m_reflectionHistory[current].srv;
        }
    }
    if (main)
        m_reflectionHistoryValid = reflections != kInvalidView && !rayReconstruction && !nrd && !regen;
    mark();
    // Accumulation and denoising of the indirect light (with NRD, also of the reflections).
    if (traced && main && !nrd) {
        Use(m_giHistory[previous], read);
        Use(m_giMoments[previous], read);
        Use(m_giHistory[current], write);
        Use(m_giMoments[current], write);
        Dispatch(Pass::GiTemporal, {.input0 = m_giRaw.srv, .input1 = m_giHistory[previous].srv, .input2 = m_giMoments[previous].srv,
            .output0 = m_giHistory[current].uav, .output1 = m_giMoments[current].uav});
        Use(m_giMoments[current], read);
    }
    mark();
    Texture* indirect = nullptr;
    if (traced && !main) {
        Texture* source = &m_giRaw;
        for (size_t i = 0; i < std::size(kFilterSteps) && GetSettings().denoiser; ++i) {
            Texture* target = &m_viewmodelIndirect[i % 2];
            Use(*source, read);
            Use(*target, write);
            Dispatch(Pass::GiFilter, {.input0 = source->srv, .input2 = i == 0 ? 1u : 0u, .output0 = target->uav, .step = kFilterSteps[i]});
            source = target;
        }
        indirect = source;
    } else if (traced && regen) {
        ID3D12Resource* passAlbedo = upscaled ? m_rrInputs[RrAlbedo].resource.Get() : albedo;
        if (RecordRayRegeneration(constants, inputs, passAlbedo, slot, current, passConstantsGpu, frame.tlas)) {
            indirect = &m_giFiltered[0];
            reflections = m_reflectionHistory[current].srv;
        } else {
            // This frame keeps the raw light; the next uses NRD.
            indirect = &m_giRaw;
        }
    } else if (traced && nrd) {
        if (RecordNrd(constants, inputs, slot, current, passConstantsGpu, frame.tlas)) {
            indirect = &m_giFiltered[0];
            reflections = m_reflectionHistory[current].srv;
            if (constants.debugView == static_cast<uint32_t>(DebugView::DenoiserValidation))
                constants.nrdValidationView = m_nrdValidation.srv;
        } else {
            // This frame keeps the raw light; the next uses the built-in denoiser.
            indirect = &m_giRaw;
        }
    } else if (traced) {
        Texture* source = &m_giHistory[current];
        // Ray Reconstruction denoises the raw estimate itself; the accumulation still serves indirect rays.
        for (size_t i = 0; i < std::size(kFilterSteps) && GetSettings().denoiser && !rayReconstruction; ++i) {
            Texture* target = &m_giFiltered[i % 2];
            Use(*source, read);
            Use(*target, write);
            Dispatch(Pass::GiFilter, {.input0 = source->srv, .input1 = i == 0 ? m_giMoments[current].srv : kInvalidView, .input2 = i == 0 ? 1u : 0u,
                .output0 = target->uav, .step = kFilterSteps[i]});
            source = target;
        }
        indirect = source;
    }
    if (main && !nrd)
        m_nrdHistory.valid = false;
    if (upscaled) {
        FrameConstants& render = *reinterpret_cast<FrameConstants*>(slotConstants + kConstantsStride);
        render.selfIllumView = constants.selfIllumView;
        render.nrdValidationView = constants.nrdValidationView;
    }
    mark();
    if (main)
        m_lastDenoised = indirect;
    Texture* shaded = indirect != nullptr && rayReconstruction ? &m_giRaw : indirect;
    bool froxels = false;
    if (volumetrics && constants.traceMask != 0) {
        // Columns of at least kFroxelTile pixels and at most kFroxelRows rows, so the volume's size stays bounded at
        // high resolutions.
        const UINT tile = std::max(kFroxelTile, (m_gridHeight + kFroxelRows - 1) / kFroxelRows);
        std::array<UINT, 3> dims = {(m_gridWidth + tile - 1) / tile, (m_gridHeight + tile - 1) / tile, static_cast<UINT>(GetSettings().volumetricSlices)};
        if (main)
            m_mainFroxelDims = dims;
        else if (m_mainFroxelDims[0] != 0)
            dims = m_mainFroxelDims;
        froxels = EnsureFroxels(dims);
        if (froxels) {
            // The slices span what the medium covers: the water the eye is in as far as it lets light through (four
            // mean free paths), the map's fog from where it starts to where it stops thickening, or the haze.
            FroxelMedium medium = FroxelMedium::Haze;
            float start = 0.0f;
            float range = kHazeDistance;
            const std::span<const GpuWaterBody> bodies = scene.WaterBodies();
            if (constants.eyeWater != kInvalidView && constants.eyeWater < bodies.size()) {
                medium = FroxelMedium::Water;
                const float* extinction = bodies[constants.eyeWater].extinction;
                range = std::min(kHazeDistance, 4.0f / std::max(std::min({extinction[0], extinction[1], extinction[2]}), 1e-6f));
            } else if (litFog && constants.fogParams[3] > 0.0f) {
                // The fog follows the distance from the eye (FogOpacity), so toward the screen's corners it starts at a
                // smaller view depth.
                medium = FroxelMedium::Fog;
                float cornerScale = 1.0f;
                for (const float x : {-1.0f, 1.0f}) {
                    for (const float y : {-1.0f, 1.0f}) {
                        const float clip[4] = {x, y, 0.5f, 1.0f};
                        float point[4] = {};
                        for (int r = 0; r < 4; ++r) {
                            for (int c = 0; c < 4; ++c)
                                point[r] += constants.clipToEye[r][c] * clip[c];
                        }
                        float offset[3];
                        for (int i = 0; i < 3; ++i)
                            offset[i] = point[i] / point[3];
                        const float viewDepth = constants.viewProj[3][0] * offset[0] + constants.viewProj[3][1] * offset[1] + constants.viewProj[3][2] * offset[2];
                        if (viewDepth > 0.0f)
                            cornerScale = std::max(cornerScale, std::sqrt(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]) / viewDepth);
                    }
                }
                start = std::max(constants.fogParams[0] / constants.fogParams[3], 0.0f) / cornerScale;
                const float end = (std::min(constants.fogParams[2], 1.0f) + constants.fogParams[0]) / constants.fogParams[3];
                range = std::clamp(end - start, 1.0f, kMaxFogDistance);
            }
            // The main view jitters its sample points within their froxels (Halton 2, 3, 5) and keeps their history
            // while the medium stays the same; the 3D skybox samples the centres afresh.
            const bool history = main && m_volumetricHistoryValid && m_froxelMedium == static_cast<int>(medium);
            const auto halton = [](uint64_t index, uint64_t base) {
                float result = 0.0f;
                float fraction = 1.0f / static_cast<float>(base);
                for (uint64_t i = index + 1; i > 0; i /= base, fraction /= static_cast<float>(base))
                    result += fraction * static_cast<float>(i % base);
                return result;
            };
            const std::array<float, 3> jitter = main ? std::array<float, 3>{halton(m_mainRuns % 1024, 2), halton(m_mainRuns % 1024, 3), halton(m_mainRuns % 1024, 5)}
                                                     : std::array<float, 3>{0.5f, 0.5f, 0.5f};
            FrameConstants* blocks[] = {&constants, upscaled ? reinterpret_cast<FrameConstants*>(slotConstants + kConstantsStride) : nullptr};
            for (FrameConstants* block : blocks) {
                if (block == nullptr)
                    continue;
                std::copy(dims.begin(), dims.end(), block->froxelDims);
                block->froxelUvScale[0] = static_cast<float>(m_gridWidth) / static_cast<float>(tile * dims[0]);
                block->froxelUvScale[1] = static_cast<float>(m_gridHeight) / static_cast<float>(tile * dims[1]);
                block->froxelStart = start;
                block->froxelRange = range;
                std::copy(jitter.begin(), jitter.end(), block->froxelJitter);
                block->prevFroxelStart = m_prevFroxelStart;
                block->prevFroxelRange = m_prevFroxelRange;
                block->volumetricFog = litFog ? 1u : 0u;
            }
            // Lit fog darkens where the irradiance volume's indirect light is weaker than the open sky's.
            const bool indirectVolume = volumeFilled && m_probeIndirect[0].resource;
            if (indirectVolume) {
                for (Texture& texture : m_probeIndirect)
                    Use(texture, kVolumeRead);
            }
            // The 3D skybox, lit before the main view, writes the slot the main view writes next, never its last.
            Texture& last = m_froxelLight[previous];
            Texture& froxelLight = m_froxelLight[current];
            const UINT groupsX = (dims[0] + 7) / 8;
            const UINT groupsY = (dims[1] + 7) / 8;
            Use(last, read);
            Use(froxelLight, write);
            Use(m_froxelProjected, write);
            // Map and dynamic lights scatter in the main view's fog and water; the 3D skybox is lit without them.
            DispatchGroups(Pass::Volumetrics, {.input0 = last.srv, .input1 = indirectVolume ? m_probeIndirect[0].srv : kInvalidView,
                .input2 = indirectVolume ? m_probeIndirect[1].srv : kInvalidView, .input3 = indirectVolume ? m_probeIndirect[2].srv : kInvalidView,
                .input4 = (history ? 1u : 0u) | (main ? 2u : 0u), .output0 = froxelLight.uav, .output1 = m_froxelProjected.uav, .step = 0}, groupsX, groupsY, dims[2]);
            Use(froxelLight, read);
            Use(m_froxelProjected, read);
            Use(m_froxelIntegrated, write);
            DispatchGroups(Pass::Volumetrics, {.input0 = froxelLight.srv, .input1 = m_froxelProjected.srv, .output0 = m_froxelIntegrated.uav, .step = 1},
                groupsX, groupsY, 1);
            // The game's translucent draws read it too (shaders/game_fog.hlsli).
            Use(m_froxelIntegrated, kVolumeRead);
            for (FrameConstants* block : blocks) {
                if (block != nullptr)
                    block->volumetricView = m_froxelIntegrated.srv;
            }
            if (main) {
                m_froxelMedium = static_cast<int>(medium);
                m_prevFroxelStart = start;
                m_prevFroxelRange = range;
            }
        }
        // Translucent surfaces drawn by the game take the lit fog's colour from the full irradiance volume
        // (shaders/forward_light.hlsli), relative to its average in open sunlit air: a quarter of the sun's irradiance
        // and half the sky's.
        if (main && litFog) {
            const auto luminance = [](const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
            const float openAir = (constants.sunEnabled != 0.0f ? 0.25f * luminance(constants.sunColor) : 0.0f) + 0.5f * luminance(constants.skyColor);
            m_litFogScale = 1.0f / std::max(openAir, 1e-6f);
        }
    }
    if (main)
        m_volumetricHistoryValid = froxels && constants.historyValid != 0;
    if (shaded != nullptr)
        Use(*shaded, read);
    Use(m_giRaw, read);
    Use(m_lit, write);
    PassArgs lighting = {.input0 = shaded != nullptr ? shaded->srv : kInvalidView, .input1 = m_giRaw.srv, .input2 = reflections, .input3 = direct.srv,
        .input4 = m_directRemainder.srv, .output0 = m_lit.uav};
    if (rayReconstruction) {
        // Water pixels overwrite the reflection guides the reflections pass wrote, and traced water the albedo guide.
        Use(m_rrSpecularAlbedo, write);
        Use(m_rrRoughness, write);
        Use(m_rrHitDistance, write);
        Use(m_rrInputs[RrAlbedoGuide], write);
        D3D12_RESOURCE_BARRIER guides[3] = {};
        ID3D12Resource* guideResources[] = {m_rrSpecularAlbedo.resource.Get(), m_rrRoughness.resource.Get(), m_rrHitDistance.resource.Get()};
        for (size_t i = 0; i < std::size(guides); ++i) {
            guides[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            guides[i].UAV.pResource = guideResources[i];
        }
        m_list->ResourceBarrier(static_cast<UINT>(std::size(guides)), guides);
        lighting.output1 = m_rrSpecularAlbedo.uav;
        lighting.output2 = m_rrRoughness.uav;
        lighting.output3 = m_rrHitDistance.uav;
        lighting.output4 = m_rrInputs[RrAlbedoGuide].uav;
    }
    Dispatch(Pass::Lighting, lighting);
    mark();
    // The 3D skybox is drawn before the main view, which covers it again, so only the main view is resolved.
    Texture* output = &m_lit;
    bool rayReconstructed = false;
    if (rayReconstruction) {
        Use(m_lit, read);
        Use(m_surface[current], read);
        Use(m_rrSpecularAlbedo, read);
        Use(m_rrRoughness, read);
        Use(m_rrHitDistance, read);
        Use(m_rrInputs[RrAlbedoGuide], read);
        Use(m_rrOutput, write);
        SplitViewProjection(constants.viewProj, constants.eyePosition, m_worldToView, m_viewToClip);
        const RayReconstruction::Inputs rr{m_lit.resource.Get(), m_rrInputs[RrDepth].resource.Get(), m_motion.resource.Get(),
            m_surface[current].resource.Get(), m_rrInputs[RrAlbedoGuide].resource.Get(), m_rrSpecularAlbedo.resource.Get(), m_rrRoughness.resource.Get(),
            m_rrHitDistance.resource.Get(), m_rrOutput.resource.Get(), m_worldToView, m_viewToClip, renderWidth, renderHeight, inputs.jitterX,
            inputs.jitterY, !m_historyValid};
        rayReconstructed = m_rayReconstruction.Evaluate(m_list.Get(), GetSettings().upscaling, m_width, m_height, rr);
        // NGX leaves its own descriptor heap and root signature bound.
        BindCompute(slotConstantsGpu, frame.tlas, slot);
        m_gridWidth = m_width;
        m_gridHeight = m_height;
        if (rayReconstructed) {
            Use(m_rrOutput, read);
            Use(m_resolved, write);
            if (resolveSkyboxDistance)
                Use(m_skyboxDistance, read);
            Dispatch(Pass::DlssResolve, {.input0 = m_rrOutput.srv, .input1 = shiftX, .input2 = shiftY, .input3 = resolveSkybox, .output0 = m_resolved.uav});
            output = &m_resolved;
        }
    } else if (upscaler == Upscaler::Fsr) {
        Use(m_lit, read);
        Use(m_rrOutput, write);
        const auto now = std::chrono::steady_clock::now();
        const float frameMs = m_lastMainTime.time_since_epoch().count() != 0
            ? static_cast<float>(std::chrono::duration<double, std::milli>(now - m_lastMainTime).count())
            : 16.7f;
        m_lastMainTime = now;
        // Source's projection (c8-c11): the w row is the view's forward axis and the z row maps view depth z to device
        // depth a + b / z, which gives the near and far planes; the y row's part across the forward axis is the
        // vertical scale, 1 / tan(half the vertical field of view).
        const float(*m)[4] = constants.viewProj;
        const auto dot3 = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
        const float forward = std::sqrt(dot3(m[3], m[3]));
        const float a = dot3(m[2], m[3]) / std::max(forward * forward, 1e-12f);
        const float b = (m[2][3] - a * m[3][3]) / std::max(forward, 1e-12f);
        const float along = dot3(m[1], m[3]) / std::max(forward, 1e-12f);
        const float across = std::sqrt(std::max(dot3(m[1], m[1]) - along * along, 1e-12f));
        const bool reversed = b > 0.0f;
        // d = a + b / z: 0 (or 1 reversed) at the near plane, 1 (or 0) at the far one.
        const float nearPlane = std::fabs(b / ((reversed ? 1.0f : 0.0f) - a));
        const float farPlane = std::fabs(b / ((reversed ? 0.0f : 1.0f) - a));
        const FsrUpscaler::Inputs fsr{m_lit.resource.Get(), m_rrInputs[RrDepth].resource.Get(), m_motion.resource.Get(), m_rrOutput.resource.Get(),
            renderWidth, renderHeight, m_width, m_height, inputs.jitterX, inputs.jitterY, frameMs, nearPlane, farPlane, 2.0f * std::atan(1.0f / across),
            reversed, !m_historyValid};
        const bool upscaledLight = m_fsr.Evaluate(m_list.Get(), fsr);
        // FSR leaves its own descriptor heaps bound.
        BindCompute(slotConstantsGpu, frame.tlas, slot);
        m_gridWidth = m_width;
        m_gridHeight = m_height;
        if (upscaledLight) {
            Use(m_rrOutput, read);
            Use(m_resolved, write);
            if (resolveSkyboxDistance)
                Use(m_skyboxDistance, read);
            Dispatch(Pass::DlssResolve, {.input0 = m_rrOutput.srv, .input1 = shiftX, .input2 = shiftY, .input3 = resolveSkybox, .output0 = m_resolved.uav});
            output = &m_resolved;
        }
    } else if (main && AntiAliasingMode() == AntiAliasing::Temporal) {
        Use(m_lit, read);
        Use(m_taaHistory[previous], read);
        Use(m_taaHistory[current], write);
        Dispatch(Pass::TemporalAntiAliasing, {.input0 = m_lit.srv, .input1 = m_taaHistory[previous].srv, .output0 = m_taaHistory[current].uav,
            .step = m_taaHistoryValid ? 1u : 0u});
        Use(m_taaHistory[current], read);
        Use(m_resolved, write);
        Dispatch(Pass::Sharpen, {.input0 = m_taaHistory[current].srv, .input1 = shiftX, .input2 = shiftY, .output0 = m_resolved.uav});
        output = &m_resolved;
    }
    m_taaHistoryValid = main ? output == &m_resolved && !upscaled : m_taaHistoryValid;
    // The game draws the view's translucent surfaces next, tested against this depth, which edges keep jittered.
    const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
    const bool stableDepth = main && output == &m_resolved && (depthDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 &&
        EnsureDepthPipeline(DepthStencilFormat(depthDesc.Format));
    if (stableDepth) {
        // The jitter in output pixels. It moves an edge up to half a render pixel (scale / 2) either way, so a pixel the
        // edge can cross may see the other side's samples up to `scale` pixels away.
        const float scale = static_cast<float>(m_width) / static_cast<float>(renderWidth);
        const float shift[2] = {inputs.jitterX * scale, inputs.jitterY * scale};
        Use(m_translucentDepth, write);
        Dispatch(Pass::TranslucentDepth, {.input0 = std::bit_cast<UINT>(shift[0]), .input1 = std::bit_cast<UINT>(shift[1]), .output0 = m_translucentDepth.uav,
            .step = static_cast<UINT>(std::floor(scale)) + 1});
        Use(m_translucentDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    Use(*output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    m_rectDispatch = false;
    m_list->SetGraphicsRootSignature(m_compositeRoot.Get());
    // The viewmodel's run lit only a rectangle, so its composite keeps to the pixels its G-buffer covers.
    const bool covered = inputs.kind != gbuffer::ViewKind::Main;
    const D3D12_RESOURCE_STATES pixelRead = read | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (covered) {
        const D3D12_RESOURCE_BARRIER barrier = Transition(flags, read, pixelRead);
        m_list->ResourceBarrier(1, &barrier);
    }
    m_list->SetPipelineState(covered ? m_compositeCoveredPipeline.Get() : m_compositePipeline.Get());
    m_list->SetGraphicsRootDescriptorTable(0, m_descriptors->Gpu(output->srv));
    if (covered)
        m_list->SetGraphicsRootDescriptorTable(1, m_descriptors->Gpu(constants.flagsView));
    m_list->RSSetViewports(1, &viewport);
    m_list->RSSetScissorRects(1, &scissor);
    m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_list->DrawInstanced(3, 1, 0, 0);
    if (covered) {
        const D3D12_RESOURCE_BARRIER barrier = Transition(flags, pixelRead, read);
        m_list->ResourceBarrier(1, &barrier);
    }
    if (stableDepth) {
        D3D12_RESOURCE_BARRIER barrier = Transition(depth, read, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_list->ResourceBarrier(1, &barrier);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = m_depthPipelineFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateDepthStencilView(depth, &dsvDesc, dsv);
        m_list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        m_list->SetPipelineState(m_depthPipeline.Get());
        m_list->SetGraphicsRootDescriptorTable(0, m_descriptors->Gpu(m_translucentDepth.srv));
        m_list->DrawInstanced(3, 1, 0, 0);
        barrier = Transition(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, read);
        m_list->ResourceBarrier(1, &barrier);
    }

    D3D12_RESOURCE_BARRIER restore[] = {
        Transition(color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON),
        Transition(depth, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(normal, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(flags, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(light, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(albedo, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(emissive, read, D3D12_RESOURCE_STATE_COMMON),
        Transition(m_colorCopy.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    m_list->ResourceBarrier(static_cast<UINT>(std::size(restore)), restore);
    for (size_t i = kInputViews; i < resources.size(); ++i) {
        D3D12_RESOURCE_BARRIER barrier = Transition(resources[i], read, D3D12_RESOURCE_STATE_COMMON);
        m_list->ResourceBarrier(1, &barrier);
    }
    mark();
    if (main && m_volumeReady && m_probeIndirect[0].resource) {
        // Forward draws read the indirect volume in their own command lists.
        for (Texture& texture : m_probeIndirect)
            Use(texture, kVolumeRead);
        FrameConstants& forward = *reinterpret_cast<FrameConstants*>(m_forwardConstantsMapped + slot * kConstantsStride);
        forward = constants;
        forward.probeRedView = m_probeIndirect[0].srv;
        forward.probeGreenView = m_probeIndirect[1].srv;
        forward.probeBlueView = m_probeIndirect[2].srv;
        m_forwardFrame = m_forwardConstants->GetGPUVirtualAddress() + slot * kConstantsStride;
        m_forwardScene = frame.tlas;
    }
    if (main) {
        // The camera effects read the main view's motion in the game's command list (PostView), after the viewmodel's
        // run has written its own over part of m_motion.
        Use(m_motion, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Use(m_postMotion, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to = {m_postMotion.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION from = {m_motion.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        const D3D12_BOX box = {0, 0, 0, renderWidth, renderHeight, 1};
        m_list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        Use(m_postMotion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        PostView& post = m_postView.emplace();
        post.frame = inputs.frame;
        post.viewProj = inputs.viewProj;
        post.light = flareLight;
        if (flareLight)
            post.lightDirection = *flareDirection;
        for (size_t i = 0; i < 3; ++i)
            post.lightColor[i] = constants.sunColor[i] * constants.tonemapScale;
        post.lightRadius = flareRadius;
        post.motionView = m_postMotion.srv;
        std::copy_n(constants.eyePosition, 3, post.eye);
        SplitViewProjection(constants.viewProj, constants.eyePosition, post.worldToView, post.viewToClip);
        post.motionScale = {static_cast<float>(renderWidth) / static_cast<float>(m_width), static_cast<float>(renderHeight) / static_cast<float>(m_height)};
        post.flareVisibilityView = flareLight ? m_flareVisibility.srv : kInvalidView;
    }
    m_list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kMarkers, kMarkers, m_timestampReadback.Get(),
        slot * kMarkers * sizeof(uint64_t));
    HRESULT hr = m_list->Close();
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {m_list.Get()};
        queue->ExecuteCommandLists(1, lists);
    } else {
        log::Info("Lighting: closing the command list failed ({:#010x})", Hex(hr));
        m_failed = true;
    }
    queue->Signal(m_fence.Get(), ++m_fenceValue);
    m_slotFenceValues[slot] = m_fenceValue;
    if (reuseScene)
        m_slotFenceValues[m_mainSceneSlot] = m_fenceValue;
    if (std::exchange(m_bakeLogPending, false))
        FinishBake(scene);
    if (probe) {
        m_probeRequested = false;
        WaitForGpu();
        uint32_t data[kProbeBytes / 4] = {};
        void* mapped = nullptr;
        const D3D12_RANGE range = {0, kProbeBytes};
        if (SUCCEEDED(m_probeReadback->Map(0, &range, &mapped))) {
            std::memcpy(data, mapped, sizeof(data));
            const D3D12_RANGE written = {0, 0};
            m_probeReadback->Unmap(0, &written);
            ProbeResult result = {data[0], data[1], std::bit_cast<float>(data[2]), std::bit_cast<float>(data[3]),
                {std::bit_cast<float>(data[4]), std::bit_cast<float>(data[5]), std::bit_cast<float>(data[6])}, -1, 0};
            if (data[15] == 1) {
                result.water = true;
                for (int i = 0; i < 3; ++i) {
                    result.drawnWaterNormal[i] = std::bit_cast<float>(data[8 + i]);
                    result.rebuiltWaterNormal[i] = std::bit_cast<float>(data[12 + i]);
                }
            }
            std::tie(result.model, result.mask) = scene.InstanceInfo(result.instance);
            m_probe = result;
        }
    }
    ++m_run;
    if (main) {
        std::memcpy(m_prevViewProj, constants.viewProj, sizeof(m_prevViewProj));
        std::memcpy(m_prevEye, constants.eyePosition, sizeof(m_prevEye));
        m_historyValid = constants.traceMask != 0;
        m_taaHistoryValid = !upscaled && (m_taaHistoryValid && constants.historyValid != 0 ? true : output == &m_resolved);
        m_lastMainFrame = inputs.frame;
        ++m_mainRuns;
    }
}

}
