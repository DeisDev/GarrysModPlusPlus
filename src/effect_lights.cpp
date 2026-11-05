#include "effect_lights.h"

#include "entities.h"
#include "glow_draws.h"
#include "log.h"
#include "module_scan.h"
#include "options.h"
#include "settings.h"
#include "studio_model.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace gmodpp::effect_lights {

namespace {

// GMod's particle systems (PCF), read after the client simulated them for the frame: OnRenderStart (FRAME_RENDER_START,
// client.dll+0x207e00) calls CParticleMgr::Simulate (+0x297310), whose threaded jobs (r_threaded_particles) are all
// finished before it returns. REA 2026-11-04, client.dll sha256 dda3f6b7:
// - g_ParticleMgr (+0xa2ff80) is built on first use by ParticleMgr() (+0x295c00); its constructor (+0x291c60) writes the
//   CParticleMgr vtable at +0, so it is found as the one object in the client's data with that vtable. The new effects'
//   list (m_NewEffects) starts at +0x60.
// - A CNewParticleEffect (0x2320 bytes) holds the next effect at +0x21c8, its flags at +0x21e0 (0x02: to be removed),
//   whether it is drawn with the viewmodel at +0x2310 and GMod's draw switch at +0x2318 (CNewParticleEffect::DrawModel,
//   +0x29a490, draws only while it is set); its CParticleCollection is at +0x20.
// - A CParticleCollection holds its live particles' count at +0x3c, its definition at +0x60 (null for none), whether it
//   is dormant at +0x70, its first child at +0x1cc0 and its next sibling at +0x1c88. Its particles' attributes are
//   pointers at +0x1d20 with strides in floats at +0x1f20 (size_t each); particle i's attribute a starts
//   stride[a] * (i / 4) + i % 4 floats into attr[a], a vector's components 4 floats apart (CParticleCollection::InitStorage
//   +0x43af90, C_INIT_ColorLitPerParticle +0x473f30). Attributes: 0 position, 3 radius, 6 tint, 7 alpha, 16 alpha2.
// - A CParticleSystemDefinition holds its material's name (char[260]) at +0x48 (the DMX field "material", +0x89f3d0).
constexpr size_t kManagerEffects = 0x60;
constexpr size_t kEffectNext = 0x21C8;
constexpr size_t kEffectFlags = 0x21E0;
constexpr uint8_t kEffectRemove = 0x02;
constexpr size_t kEffectViewModel = 0x2310;
constexpr size_t kEffectDrawn = 0x2318;
constexpr size_t kEffectCollection = 0x20;
constexpr size_t kCollectionCount = 0x3C;
constexpr size_t kCollectionDefinition = 0x60;
constexpr size_t kCollectionDormant = 0x70;
constexpr size_t kCollectionChildren = 0x1CC0;
constexpr size_t kCollectionSibling = 0x1C88;
constexpr size_t kCollectionAttributes = 0x1D20;
constexpr size_t kCollectionStrides = 0x1F20;
constexpr size_t kDefinitionMaterial = 0x48;
constexpr size_t kMaterialNameBytes = 260;
constexpr size_t kAttributePosition = 0;
constexpr size_t kAttributeRadius = 3;
constexpr size_t kAttributeTint = 6;
constexpr size_t kAttributeAlpha = 7;
constexpr size_t kAttributeAlpha2 = 16;

// Old-style particle effects (CParticleEffectBinding), read after CParticleMgr::Simulate as for the PCF systems. REA
// 2026-11-05, client.dll sha256 dda3f6b7 (CParticleMgr::Simulate +0x297310, CSimpleEmitter::RenderParticles +0x29c0c0,
// CLuaEmitter's render +0x107cc0):
// - The manager's m_Effects is a CUtlLinkedList: its elements (16 bytes: the binding, then the previous and next indices
//   as u16 at +8 and +0xa) at the pointer at +0x38, the head index (u16) at +0x48, 0xffff ending it. Simulate removes
//   bindings marked for removal before it returns.
// - A binding (CParticleEffectBinding) holds its local space transform (VMatrix, rows) at +0x18, whether that is the
//   identity at +0x58 (a particle's world position is the transform of its m_Pos otherwise), its IParticleEffect at
//   +0x98, its flags at +0xa8 (1: to be removed) and its materials, a CUtlLinkedList like the manager's, at +0xf0 (head
//   at +0x100). A CEffectMaterial holds its CParticleSubTextureGroup at +0 (whose IMaterial is at +0) and a sentinel
//   particle at +8, whose next is at +0x10; a particle's next is at +8 and its position (m_Pos) at +0x18.
// - CSimpleEmitter and its subclasses (CLocalSpaceEmitter for muzzle flashes, CExplosionParticle) give a SimpleParticle's
//   drawn alpha (0-1), size (half the quad's side, units) and colour (0-1) through their virtuals UpdateAlpha (slot 13),
//   UpdateScale (14) and UpdateColor (17), which subclasses override.
// - Lua's CLuaEmitter draws a CLuaParticle once it has started (flags at +0x78, 0x40), at t = age (+0x60) / die time
//   (+0x5c), 1 when the die time is 0, between 0 and 1: alpha lerped from +0x6b to +0x6c (bytes), size from +0x70 to
//   +0x74 (floats), colour the bytes at +0x68, lit by the world when its flag 0x10 is set (it then gives no light).
constexpr size_t kManagerBindings = 0x38;
constexpr size_t kManagerBindingsHead = 0x48;
constexpr size_t kListElement = 0x10;
constexpr size_t kListNext = 0xA;
constexpr uint16_t kListEnd = 0xFFFF;
constexpr size_t kBindingTransform = 0x18;
constexpr size_t kBindingIdentity = 0x58;
constexpr size_t kBindingEffect = 0x98;
constexpr size_t kBindingFlags = 0xA8;
constexpr uint8_t kBindingRemove = 0x01;
constexpr size_t kBindingMaterials = 0xF0;
constexpr size_t kBindingMaterialsHead = 0x100;
constexpr size_t kEffectMaterialSentinel = 0x8;
constexpr size_t kParticleNext = 0x8;
constexpr size_t kParticlePosition = 0x18;
constexpr int kUpdateAlphaSlot = 13;
constexpr int kUpdateScaleSlot = 14;
constexpr int kUpdateColorSlot = 17;
constexpr size_t kLuaDieTime = 0x5C;
constexpr size_t kLuaAge = 0x60;
constexpr size_t kLuaColor = 0x68;
constexpr size_t kLuaStartAlpha = 0x6B;
constexpr size_t kLuaEndAlpha = 0x6C;
constexpr size_t kLuaStartSize = 0x70;
constexpr size_t kLuaEndSize = 0x74;
constexpr size_t kLuaFlags = 0x78;
constexpr uint8_t kLuaLit = 0x10;
constexpr uint8_t kLuaStarted = 0x40;
constexpr int kMaxBindings = 4096;
constexpr int kMaxBindingParticles = 16384;

// More than this in one collection, or more nested children, means the structures are not what they are thought to be.
constexpr int kMaxParticles = 65536;
constexpr int kMaxDepth = 8;
// How often the particle manager is looked for again while the client has not built it.
constexpr uint32_t kManagerRetryFrames = 120;

// Glow an engine light or an effect read before the view was lit lies within this many units of is left to it: the
// effect already has its light. Drawn glow is matched against the effects read the frame it was drawn.
constexpr float kEffectCover = 32.0f;
// A glow within this many units of a map light is that lamp's halo (Settings::skipLampGlows).
constexpr float kLampHaloReach = 24.0f;
// Glowing particle systems' and drawn glow's lights are spheres at least and at most this big.
constexpr float kMinGlowSize = 1.0f;
constexpr float kMaxGlowSize = 64.0f;
// Elights are lit as spheres this big.
constexpr float kElightSize = 2.0f;
// Where Source's falloff of an elight, (1 - (d / r)^2)^2, is matched by the inverse square: at half its radius.
constexpr float kElightMatch = 0.5f;
// An elight within this many units of the local player is its own weapon's, whose shadow rays pass the player.
constexpr float kLocalPlayerReach = 96.0f;
// Dynamic light keys (FrameLight::key) carry what made the light in their top bits (glowing patches': src/world.cpp).
constexpr uint64_t kEngineLightKey = 1ull << 62;
constexpr uint64_t kParticleLightKey = 3ull << 62;
constexpr float kPi = 3.14159265358979f;

struct State {
    HMODULE client = nullptr;
    const std::byte* manager = nullptr;
    const void* managerVtable = nullptr;
    const void* effectVtable = nullptr;
    const void* effectCollectionVtable = nullptr;
    const void* collectionVtable = nullptr;
    uint32_t framesUntilSearch = 0;
    bool unusable = false;
    bool loggedMismatch = false;
    // Where this frame's and last frame's effects read before their view was lit are: drawn glow is matched against those
    // of the frame it was drawn.
    std::vector<std::array<float, 3>> sameFrame;
    std::vector<std::array<float, 3>> previousSameFrame;
};

State g_state;

template <class T>
T Read(const std::byte* address)
{
    T value;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

float Luminance(const float (&c)[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// The particle manager, once the client has built it.
bool FindManager()
{
    if (g_state.manager != nullptr)
        return true;
    if (g_state.unusable)
        return false;
    if (g_state.client == nullptr) {
        g_state.client = GetModuleHandleW(L"client.dll");
        if (g_state.client == nullptr)
            return false;
        g_state.managerVtable = FindVtable(g_state.client, ".?AVCParticleMgr@@", 0);
        g_state.effectVtable = FindVtable(g_state.client, ".?AVCNewParticleEffect@@", 0);
        g_state.effectCollectionVtable = FindVtable(g_state.client, ".?AVCNewParticleEffect@@", static_cast<DWORD>(kEffectCollection));
        g_state.collectionVtable = FindVtable(g_state.client, ".?AVCParticleCollection@@", 0);
        if (g_state.managerVtable == nullptr || g_state.effectVtable == nullptr || g_state.effectCollectionVtable == nullptr ||
            g_state.collectionVtable == nullptr) {
            g_state.unusable = true;
            log::Info("Effect lights: the client's particle classes were not found; particle systems light nothing");
            return false;
        }
    }
    if (g_state.framesUntilSearch > 0) {
        --g_state.framesUntilSearch;
        return false;
    }
    const std::vector<const std::byte*> managers = FindObjectsWithVtable(g_state.client, g_state.managerVtable);
    if (managers.size() > 1) {
        g_state.unusable = true;
        log::Info("Effect lights: {} particle managers found; particle systems light nothing", managers.size());
        return false;
    }
    if (managers.empty()) {
        g_state.framesUntilSearch = kManagerRetryFrames;
        return false;
    }
    g_state.manager = managers.front();
    log::Info("Effect lights: particle manager found");
    return true;
}

// What a definition's particles add to the image per unit of their tint and alpha: SpriteCard adds its texture's colour
// times its alpha where the material is $additive; other materials blend and give no light.
const float* GlowOf(const std::byte* definition)
{
    const char* raw = reinterpret_cast<const char*>(definition + kDefinitionMaterial);
    const glow_draws::MaterialGlow& glow = glow_draws::GlowOf(std::string_view(raw, strnlen(raw, kMaterialNameBytes)));
    return glow.known && glow.additive ? glow.alphaWeighted : nullptr;
}

// What a particle system's glowing particles give off, summed over its collections.
struct Emission {
    float color[3] = {};
    // Luminance-weighted sums of the particles' positions and their squares, and the luminance.
    double position[3] = {};
    double squared = 0.0;
    double weight = 0.0;
};

bool ValidCollection(const std::byte* collection, int depth)
{
    const void* vtable = Read<const void*>(collection);
    return depth == 0 ? vtable == g_state.effectCollectionVtable : vtable == g_state.collectionVtable || vtable == g_state.effectCollectionVtable;
}

// Adds a collection's glowing particles and its children's; false when its structure is not as expected.
bool AddCollection(const std::byte* collection, int depth, Emission& emission)
{
    if (depth > kMaxDepth || !ValidCollection(collection, depth))
        return false;
    const int count = Read<int>(collection + kCollectionCount);
    const std::byte* definition = Read<const std::byte*>(collection + kCollectionDefinition);
    if (count < 0 || count > kMaxParticles)
        return false;
    if (count > 0 && definition != nullptr && Read<uint8_t>(collection + kCollectionDormant) == 0) {
        const float* glow = GlowOf(definition);
        const float* attributes[17] = {};
        size_t strides[17] = {};
        for (size_t a : {kAttributePosition, kAttributeRadius, kAttributeTint, kAttributeAlpha, kAttributeAlpha2}) {
            attributes[a] = Read<const float*>(collection + kCollectionAttributes + a * sizeof(void*));
            strides[a] = Read<size_t>(collection + kCollectionStrides + a * sizeof(size_t));
        }
        const bool readable = attributes[kAttributePosition] != nullptr && attributes[kAttributeRadius] != nullptr && attributes[kAttributeTint] != nullptr &&
            attributes[kAttributeAlpha] != nullptr && attributes[kAttributeAlpha2] != nullptr;
        for (int i = 0; glow != nullptr && readable && i < count; ++i) {
            const size_t block = static_cast<size_t>(i) / 4;
            const size_t lane = static_cast<size_t>(i) % 4;
            const auto at = [&](size_t a) { return attributes[a] + strides[a] * block + lane; };
            const float* position = at(kAttributePosition);
            const float* tint = at(kAttributeTint);
            const float radius = *at(kAttributeRadius);
            const float alpha = *at(kAttributeAlpha) * *at(kAttributeAlpha2);
            const float p[3] = {position[0], position[4], position[8]};
            if (!(radius > 0.0f) || !(alpha > 0.0f) || !std::isfinite(p[0] + p[1] + p[2]))
                continue;
            // A camera-facing sprite 2r across shows its texture's mean over that square from every side.
            const float area = 4.0f * radius * radius * alpha;
            const float color[3] = {glow[0] * tint[0] * area, glow[1] * tint[4] * area, glow[2] * tint[8] * area};
            const float luminance = Luminance(color);
            if (!(luminance > 0.0f) || !std::isfinite(luminance))
                continue;
            for (int c = 0; c < 3; ++c) {
                emission.color[c] += color[c];
                emission.position[c] += luminance * p[c];
            }
            emission.squared += luminance * (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            emission.weight += luminance;
        }
    }
    for (const std::byte* child = Read<const std::byte*>(collection + kCollectionChildren); child != nullptr;
         child = Read<const std::byte*>(child + kCollectionSibling)) {
        if (!AddCollection(child, depth + 1, emission))
            return false;
    }
    return true;
}

// The light of glow of this colour in all (emission times area, linear) about `centre`, `spread` units across: a sphere
// that shows its area every direction alike, at the calibration of glow's brightness (glow_draws::kLightPerGlow).
FrameLight GlowLight(const float (&color)[3], const double (&centre)[3], float spread, float strength, uint32_t flags, uint64_t key)
{
    FrameLight light;
    light.kind = FrameLight::Kind::Particles;
    for (int c = 0; c < 3; ++c) {
        light.origin[c] = static_cast<float>(centre[c]);
        light.color[c] = color[c] * glow_draws::kLightPerGlow * strength;
    }
    light.innerCosine = 1.0f;
    light.size = std::clamp(spread, kMinGlowSize, kMaxGlowSize);
    light.radius = std::max(std::sqrt(Luminance(light.color) / (kPi * kLightThreshold)), light.size);
    light.flags = flags;
    light.key = key;
    return light;
}

float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// The kind of old-style emitter, by its class (RTTI), cached by vtable.
enum class EmitterKind { Other, Simple, Lua };

EmitterKind KindOf(const void* vtable)
{
    static std::unordered_map<const void*, EmitterKind> kinds;
    const auto [entry, added] = kinds.try_emplace(vtable, EmitterKind::Other);
    if (added && g_state.client != nullptr) {
        if (DerivesFrom(g_state.client, vtable, ".?AVCSimpleEmitter@@"))
            entry->second = EmitterKind::Simple;
        else if (DerivesFrom(g_state.client, vtable, ".?AVCLuaEmitter@@"))
            entry->second = EmitterKind::Lua;
    }
    return entry->second;
}

// The light of each old-style particle effect that glows: CSimpleEmitter's and Lua's particles drawn with an additive
// material, each a quad 2 x size across.
void EmitterLights(float strength, std::vector<FrameLight>& lights)
{
    if (g_state.manager == nullptr)
        return;
    const std::byte* elements = Read<const std::byte*>(g_state.manager + kManagerBindings);
    int bindings = 0;
    for (uint16_t i = Read<uint16_t>(g_state.manager + kManagerBindingsHead); i != kListEnd && elements != nullptr && bindings < kMaxBindings;
         i = Read<uint16_t>(elements + i * kListElement + kListNext), ++bindings) {
        const std::byte* binding = Read<const std::byte*>(elements + i * kListElement);
        if (binding == nullptr || (Read<uint8_t>(binding + kBindingFlags) & kBindingRemove) != 0)
            continue;
        void* effect = Read<void*>(binding + kBindingEffect);
        if (effect == nullptr)
            continue;
        const void* const* vtable = Read<const void* const*>(static_cast<const std::byte*>(effect));
        const EmitterKind kind = KindOf(vtable);
        if (kind == EmitterKind::Other)
            continue;
        float transform[4][4];
        std::memcpy(transform, binding + kBindingTransform, sizeof(transform));
        const bool local = Read<uint8_t>(binding + kBindingIdentity) == 0;
        Emission emission;
        int particles = 0;
        const std::byte* materials = Read<const std::byte*>(binding + kBindingMaterials);
        for (uint16_t m = Read<uint16_t>(binding + kBindingMaterialsHead); m != kListEnd && materials != nullptr;
             m = Read<uint16_t>(materials + m * kListElement + kListNext)) {
            const std::byte* material = Read<const std::byte*>(materials + m * kListElement);
            const std::byte* group = material != nullptr ? Read<const std::byte*>(material) : nullptr;
            void* imaterial = group != nullptr ? Read<void*>(group) : nullptr;
            const char* name = imaterial != nullptr ? (*reinterpret_cast<const char* (__fastcall* const*)(void*)>(*static_cast<void* const*>(imaterial)))(imaterial)
                                                    : nullptr;
            const glow_draws::MaterialGlow& glow = glow_draws::GlowOf(name != nullptr ? name : "");
            if (!glow.known || !glow.additive)
                continue;
            const std::byte* sentinel = material + kEffectMaterialSentinel;
            for (const std::byte* particle = Read<const std::byte*>(sentinel + kParticleNext); particle != nullptr && particle != sentinel;
                 particle = Read<const std::byte*>(particle + kParticleNext)) {
                if (++particles > kMaxBindingParticles)
                    break;
                float alpha = 0.0f;
                float size = 0.0f;
                float color[3] = {};
                if (kind == EmitterKind::Simple) {
                    alpha = reinterpret_cast<float(__fastcall*)(void*, const void*)>(vtable[kUpdateAlphaSlot])(effect, particle);
                    size = reinterpret_cast<float(__fastcall*)(void*, const void*)>(vtable[kUpdateScaleSlot])(effect, particle);
                    float returned[3] = {};
                    reinterpret_cast<void*(__fastcall*)(void*, float*, const void*)>(vtable[kUpdateColorSlot])(effect, returned, particle);
                    std::copy_n(returned, 3, color);
                } else {
                    const uint8_t flags = Read<uint8_t>(particle + kLuaFlags);
                    if ((flags & kLuaStarted) == 0 || (flags & kLuaLit) != 0)
                        continue;
                    const float die = Read<float>(particle + kLuaDieTime);
                    const float t = die == 0.0f ? 1.0f : Read<float>(particle + kLuaAge) / die;
                    if (!(t >= 0.0f && t <= 1.0f))
                        continue;
                    const float start = Read<uint8_t>(particle + kLuaStartAlpha) / 255.0f;
                    const float end = Read<uint8_t>(particle + kLuaEndAlpha) / 255.0f;
                    alpha = std::clamp(start + (end - start) * t, 0.0f, 1.0f);
                    const float startSize = Read<float>(particle + kLuaStartSize);
                    size = startSize + (Read<float>(particle + kLuaEndSize) - startSize) * t;
                    for (int c = 0; c < 3; ++c)
                        color[c] = Read<uint8_t>(particle + kLuaColor + static_cast<size_t>(c)) / 255.0f;
                }
                if (!(alpha > 0.001f) || !(size > 0.0f) || !std::isfinite(alpha * size))
                    continue;
                float position[3];
                std::memcpy(position, particle + kParticlePosition, sizeof(position));
                if (local) {
                    const float p[3] = {position[0], position[1], position[2]};
                    for (int r = 0; r < 3; ++r)
                        position[r] = transform[r][0] * p[0] + transform[r][1] * p[1] + transform[r][2] * p[2] + transform[r][3];
                }
                const float area = 4.0f * size * size * alpha;
                const float emitted[3] = {glow.alphaWeighted[0] * SrgbToLinear(std::clamp(color[0], 0.0f, 1.0f)) * area,
                    glow.alphaWeighted[1] * SrgbToLinear(std::clamp(color[1], 0.0f, 1.0f)) * area,
                    glow.alphaWeighted[2] * SrgbToLinear(std::clamp(color[2], 0.0f, 1.0f)) * area};
                const float luminance = Luminance(emitted);
                if (!(luminance > 0.0f) || !std::isfinite(luminance + position[0] + position[1] + position[2]))
                    continue;
                for (int c = 0; c < 3; ++c) {
                    emission.color[c] += emitted[c];
                    emission.position[c] += luminance * position[c];
                }
                emission.squared += luminance * (position[0] * position[0] + position[1] * position[1] + position[2] * position[2]);
                emission.weight += luminance;
            }
        }
        if (emission.weight <= 0.0)
            continue;
        double centre[3];
        double spread = emission.squared / emission.weight;
        for (int c = 0; c < 3; ++c) {
            centre[c] = emission.position[c] / emission.weight;
            spread -= centre[c] * centre[c];
        }
        lights.push_back(GlowLight(emission.color, centre, static_cast<float>(std::sqrt(std::max(spread, 0.0))), strength, 0u,
            kParticleLightKey | reinterpret_cast<uintptr_t>(binding)));
    }
}

// The light of each drawn sprite entity that adds its colour (entities::CollectGlowSprites): its material's mean colour
// times its colour and brightness and the area it covers.
void SpriteLights(float strength, std::vector<FrameLight>& lights)
{
    static std::vector<entities::GlowSprite> sprites;
    entities::CollectGlowSprites(sprites);
    for (const entities::GlowSprite& sprite : sprites) {
        const glow_draws::MaterialGlow& glow = glow_draws::GlowOf(sprite.material);
        if (!glow.known)
            continue;
        const float area = sprite.area * sprite.brightness;
        const float emitted[3] = {glow.alphaWeighted[0] * SrgbToLinear(std::clamp(sprite.color[0], 0.0f, 1.0f)) * area,
            glow.alphaWeighted[1] * SrgbToLinear(std::clamp(sprite.color[1], 0.0f, 1.0f)) * area,
            glow.alphaWeighted[2] * SrgbToLinear(std::clamp(sprite.color[2], 0.0f, 1.0f)) * area};
        if (!(Luminance(emitted) > 0.0f))
            continue;
        const double centre[3] = {sprite.origin[0], sprite.origin[1], sprite.origin[2]};
        lights.push_back(GlowLight(emitted, centre, 0.25f * std::sqrt(sprite.area), strength, 0u, kParticleLightKey));
    }
}

// The light of each drawn particle system that glows.
void ParticleLights(float strength, std::vector<FrameLight>& lights)
{
    if (!FindManager())
        return;
    for (const std::byte* effect = Read<const std::byte*>(g_state.manager + kManagerEffects); effect != nullptr;
         effect = Read<const std::byte*>(effect + kEffectNext)) {
        if (Read<const void*>(effect) != g_state.effectVtable) {
            if (!std::exchange(g_state.loggedMismatch, true))
                log::Info("Effect lights: a particle effect is not as expected; particle systems light nothing this frame");
            return;
        }
        if ((Read<uint8_t>(effect + kEffectFlags) & kEffectRemove) != 0 || Read<uint8_t>(effect + kEffectDrawn) == 0)
            continue;
        Emission emission;
        if (!AddCollection(effect + kEffectCollection, 0, emission)) {
            if (!std::exchange(g_state.loggedMismatch, true))
                log::Info("Effect lights: a particle collection is not as expected; particle systems light nothing this frame");
            return;
        }
        if (emission.weight <= 0.0)
            continue;
        double centre[3];
        double spread = emission.squared / emission.weight;
        for (int c = 0; c < 3; ++c) {
            centre[c] = emission.position[c] / emission.weight;
            spread -= centre[c] * centre[c];
        }
        lights.push_back(GlowLight(emission.color, centre, static_cast<float>(std::sqrt(std::max(spread, 0.0))), strength,
            Read<uint8_t>(effect + kEffectViewModel) != 0 ? kLightOnLocalPlayer : 0u, kParticleLightKey | reinterpret_cast<uintptr_t>(effect)));
    }
}

float DistanceSquared(const float (&a)[3], const float (&b)[3])
{
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

float Score(const FrameLight& light, const std::array<float, 3>& eye)
{
    const float d[3] = {light.origin[0] - eye[0], light.origin[1] - eye[1], light.origin[2] - eye[2]};
    const float reach = light.kind == FrameLight::Kind::Engine ? light.radius * light.radius : 1.0f;
    return Luminance(light.color) * reach / std::max(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], std::max(light.size * light.size, 1.0f));
}

// An elight as a sphere whose light falls off with the inverse square, as bright at half its radius as Source's falloff
// makes it there: Source sized elights to light the models next to them, and its falloff ends there.
FrameLight ElightLight(const engine::DynamicLight& light, float strength, const std::optional<std::array<float, 3>>& localPlayer)
{
    FrameLight frame;
    frame.kind = FrameLight::Kind::Particles;
    std::copy_n(light.origin, 3, frame.origin);
    const float match = kElightMatch * light.radius;
    const float source = (1.0f - kElightMatch * kElightMatch) * (1.0f - kElightMatch * kElightMatch);
    for (int c = 0; c < 3; ++c)
        frame.color[c] = light.color[c] * strength * source * kPi * match * match;
    frame.innerCosine = 1.0f;
    frame.size = kElightSize;
    frame.radius = std::max(std::sqrt(Luminance(frame.color) / (kPi * kLightThreshold)), frame.size);
    frame.flags = ((light.flags & engine::kDlightNoWorld) != 0 ? kLightNoWorld : 0u) | ((light.flags & engine::kDlightNoModel) != 0 ? kLightNoModel : 0u);
    if (localPlayer) {
        const float player[3] = {(*localPlayer)[0], (*localPlayer)[1], (*localPlayer)[2]};
        if (DistanceSquared(frame.origin, player) < kLocalPlayerReach * kLocalPlayerReach)
            frame.flags |= kLightOnLocalPlayer;
    }
    frame.key = kEngineLightKey | light.identity;
    return frame;
}

}

FrameLight FromEngineLight(const engine::DynamicLight& light, float strength)
{
    constexpr float kDegreesToRadians = kPi / 180.0f;
    FrameLight frame;
    frame.kind = FrameLight::Kind::Engine;
    std::copy_n(light.origin, 3, frame.origin);
    for (int c = 0; c < 3; ++c)
        frame.color[c] = light.color[c] * strength;
    frame.radius = light.radius;
    std::copy_n(light.direction, 3, frame.direction);
    const bool spot = light.outerAngle > 0.0f;
    frame.innerCosine = spot ? std::cos(light.innerAngle * kDegreesToRadians) : 0.0f;
    frame.outerCosine = spot ? std::cos(light.outerAngle * kDegreesToRadians) : -1.0f;
    frame.flags = ((light.flags & engine::kDlightNoWorld) != 0 ? kLightNoWorld : 0u) | ((light.flags & engine::kDlightNoModel) != 0 ? kLightNoModel : 0u);
    frame.key = kEngineLightKey | light.identity;
    return frame;
}

void Collect(const Context& context, size_t limit, std::vector<FrameLight>& lights)
{
    const Settings& settings = GetSettings();
    const float strength = settings.effectLightStrength;
    // The glow drawn last frame is taken even when nothing is lit now, so it never lights a later frame.
    const std::vector<glow_draws::Glow> drawn = glow_draws::Take();
    std::swap(g_state.previousSameFrame, g_state.sameFrame);
    g_state.sameFrame.clear();
    if (!settings.effectLights || strength <= 0.0f || limit == 0)
        return;
    static std::vector<engine::DynamicLight> entityLights;
    engine::ActiveEntityLights(entityLights);
    static std::vector<FrameLight> flashes;
    flashes.clear();
    constexpr uint32_t kNowhere = engine::kDlightNoWorld | engine::kDlightNoModel;
    for (const engine::DynamicLight& light : entityLights) {
        if ((light.flags & kNowhere) != kNowhere)
            flashes.push_back(ElightLight(light, strength, context.localPlayer));
    }
    const size_t glowsFirst = flashes.size();
    ParticleLights(strength, flashes);
    EmitterLights(strength, flashes);
    SpriteLights(strength, flashes);
    const size_t drawnFirst = flashes.size();
    for (const FrameLight& light : flashes)
        g_state.sameFrame.push_back({light.origin[0], light.origin[1], light.origin[2]});
    for (const glow_draws::Glow& glow : drawn) {
        const double centre[3] = {glow.centre[0], glow.centre[1], glow.centre[2]};
        flashes.push_back(GlowLight(glow.color, centre, glow.spread, strength, 0u, kParticleLightKey));
    }
    // Effects that bring their own engine light keep only it, drawn glow of effects read before their view was lit is
    // theirs, and glow at a map light is its halo when lamps' glows are skipped.
    const auto within = [](const float (&point)[3], const float (&other)[3], float reach) { return DistanceSquared(point, other) < reach * reach; };
    const auto covered = [&](const FrameLight& light, std::span<const std::array<float, 3>> effects) {
        for (const engine::DynamicLight& engine : context.engineLights) {
            if (within(light.origin, engine.origin, kEffectCover))
                return true;
        }
        for (const engine::DynamicLight& engine : entityLights) {
            if (within(light.origin, engine.origin, kEffectCover))
                return true;
        }
        for (const std::array<float, 3>& effect : effects) {
            const float point[3] = {effect[0], effect[1], effect[2]};
            if (within(light.origin, point, kEffectCover))
                return true;
        }
        if (settings.skipLampGlows) {
            for (const bsp::Float3& lamp : context.mapLights) {
                const float point[3] = {lamp.x, lamp.y, lamp.z};
                if (within(light.origin, point, kLampHaloReach))
                    return true;
            }
        }
        return false;
    };
    size_t kept = 0;
    for (size_t i = 0; i < flashes.size(); ++i) {
        const bool glow = i >= glowsFirst;
        const bool drawnGlow = i >= drawnFirst;
        if (glow && covered(flashes[i], drawnGlow ? std::span<const std::array<float, 3>>(g_state.previousSameFrame) : std::span<const std::array<float, 3>>()))
            continue;
        flashes[kept++] = flashes[i];
    }
    flashes.resize(kept);
    const size_t taken = std::min(limit, flashes.size());
    std::partial_sort(flashes.begin(), flashes.begin() + static_cast<std::ptrdiff_t>(taken), flashes.end(),
        [&](const FrameLight& a, const FrameLight& b) { return Score(a, context.eye) > Score(b, context.eye); });
    lights.insert(lights.end(), flashes.begin(), flashes.begin() + static_cast<std::ptrdiff_t>(taken));
    // With -gmodpp_dev, what was found, when it changes and at most once a second.
    if (GetOptions().devMode) {
        static std::array<size_t, 5> logged = {};
        static std::chrono::steady_clock::time_point last;
        const std::array<size_t, 5> counts = {entityLights.size(), drawnFirst - glowsFirst, drawn.size(), flashes.size(), taken};
        // counts[1]: particle systems, old-style effects and sprite entities that glow.
        const auto now = std::chrono::steady_clock::now();
        if (counts != logged && now - last >= std::chrono::seconds(1)) {
            logged = counts;
            last = now;
            log::Info("Effect lights: {} elights, {} glowing particle systems, effects and sprites{}, {} drawn glows last frame; {} lights kept of {}", counts[0], counts[1],
                g_state.manager != nullptr ? "" : " (particle manager not found yet)", counts[2], counts[4], counts[3]);
        }
    }
}

}
