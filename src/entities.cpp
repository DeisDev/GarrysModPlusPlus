#include "entities.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>

namespace gmodpp::entities {

namespace {

using CreateInterfaceFn = void*(__cdecl*)(const char* name, int* returnCode);

// Vtable slots follow the Source SDK 2013 declarations (IClientEntityList, IClientUnknown,
// IClientRenderable, IVModelInfo, IVEngineClient013) and were confirmed in GMod's client.dll and
// engine.dll with Ghidra. GMod's SetupBones and IVEngineClient::Time use double-precision time.
constexpr int kGetClientEntitySlot = 3;
constexpr int kGetHighestEntityIndexSlot = 6;
constexpr int kGetClientRenderableSlot = 5;
constexpr int kShouldDrawSlot = 3;
constexpr int kGetModelSlot = 9;
// IClientRenderable::GetShadowParent: decodes the move parent handle and returns that entity's renderable
// (client.dll+0x1b4860, C_BaseCombatWeapon's renderable vtable found through RTTI in Ghidra).
constexpr int kGetShadowParentSlot = 28;
constexpr int kRenderableToWorldTransformSlot = 34;
constexpr int kGetModelNameSlot = 3;
constexpr int kGetModelTypeSlot = 9;
// modtype_t: mod_bad, mod_brush, mod_sprite, mod_studio.
constexpr int kModelTypeBrush = 1;
constexpr int kModelTypeStudio = 3;
constexpr int kSetupBonesSlot = 16;
constexpr int kEngineTimeSlot = 14;
constexpr int kBoneUsedByAnything = 0x0007FF00;
constexpr int kGetClientNetworkableSlot = 4;
constexpr int kGetClientClassSlot = 2;
constexpr size_t kClientClassName = 16;
// ClientClass::m_pRecvTable, RecvTable::m_pProps and m_nProps, and RecvProp (SDK public/client_class.h and dt_recv.h,
// 64-bit layout): m_pVarName, m_RecvType, m_pDataTable and m_Offset in a 96-byte struct. Checked in game: the walk finds
// m_iViewModelIndex of DT_BaseCombatWeapon at kWeaponViewModelIndex.
constexpr size_t kClientClassRecvTable = 24;
constexpr size_t kRecvTableProps = 0;
constexpr size_t kRecvTablePropCount = 8;
constexpr size_t kRecvPropSize = 96;
constexpr size_t kRecvPropName = 0;
constexpr size_t kRecvPropType = 8;
constexpr size_t kRecvPropDataTable = 64;
constexpr size_t kRecvPropOffset = 72;
constexpr int kPropTypeDataTable = 6;
constexpr int kMaxRecvProps = 4096;
constexpr int kMaxRecvDepth = 16;
constexpr char kSkinProp[] = "m_nSkin";
// DT_BaseAnimating's m_flModelScale (built at client.dll+0x195830) and GMod's legacy transform flag beside it, set by
// Entity:SetLegacyTransform (client.dll+0x12a8b0). C_BaseAnimating::InternalDrawModel (0x1a0270, vtable slot 212) reads
// both at these offsets (REA 2026-11-03), so the flag is read only where m_flModelScale is where REA found it.
constexpr char kModelScaleProp[] = "m_flModelScale";
constexpr int kModelScaleOffset = 0x1818;
constexpr size_t kLegacyTransform = 0x1be4;
constexpr int kNoProp = -1;
constexpr int kModelInfoGetModelSlot = 1;
// C_BaseCombatWeapon's m_iViewModelIndex and m_iWorldModelIndex (their DT_BaseCombatWeapon RecvProps are built at
// client.dll+0x27990, confirmed in Ghidra).
constexpr size_t kWeaponViewModelIndex = 0x1c10;
constexpr size_t kWeaponWorldModelIndex = 0x1c14;

// C_RopeKeyframe in GMod's client.dll (DT_RopeKeyframe, DrawModel and OnDataChanged, confirmed in Ghidra). Between the
// networked m_iRopeMaterialModelIndex and m_nSegments lie CRopePhysics<10> m_RopePhysics (SDK public/rope_physics.h:
// node pointer, node count, then the nodes, each m_vPos, m_vPrevPos and the drawn m_vPredicted) and m_LightValues.
constexpr char kRopeClass[] = "CRopeKeyframe";
constexpr size_t kRopeMaterialIndex = 0x16fc;
constexpr size_t kRopeNodePointer = 0x1710;
constexpr size_t kRopeNodeCount = 0x1718;
constexpr size_t kRopeNodes = 0x1750;
constexpr size_t kRopeNodeSize = 36;
constexpr size_t kRopeNodePredicted = 24;
constexpr size_t kRopeSubdiv = 0x19f8;
constexpr size_t kRopeWidth = 0x1a0c;
constexpr int kRopeMaxNodes = 10;
// m_Subdiv 255 takes the rope_subdiv convar (default 2); the game clamps it below MAX_ROPE_SUBDIVS (8).
constexpr int kRopeSubdivFromConvar = 255;
constexpr int kRopeDefaultSubdiv = 2;
constexpr int kRopeMaxSubdiv = 7;
constexpr float kWorldLimit = 65536.0f;

// Sprite entities (C_Sprite, ClientClass "CSprite", and C_SpriteOriented, "CSpriteOriented"), as C_Sprite::DrawModel
// (client.dll+0x2dadc0) draws them (REA 2026-11-05): at its renderable's GetRenderOrigin (slot 1, +0x2db170: the attached
// entity's attachment, else its origin), sized by GetRenderScale (entity vtable slot 206, +0x2dacd0), divided by the
// smaller of its sprite's width and height with m_bWorldSpaceScale, as bright as GetRenderBrightness (slot 207,
// +0x2dad40; 0-255), coloured by m_clrRender and $HDRCOLORSCALE (m_flHDRColorScale). DrawSpriteModel (+0x1fae00) spans
// the quad from the origin by the scale times the CEngineSprite's up, down, left and right extents (+0x80, +0x84, +0x88,
// +0x8c), which IVModelInfo::GetModelExtraData (slot 10) gives for a sprite model (GetModelType, slot 9, 2), and draws
// it with the CEngineSprite's material for its render mode (+0x18 + 8 * mode). Additive render modes (the SDK's
// RenderMode_t): glow 3, add 5, add with frame blend 7, alpha add 8, world glow 9.
constexpr char kSpriteClass[] = "CSprite";
constexpr char kOrientedSpriteClass[] = "CSpriteOriented";
constexpr int kGetRenderOriginSlot = 1;
constexpr int kGetRenderScaleSlot = 206;
constexpr int kGetRenderBrightnessSlot = 207;
constexpr int kGetModelExtraDataSlot = 10;
constexpr int kModelTypeSprite = 2;
constexpr size_t kEngineSpriteWidth = 0x8;
constexpr size_t kEngineSpriteHeight = 0xC;
constexpr size_t kEngineSpriteMaterials = 0x18;
constexpr size_t kEngineSpriteExtents = 0x80;
constexpr int kRenderModes = 11;
constexpr int kGetMaterialNameSlot = 0;
constexpr char kRenderModeProp[] = "m_nRenderMode";
constexpr char kRenderColorProp[] = "m_clrRender";
constexpr char kWorldSpaceScaleProp[] = "m_bWorldSpaceScale";
constexpr char kHdrColorScaleProp[] = "m_flHDRColorScale";

// env_sun's client class, C_Sun in GMod's client.dll (ClientClass "CSun" at 0x87F2C8, DT_Sun built at 0x30380):
// m_vDirection is a vector, m_bOn a one-byte bool, m_clrRender a color32, m_nSize an int. Its OnDataChanged (0x1FCC10)
// gives the sun's sprite the half size m_nSize and m_clrRender divided by its largest component (REA, 2026-11-02).
constexpr char kSunClass[] = "CSun";
struct SunProps {
    int direction;
    int on;
    int color;
    int size;
};

void* g_entityList = nullptr;
// Offset of m_nSkin and of m_flModelScale in each client class, or kNoProp.
std::unordered_map<const void*, int> g_skinOffsets;
std::unordered_map<const void*, int> g_modelScaleOffsets;
// DT_Sun's offsets per client class.
std::unordered_map<const void*, SunProps> g_sunProps;
void* g_modelInfo = nullptr;
void* g_engineClient = nullptr;
bool g_attempted = false;

void* Create(const wchar_t* moduleName, const char* name)
{
    HMODULE module = GetModuleHandleW(moduleName);
    auto create = module ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface")) : nullptr;
    int code = 0;
    return create ? create(name, &code) : nullptr;
}

template <class Fn>
Fn Slot(void* object, int slot)
{
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[slot]);
}

template <class T>
T Field(const void* object, size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(T));
    return value;
}

// The offset of a networked variable in an entity of the class, found through its receive tables (base classes are
// nested data tables, at their own offset), or kNoProp.
int FindProp(const void* table, const char* name, int depth)
{
    if (table == nullptr || depth > kMaxRecvDepth)
        return kNoProp;
    const auto* props = Field<const std::byte*>(table, kRecvTableProps);
    const int count = Field<int>(table, kRecvTablePropCount);
    if (props == nullptr || count < 0 || count > kMaxRecvProps)
        return kNoProp;
    for (int i = 0; i < count; ++i) {
        const std::byte* prop = props + static_cast<size_t>(i) * kRecvPropSize;
        const char* propName = Field<const char*>(prop, kRecvPropName);
        const int offset = Field<int>(prop, kRecvPropOffset);
        if (propName != nullptr && std::strcmp(propName, name) == 0)
            return offset;
        if (Field<int>(prop, kRecvPropType) == kPropTypeDataTable) {
            const int nested = FindProp(Field<const void*>(prop, kRecvPropDataTable), name, depth + 1);
            if (nested != kNoProp)
                return offset + nested;
        }
    }
    return kNoProp;
}

// FindProp for a client class, remembered per class in `offsets`.
int ClassProp(std::unordered_map<const void*, int>& offsets, const void* clientClass, const char* name)
{
    auto found = offsets.find(clientClass);
    if (found == offsets.end())
        found = offsets.emplace(clientClass, FindProp(Field<const void*>(clientClass, kClientClassRecvTable), name, 0)).first;
    return found->second;
}

}

void TryInitialize()
{
    if (g_attempted || GetModuleHandleW(L"client.dll") == nullptr || GetModuleHandleW(L"engine.dll") == nullptr)
        return;
    g_attempted = true;
    void* entityList = Create(L"client.dll", "VClientEntityList003");
    void* modelInfo = Create(L"engine.dll", "VModelInfoClient006");
    if (entityList == nullptr || modelInfo == nullptr) {
        log::Info("Entity interfaces unavailable; dynamic shadow casters disabled");
        return;
    }
    g_entityList = entityList;
    g_modelInfo = modelInfo;
    g_engineClient = Create(L"engine.dll", "VEngineClient013");
}

bool Ready() { return g_entityList != nullptr && g_modelInfo != nullptr; }

void Collect(std::vector<ModelEntity>& entities)
{
    entities.clear();
    if (!Ready())
        return;
    const int highest = Slot<int(__fastcall*)(void*)>(g_entityList, kGetHighestEntityIndexSlot)(g_entityList);
    for (int index = 1; index <= highest; ++index) {
        void* entity = Slot<void*(__fastcall*)(void*, int)>(g_entityList, kGetClientEntitySlot)(g_entityList, index);
        if (entity == nullptr)
            continue;
        void* renderable = Slot<void*(__fastcall*)(void*)>(entity, kGetClientRenderableSlot)(entity);
        if (renderable == nullptr || !Slot<bool(__fastcall*)(void*)>(renderable, kShouldDrawSlot)(renderable))
            continue;
        const void* model = Slot<const void*(__fastcall*)(void*)>(renderable, kGetModelSlot)(renderable);
        if (model == nullptr)
            continue;
        const int type = Slot<int(__fastcall*)(void*, const void*)>(g_modelInfo, kGetModelTypeSlot)(g_modelInfo, model);
        if (type != kModelTypeStudio && type != kModelTypeBrush)
            continue;
        const char* name = Slot<const char*(__fastcall*)(void*, const void*)>(g_modelInfo, kGetModelNameSlot)(g_modelInfo, model);
        const float* transform = Slot<const float*(__fastcall*)(void*)>(renderable, kRenderableToWorldTransformSlot)(renderable);
        if (name == nullptr || transform == nullptr)
            continue;
        ModelEntity& out = entities.emplace_back();
        out.index = index;
        out.entity = entity;
        out.renderable = renderable;
        out.parent = Slot<void*(__fastcall*)(void*)>(renderable, kGetShadowParentSlot)(renderable);
        out.model = name;
        void* networkable = Slot<void*(__fastcall*)(void*)>(entity, kGetClientNetworkableSlot)(entity);
        const void* clientClass = networkable != nullptr ? Slot<const void*(__fastcall*)(void*)>(networkable, kGetClientClassSlot)(networkable) : nullptr;
        const int skin = clientClass != nullptr ? ClassProp(g_skinOffsets, clientClass, kSkinProp) : kNoProp;
        out.skin = skin != kNoProp ? std::max(Field<int>(entity, static_cast<size_t>(skin)), 0) : 0;
        const int scale = clientClass != nullptr ? ClassProp(g_modelScaleOffsets, clientClass, kModelScaleProp) : kNoProp;
        out.modelScale = scale != kNoProp ? Field<float>(entity, static_cast<size_t>(scale)) : 1.0f;
        out.legacyTransform = scale == kModelScaleOffset && Field<uint8_t>(entity, kLegacyTransform) != 0;
        std::memcpy(out.transform, transform, sizeof(out.transform));
    }
}

void CollectRopes(std::vector<Rope>& ropes)
{
    ropes.clear();
    if (!Ready())
        return;
    const int highest = Slot<int(__fastcall*)(void*)>(g_entityList, kGetHighestEntityIndexSlot)(g_entityList);
    for (int index = 1; index <= highest; ++index) {
        void* entity = Slot<void*(__fastcall*)(void*, int)>(g_entityList, kGetClientEntitySlot)(g_entityList, index);
        if (entity == nullptr)
            continue;
        void* networkable = Slot<void*(__fastcall*)(void*)>(entity, kGetClientNetworkableSlot)(entity);
        const void* clientClass = networkable != nullptr ? Slot<const void*(__fastcall*)(void*)>(networkable, kGetClientClassSlot)(networkable) : nullptr;
        if (clientClass == nullptr || std::strcmp(Field<const char*>(clientClass, kClientClassName), kRopeClass) != 0)
            continue;
        void* renderable = Slot<void*(__fastcall*)(void*)>(entity, kGetClientRenderableSlot)(entity);
        if (renderable == nullptr || !Slot<bool(__fastcall*)(void*)>(renderable, kShouldDrawSlot)(renderable))
            continue;
        // The node pointer points at the nodes inside the entity only where the layout above holds.
        const int nodes = Field<int>(entity, kRopeNodeCount);
        if (Field<const std::byte*>(entity, kRopeNodePointer) != static_cast<const std::byte*>(entity) + kRopeNodes || nodes < 2 || nodes > kRopeMaxNodes)
            continue;
        Rope rope;
        rope.index = index;
        rope.width = Field<float>(entity, kRopeWidth);
        const int subdiv = Field<uint8_t>(entity, kRopeSubdiv);
        rope.subdivisions = std::min(subdiv == kRopeSubdivFromConvar ? kRopeDefaultSubdiv : subdiv, kRopeMaxSubdiv);
        bool valid = std::isfinite(rope.width) && rope.width > 0.0f;
        for (int n = 0; n < nodes && valid; ++n) {
            const std::array<float, 3> node = Field<std::array<float, 3>>(entity, kRopeNodes + static_cast<size_t>(n) * kRopeNodeSize + kRopeNodePredicted);
            for (float v : node)
                valid = valid && std::isfinite(v) && std::fabs(v) < kWorldLimit;
            rope.nodes.push_back(node);
        }
        const void* model = Slot<const void*(__fastcall*)(void*, int)>(g_modelInfo, kModelInfoGetModelSlot)(g_modelInfo, Field<int>(entity, kRopeMaterialIndex));
        const char* material = model != nullptr ? Slot<const char*(__fastcall*)(void*, const void*)>(g_modelInfo, kGetModelNameSlot)(g_modelInfo, model) : nullptr;
        if (!valid || material == nullptr)
            continue;
        rope.material = material;
        ropes.push_back(std::move(rope));
    }
}

std::optional<gbuffer::EnvSun> FindSun()
{
    if (!Ready())
        return std::nullopt;
    std::optional<gbuffer::EnvSun> found;
    const int highest = Slot<int(__fastcall*)(void*)>(g_entityList, kGetHighestEntityIndexSlot)(g_entityList);
    for (int index = 1; index <= highest; ++index) {
        void* entity = Slot<void*(__fastcall*)(void*, int)>(g_entityList, kGetClientEntitySlot)(g_entityList, index);
        if (entity == nullptr)
            continue;
        void* networkable = Slot<void*(__fastcall*)(void*)>(entity, kGetClientNetworkableSlot)(entity);
        const void* clientClass = networkable != nullptr ? Slot<const void*(__fastcall*)(void*)>(networkable, kGetClientClassSlot)(networkable) : nullptr;
        if (clientClass == nullptr || std::strcmp(Field<const char*>(clientClass, kClientClassName), kSunClass) != 0)
            continue;
        auto props = g_sunProps.find(clientClass);
        if (props == g_sunProps.end()) {
            const void* table = Field<const void*>(clientClass, kClientClassRecvTable);
            props = g_sunProps.emplace(clientClass, SunProps{FindProp(table, "m_vDirection", 0), FindProp(table, "m_bOn", 0), FindProp(table, "m_clrRender", 0),
                FindProp(table, "m_nSize", 0)}).first;
            const SunProps& p = props->second;
            if (p.direction == kNoProp || p.on == kNoProp || p.color == kNoProp || p.size == kNoProp)
                log::Info("Entities: env_sun's DT_Sun lacks a known prop (direction {}, on {}, colour {}, size {}); its sun is ignored", p.direction, p.on,
                    p.color, p.size);
        }
        const SunProps& p = props->second;
        if (p.direction == kNoProp || p.on == kNoProp || p.color == kNoProp || p.size == kNoProp)
            continue;
        gbuffer::EnvSun sun;
        sun.direction = Field<std::array<float, 3>>(entity, static_cast<size_t>(p.direction));
        const float length = std::sqrt(sun.direction[0] * sun.direction[0] + sun.direction[1] * sun.direction[1] + sun.direction[2] * sun.direction[2]);
        if (!std::isfinite(length) || length < 1e-3f)
            continue;
        for (float& c : sun.direction)
            c /= length;
        sun.on = Field<std::uint8_t>(entity, static_cast<size_t>(p.on)) != 0;
        const std::array<std::uint8_t, 4> color = Field<std::array<std::uint8_t, 4>>(entity, static_cast<size_t>(p.color));
        const float largest = static_cast<float>(std::max({color[0], color[1], color[2]}));
        if (largest > 0.0f)
            sun.color = {color[0] / largest, color[1] / largest, color[2] / largest};
        sun.size = static_cast<float>(Field<int>(entity, static_cast<size_t>(p.size)));
        if (sun.on)
            return sun;
        if (!found)
            found = std::move(sun);
    }
    return found;
}

void CollectGlowSprites(std::vector<GlowSprite>& sprites)
{
    sprites.clear();
    if (!Ready())
        return;
    static std::unordered_map<const void*, int> renderModes;
    static std::unordered_map<const void*, int> colors;
    static std::unordered_map<const void*, int> worldScales;
    static std::unordered_map<const void*, int> colorScales;
    const int highest = Slot<int(__fastcall*)(void*)>(g_entityList, kGetHighestEntityIndexSlot)(g_entityList);
    for (int index = 1; index <= highest; ++index) {
        void* entity = Slot<void*(__fastcall*)(void*, int)>(g_entityList, kGetClientEntitySlot)(g_entityList, index);
        if (entity == nullptr)
            continue;
        void* networkable = Slot<void*(__fastcall*)(void*)>(entity, kGetClientNetworkableSlot)(entity);
        const void* clientClass = networkable != nullptr ? Slot<const void*(__fastcall*)(void*)>(networkable, kGetClientClassSlot)(networkable) : nullptr;
        if (clientClass == nullptr)
            continue;
        const char* className = Field<const char*>(clientClass, kClientClassName);
        if (className == nullptr || (std::strcmp(className, kSpriteClass) != 0 && std::strcmp(className, kOrientedSpriteClass) != 0))
            continue;
        void* renderable = Slot<void*(__fastcall*)(void*)>(entity, kGetClientRenderableSlot)(entity);
        if (renderable == nullptr || !Slot<bool(__fastcall*)(void*)>(renderable, kShouldDrawSlot)(renderable))
            continue;
        const int renderModeAt = ClassProp(renderModes, clientClass, kRenderModeProp);
        const int colorAt = ClassProp(colors, clientClass, kRenderColorProp);
        const int worldScaleAt = ClassProp(worldScales, clientClass, kWorldSpaceScaleProp);
        const int colorScaleAt = ClassProp(colorScales, clientClass, kHdrColorScaleProp);
        if (renderModeAt == kNoProp || colorAt == kNoProp)
            continue;
        const int mode = Field<uint8_t>(entity, static_cast<size_t>(renderModeAt));
        if (mode != 3 && mode != 5 && mode != 7 && mode != 8 && mode != 9)
            continue;
        const void* model = Slot<const void*(__fastcall*)(void*)>(renderable, kGetModelSlot)(renderable);
        if (model == nullptr || Slot<int(__fastcall*)(void*, const void*)>(g_modelInfo, kGetModelTypeSlot)(g_modelInfo, model) != kModelTypeSprite)
            continue;
        const std::byte* sprite = Slot<const std::byte*(__fastcall*)(void*, const void*)>(g_modelInfo, kGetModelExtraDataSlot)(g_modelInfo, model);
        if (sprite == nullptr || mode >= kRenderModes)
            continue;
        void* material = Field<void*>(sprite, kEngineSpriteMaterials + 8 * static_cast<size_t>(mode));
        const char* materialName = material != nullptr ? Slot<const char*(__fastcall*)(void*)>(material, kGetMaterialNameSlot)(material) : nullptr;
        if (materialName == nullptr)
            continue;
        float scale = Slot<float(__fastcall*)(void*)>(entity, kGetRenderScaleSlot)(entity);
        if (scale <= 0.0f)
            scale = 1.0f;
        if (worldScaleAt != kNoProp && Field<uint8_t>(entity, static_cast<size_t>(worldScaleAt)) != 0) {
            const int smaller = std::min(Field<int>(sprite, kEngineSpriteWidth), Field<int>(sprite, kEngineSpriteHeight));
            if (smaller > 0)
                scale /= static_cast<float>(smaller);
        }
        const float up = Field<float>(sprite, kEngineSpriteExtents);
        const float down = Field<float>(sprite, kEngineSpriteExtents + 4);
        const float left = Field<float>(sprite, kEngineSpriteExtents + 8);
        const float right = Field<float>(sprite, kEngineSpriteExtents + 12);
        const int brightness = Slot<int(__fastcall*)(void*)>(entity, kGetRenderBrightnessSlot)(entity);
        const float* origin = Slot<const float*(__fastcall*)(void*)>(renderable, kGetRenderOriginSlot)(renderable);
        if (origin == nullptr || brightness <= 0)
            continue;
        GlowSprite& out = sprites.emplace_back();
        std::copy_n(origin, 3, out.origin);
        const float colorScale = colorScaleAt != kNoProp ? Field<float>(entity, static_cast<size_t>(colorScaleAt)) : 1.0f;
        for (int c = 0; c < 3; ++c)
            out.color[c] = Field<uint8_t>(entity, static_cast<size_t>(colorAt) + static_cast<size_t>(c)) / 255.0f * colorScale;
        out.brightness = std::min(brightness, 255) / 255.0f;
        out.area = std::fabs((up - down) * (right - left)) * scale * scale;
        out.material = materialName;
    }
}

std::string WeaponWorldModel(const ModelEntity& weapon)
{
    if (!Ready())
        return {};
    const auto modelAt = [](int index) { return Slot<const void*(__fastcall*)(void*, int)>(g_modelInfo, kModelInfoGetModelSlot)(g_modelInfo, index); };
    const void* shown = Slot<const void*(__fastcall*)(void*)>(weapon.renderable, kGetModelSlot)(weapon.renderable);
    if (shown == nullptr || modelAt(Field<int>(weapon.entity, kWeaponViewModelIndex)) != shown)
        return {};
    const void* world = modelAt(Field<int>(weapon.entity, kWeaponWorldModelIndex));
    const char* name = world != nullptr ? Slot<const char*(__fastcall*)(void*, const void*)>(g_modelInfo, kGetModelNameSlot)(g_modelInfo, world) : nullptr;
    return name != nullptr ? name : std::string{};
}

bool SetupBones(void* renderable, float (*boneToWorld)[3][4], int maxBones)
{
    if (g_engineClient == nullptr)
        return false;
    const double now = Slot<double(__fastcall*)(void*)>(g_engineClient, kEngineTimeSlot)(g_engineClient);
    return Slot<bool(__fastcall*)(void*, float (*)[3][4], int, int, double)>(renderable, kSetupBonesSlot)(
        renderable, boneToWorld, maxBones, kBoneUsedByAnything, now);
}

}
