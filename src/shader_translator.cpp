#include "shader_translator.h"

#include "shader_bytecode.h"

#include <d3d9types.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string_view>

// Binding model of the generated HLSL (shader model 6.6):
// - b0: the float constants the shader reads, gmodpp_c[0] = c[firstFloatConstant].
// - b1: the integer and boolean constants, the half-pixel offset, per sampler the indices of its texture in
//   ResourceDescriptorHeap and of its sampler in SamplerDescriptorHeap (vertex shaders: their s0-s3), and the alpha
//   test reference.
// - Vertex shader inputs keep their D3D9 usage as the semantic (POSITION0, TEXCOORD3, ...).
// - Vertex and pixel shaders link through one fixed interpolant struct (TEXCOORD0-9, COLOR0-1, FOG), so any pair
//   matches. A masked declaration keeps its components in place: a pixel shader reading texcoord5 into v4.z reads the
//   z the vertex shader wrote to texcoord5 ("Match Semantics on vs_3_0 and ps_3_0 Shaders", Shader model 3).
// - Pixel shaders that blend Source's fog read its parameters (g_FogParams) through gmodpp_FogParams and its colour
//   (g_LinearFogColor) through gmodpp_LitFogColor, and those of draws Source leaves unfogged (Variant::fogEpilogue) end
//   with gmodpp_FogBlend (shaders/game_fog.hlsli), all off unless the draw's gmodpp_alpha says otherwise.
// D3D9 semantics follow the shader model reference (learn.microsoft.com, "Asm Shader Reference"): rcp, rsq, log and
// nrm of 0 give +-FLT_MAX, pow and log take the absolute value, mova rounds to nearest, texkill tests xyz, vPos is the
// pixel centre at integer coordinates, and D3D9 pixel centres sit half a pixel off D3D10's (applied to positions).

namespace gmodpp::translator {

namespace {

constexpr DWORD kEndToken = 0x0000FFFF;
constexpr UINT kVertexFloatConstants = 256;
constexpr UINT kPixelFloatConstants = 224;
constexpr UINT kInterpolantTexcoords = 10;
constexpr UINT kInterpolantColors = 2;
constexpr UINT kSamplers = 16;
constexpr DWORD kSourceModifierX2 = 7;
constexpr DWORD kSourceModifierX2Negate = 8;
constexpr const char* kComponents = "xyzw";

DWORD Opcode(DWORD token) { return token & D3DSI_OPCODE_MASK; }

DWORD RegisterType(DWORD token)
{
    return ((token & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) | ((token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2);
}

UINT RegisterNumber(DWORD token) { return token & D3DSP_REGNUM_MASK; }

DWORD WriteMask(DWORD token) { return (token & D3DSP_WRITEMASK_ALL) >> 16; }

const char* UsageSemantic(DWORD usage)
{
    static constexpr const char* kNames[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
        "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };
    return usage < std::size(kNames) ? kNames[usage] : nullptr;
}

std::string MaskString(DWORD mask)
{
    if (mask == 0xF)
        return "";
    std::string text = ".";
    for (int i = 0; i < 4; ++i) {
        if (mask & (1u << i))
            text += kComponents[i];
    }
    return text;
}

std::string Float(float value)
{
    if (std::isinf(value))
        return value > 0 ? "gmodpp_max" : "-gmodpp_max";
    if (std::isnan(value))
        return "asfloat(0x7fc00000u)";
    return std::format("asfloat({:#x}u)", std::bit_cast<std::uint32_t>(value));
}

const char* Comparison(DWORD token)
{
    switch ((token & D3DSHADER_COMPARISON_MASK) >> D3DSHADER_COMPARISON_SHIFT) {
    case D3DSPC_GT: return ">";
    case D3DSPC_EQ: return "==";
    case D3DSPC_GE: return ">=";
    case D3DSPC_LT: return "<";
    case D3DSPC_NE: return "!=";
    case D3DSPC_LE: return "<=";
    default: return nullptr;
    }
}

struct Operand {
    DWORD token = 0;
    DWORD relative = 0;
};

struct Declaration {
    DWORD usage;
    UINT usageIndex;
    UINT reg;
    DWORD mask;
    bool centroid;
};

enum class SampleKind { Plain, Bias, Level, Gradient, Compare };

const char* kHelpers = R"(static const float gmodpp_max = 3.402823466e+38;
float gmodpp_rcp(float x) { return x == 0 ? gmodpp_max : (x == 1 ? 1 : 1 / x); }
float gmodpp_rsq(float x) { x = abs(x); return x == 0 ? gmodpp_max : (x == 1 ? 1 : rsqrt(x)); }
float gmodpp_log(float x) { x = abs(x); return x == 0 ? -gmodpp_max : log2(x); }
float gmodpp_pow(float x, float y) { return exp2(y * gmodpp_log(x)); }
float4 gmodpp_nrm(float4 v) { float f = dot(v.xyz, v.xyz); return v * (f != 0 ? rsqrt(f) : gmodpp_max); }
float4 gmodpp_encode(float4 c) { float3 x = max(c.rgb, 0); return float4(select(x <= 0.0031308, x * 12.92, 1.055 * pow(x, 1.0 / 2.4) - 0.055), c.a); }
float3 gmodpp_decode(float3 c) { c = max(c, 0); return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }
float4 gmodpp_lit(float4 s)
{
    float power = clamp(s.w, -127.9961, 127.9961);
    float4 d = float4(1, 0, 0, 1);
    if (s.x > 0) {
        d.y = s.x;
        if (s.y > 0)
            d.z = pow(s.y, power);
    }
    return d;
}
)";

// The float register of a constant the constant table names, if the shader has it.
std::optional<UINT> FloatRegister(const std::vector<bytecode::Constant>& table, std::string_view name)
{
    for (const bytecode::Constant& constant : table) {
        if (constant.set == bytecode::RegisterSet::Float4 && constant.name == name)
            return constant.index;
    }
    return std::nullopt;
}

// The registers a pixel shader reads Source's fog from, if it blends any: its colour (g_LinearFogColor,
// PSREG_LINEAR_FOG_COLOR in the SDK's shader_constant_register_map.h) and its parameters (g_FogParams, common_ps_fxc.h).
std::optional<UINT> LinearFogColorRegister(const std::vector<bytecode::Constant>& table) { return FloatRegister(table, "g_LinearFogColor"); }
std::optional<UINT> FogParamsRegister(const std::vector<bytecode::Constant>& table) { return FloatRegister(table, "g_FogParams"); }

// Variant::fogEpilogue fogs only what the shader leaves unfogged.
bool FogEpilogue(const Variant& variant, const std::vector<bytecode::Constant>& table)
{
    return variant.fogEpilogue != 0 && variant.forwardLighting == 0 && !LinearFogColorRegister(table);
}

class Translator {
public:
    Translator(std::span<const DWORD> code, const Variant& variant, Translation& out, std::string& error)
        : m_code(code), m_variant(variant), m_out(out), m_error(error)
    {
    }

    bool Run();

private:
    bool Fail(std::string reason)
    {
        if (m_error.empty())
            m_error = std::move(reason);
        return false;
    }

    bool Scan();
    bool Emit();
    std::string Assemble();

    Operand Next(const DWORD*& p) const
    {
        Operand operand{ *p++ };
        if (operand.token & D3DSHADER_ADDRMODE_RELATIVE)
            operand.relative = *p++;
        return operand;
    }

    void Line(const std::string& text) { m_body->append(std::string(m_indent * 4, ' ') + text + "\n"); }

    std::string IntConstant(UINT n)
    {
        if (auto found = m_intDefs.find(n); found != m_intDefs.end())
            return std::format("int4({}, {}, {}, {})", found->second[0], found->second[1], found->second[2], found->second[3]);
        return std::format("gmodpp_i[{}]", n);
    }

    std::string BoolConstant(UINT n)
    {
        if (auto found = m_boolDefs.find(n); found != m_boolDefs.end())
            return found->second ? "true" : "false";
        return std::format("(gmodpp_b[{}].{} != 0)", n / 4, kComponents[n % 4]);
    }

    std::string Predicate(const Operand& operand, int component) const
    {
        DWORD swizzle = (operand.token >> D3DSP_SWIZZLE_SHIFT) & 0xFF;
        std::string text = std::format("p0.{}", kComponents[(swizzle >> (2 * component)) & 3]);
        return (operand.token & D3DSP_SRCMOD_MASK) == D3DSPSM_NOT ? "!" + text : text;
    }

    std::optional<std::string> RelativeIndex(const Operand& operand)
    {
        const DWORD type = RegisterType(operand.relative);
        if (type == D3DSPR_ADDR && m_stage == Stage::Vertex) {
            m_usesAddress = true;
            return std::format("a0.{}", kComponents[(operand.relative >> D3DSP_SWIZZLE_SHIFT) & 3]);
        }
        if (type == D3DSPR_LOOP)
            return "aL";
        Fail(std::format("relative addressing through register type {}", type));
        return std::nullopt;
    }

    std::optional<std::string> Register(const Operand& operand, int offset);
    std::optional<std::string> Source(const Operand& operand, int offset = 0);
    std::optional<std::string> Destination(const Operand& operand);
    bool Assign(const Operand& destination, const std::string& value, const Operand* predicate);
    std::optional<std::string> Sample(UINT sampler, SampleKind kind, const std::string& coordinate, const std::string& extra);
    bool Instruction(DWORD token, const DWORD* params, const DWORD* end);

    std::span<const DWORD> m_code;
    const Variant& m_variant;
    Translation& m_out;
    std::string& m_error;

    Stage m_stage = Stage::Vertex;
    UINT m_major = 0;
    std::map<UINT, std::array<float, 4>> m_floatDefs;
    std::map<UINT, std::array<int, 4>> m_intDefs;
    std::map<UINT, bool> m_boolDefs;
    std::vector<Declaration> m_inputs;
    std::vector<Declaration> m_outputs;
    std::vector<bytecode::Constant> m_constantTable;
    std::set<UINT> m_temps;
    std::set<std::string> m_globals;
    std::set<std::pair<UINT, SampleKind>> m_sampleHelpers;
    UINT m_floatMin = UINT_MAX;
    UINT m_floatMax = 0;
    bool m_usesAddress = false;
    bool m_usesPredicate = false;
    bool m_usesLoop = false;
    bool m_usesPosition = false;
    bool m_usesFace = false;
    // The registers of Source's linear fog colour and fog parameters (pixel shaders), and whether the shader reads them.
    std::optional<UINT> m_fogColorRegister;
    std::optional<UINT> m_fogParamsRegister;
    bool m_usesFogColor = false;
    bool m_usesFogParams = false;
    std::string m_main;
    std::map<UINT, std::string> m_labels;
    std::string* m_body = &m_main;
    int m_indent = 1;
    int m_loopDepth = 0;
    std::vector<int> m_loops;
};

std::optional<std::string> Translator::Register(const Operand& operand, int offset)
{
    const DWORD type = RegisterType(operand.token);
    const UINT n = RegisterNumber(operand.token) + offset;
    switch (type) {
    case D3DSPR_TEMP:
        m_temps.insert(n);
        return std::format("r{}", n);
    case D3DSPR_INPUT:
        if (operand.token & D3DSHADER_ADDRMODE_RELATIVE) {
            Fail("relative addressing of input registers");
            return std::nullopt;
        }
        return std::format("v{}", n);
    case D3DSPR_CONST: {
        if (operand.token & D3DSHADER_ADDRMODE_RELATIVE) {
            auto index = RelativeIndex(operand);
            if (!index)
                return std::nullopt;
            UINT last = m_stage == Stage::Vertex ? kVertexFloatConstants - 1 : kPixelFloatConstants - 1;
            UINT first = n;
            for (const bytecode::Constant& constant : m_constantTable) {
                if (constant.set == bytecode::RegisterSet::Float4 && n >= constant.index && n < constant.index + constant.count) {
                    first = constant.index;
                    last = constant.index + constant.count - 1;
                }
            }
            m_floatMin = std::min(m_floatMin, first);
            m_floatMax = std::max(m_floatMax, last);
            return std::format("gmodpp_c[{} - GMODPP_FIRST + {}]", n, *index);
        }
        if (m_floatDefs.contains(n))
            return std::format("d{}", n);
        m_floatMin = std::min(m_floatMin, n);
        m_floatMax = std::max(m_floatMax, n);
        if (m_fogColorRegister && n == *m_fogColorRegister) {
            m_usesFogColor = true;
            return std::string("gmodpp_fogColor");
        }
        if (m_fogParamsRegister && n == *m_fogParamsRegister) {
            m_usesFogParams = true;
            return std::string("gmodpp_fogParams");
        }
        return std::format("gmodpp_c[{} - GMODPP_FIRST]", n);
    }
    case D3DSPR_ADDR:
        if (m_stage == Stage::Vertex) {
            m_usesAddress = true;
            return std::string("float4(a0)");
        }
        return std::format("t{}", n);
    case D3DSPR_CONSTINT:
        return std::format("float4({})", IntConstant(n));
    case D3DSPR_CONSTBOOL:
        return std::format("(float4){}", BoolConstant(n));
    case D3DSPR_LOOP:
        return std::string("(float4)aL");
    case D3DSPR_MISCTYPE:
        if (n == D3DSMO_POSITION) {
            m_usesPosition = true;
            return std::string("vPos");
        }
        if (n == D3DSMO_FACE) {
            m_usesFace = true;
            return std::string("vFace");
        }
        break;
    case D3DSPR_PREDICATE:
        m_usesPredicate = true;
        return std::string("float4(p0)");
    default:
        break;
    }
    Fail(std::format("source register type {}", type));
    return std::nullopt;
}

std::optional<std::string> Translator::Source(const Operand& operand, int offset)
{
    auto reg = Register(operand, offset);
    if (!reg)
        return std::nullopt;
    std::string text = *reg;
    const DWORD swizzle = (operand.token >> D3DSP_SWIZZLE_SHIFT) & 0xFF;
    if (swizzle != 0xE4) {
        text += '.';
        for (int i = 0; i < 4; ++i)
            text += kComponents[(swizzle >> (2 * i)) & 3];
    }
    switch ((operand.token & D3DSP_SRCMOD_MASK) >> D3DSP_SRCMOD_SHIFT) {
    case D3DSPSM_NONE >> D3DSP_SRCMOD_SHIFT: return text;
    case D3DSPSM_NEG >> D3DSP_SRCMOD_SHIFT: return "-" + text;
    case D3DSPSM_BIAS >> D3DSP_SRCMOD_SHIFT: return "(" + text + " - 0.5)";
    case D3DSPSM_BIASNEG >> D3DSP_SRCMOD_SHIFT: return "-(" + text + " - 0.5)";
    case D3DSPSM_SIGN >> D3DSP_SRCMOD_SHIFT: return "(" + text + " * 2 - 1)";
    case D3DSPSM_SIGNNEG >> D3DSP_SRCMOD_SHIFT: return "-(" + text + " * 2 - 1)";
    case D3DSPSM_COMP >> D3DSP_SRCMOD_SHIFT: return "(1 - " + text + ")";
    case kSourceModifierX2: return "(" + text + " * 2)";
    case kSourceModifierX2Negate: return "-(" + text + " * 2)";
    case D3DSPSM_ABS >> D3DSP_SRCMOD_SHIFT: return "abs(" + text + ")";
    case D3DSPSM_ABSNEG >> D3DSP_SRCMOD_SHIFT: return "-abs(" + text + ")";
    default: break;
    }
    Fail(std::format("source modifier {}", (operand.token & D3DSP_SRCMOD_MASK) >> D3DSP_SRCMOD_SHIFT));
    return std::nullopt;
}

std::optional<std::string> Translator::Destination(const Operand& operand)
{
    const UINT n = RegisterNumber(operand.token);
    if (operand.token & D3DSHADER_ADDRMODE_RELATIVE) {
        Fail("relative addressing of a destination");
        return std::nullopt;
    }
    std::string name;
    switch (RegisterType(operand.token)) {
    case D3DSPR_TEMP:
        m_temps.insert(n);
        return std::format("r{}", n);
    case D3DSPR_ADDR:
        if (m_stage != Stage::Vertex)
            break;
        m_usesAddress = true;
        return std::string("a0");
    case D3DSPR_RASTOUT:
        if (n > D3DSRO_POINT_SIZE)
            break;
        name = n == D3DSRO_POSITION ? "oPos" : n == D3DSRO_FOG ? "oFog" : "oPts";
        m_globals.insert(name);
        return name;
    case D3DSPR_ATTROUT:
        name = std::format("oD{}", n);
        m_globals.insert(name);
        return name;
    case D3DSPR_OUTPUT:
        name = m_major >= 3 ? std::format("o{}", n) : std::format("oT{}", n);
        m_globals.insert(name);
        return name;
    case D3DSPR_COLOROUT:
        m_out.colorOutputs |= 1u << n;
        name = std::format("oC{}", n);
        m_globals.insert(name);
        return name;
    case D3DSPR_DEPTHOUT:
        m_out.writesDepth = true;
        m_globals.insert("oDepth");
        return std::string("oDepth");
    case D3DSPR_PREDICATE:
        m_usesPredicate = true;
        return std::string("p0");
    default:
        break;
    }
    Fail(std::format("destination register type {}", RegisterType(operand.token)));
    return std::nullopt;
}

bool Translator::Assign(const Operand& destination, const std::string& value, const Operand* predicate)
{
    auto name = Destination(destination);
    if (!name)
        return false;
    const DWORD mask = WriteMask(destination.token);
    const DWORD modifiers = destination.token & D3DSP_DSTMOD_MASK;
    if (destination.token & D3DSP_DSTSHIFT_MASK)
        return Fail("destination shift");
    std::string expression = value;
    if (modifiers & D3DSPDM_SATURATE)
        expression = "saturate(" + expression + ")";
    const bool address = *name == "a0";
    const bool predicateRegister = *name == "p0";
    if (address)
        expression = "int4(" + expression + ")";
    else if (!predicateRegister)
        expression = "(float4)(" + expression + ")";
    if (predicate == nullptr) {
        Line(std::format("{}{} = ({}){};", *name, MaskString(mask), expression, MaskString(mask)));
        return true;
    }
    Line(std::format("{{ {} gmodpp_t = {};", address ? "int4" : predicateRegister ? "bool4" : "float4", expression));
    for (int i = 0; i < 4; ++i) {
        if (mask & (1u << i))
            Line(std::format("    if ({}) {}.{} = gmodpp_t.{};", Predicate(*predicate, i), *name, kComponents[i], kComponents[i]));
    }
    Line("}");
    return true;
}

std::optional<std::string> Translator::Sample(UINT sampler, SampleKind kind, const std::string& coordinate, const std::string& extra)
{
    if (sampler >= kSamplers || m_out.samplers[sampler] == TextureType::None) {
        Fail(std::format("sampler s{} is not declared", sampler));
        return std::nullopt;
    }
    if (kind != SampleKind::Level && kind != SampleKind::Compare && m_stage == Stage::Vertex) {
        Fail("implicit-derivative sampling in a vertex shader");
        return std::nullopt;
    }
    const bool cube = m_out.samplers[sampler] != TextureType::Texture2D;
    std::string coords = coordinate + (cube ? ".xyz" : ".xy");
    m_sampleHelpers.insert({ sampler, kind });
    std::string call = std::format("gmodpp_sample{}_{}({}", sampler, static_cast<int>(kind), coords);
    return extra.empty() ? call + ")" : call + ", " + extra + ")";
}

bool Translator::Scan()
{
    bool declarationsDone = false;
    for (size_t i = 1; i < m_code.size();) {
        const DWORD token = m_code[i];
        if (token == kEndToken) {
            declarationsDone = true;
            break;
        }
        const DWORD opcode = Opcode(token);
        const size_t length = opcode == D3DSIO_COMMENT ? (token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT
                                                       : (token & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
        if (i + 1 + length > m_code.size())
            return Fail("truncated instruction");
        const DWORD* params = &m_code[i + 1];
        if (opcode == D3DSIO_DEF && length >= 5) {
            std::array<float, 4> values;
            std::memcpy(values.data(), params + 1, sizeof(values));
            m_floatDefs[RegisterNumber(params[0])] = values;
        } else if (opcode == D3DSIO_DEFI && length >= 5) {
            std::array<int, 4> values;
            std::memcpy(values.data(), params + 1, sizeof(values));
            m_intDefs[RegisterNumber(params[0])] = values;
        } else if (opcode == D3DSIO_DEFB && length >= 2) {
            m_boolDefs[RegisterNumber(params[0])] = params[1] != 0;
        } else if (opcode == D3DSIO_DCL && length >= 2) {
            const DWORD usage = params[0];
            const DWORD reg = params[1];
            const UINT n = RegisterNumber(reg);
            const DWORD type = RegisterType(reg);
            Declaration declaration{ usage & D3DSP_DCL_USAGE_MASK, (usage & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT, n,
                WriteMask(reg), (reg & D3DSPDM_MSAMPCENTROID) != 0 };
            if (type == D3DSPR_SAMPLER) {
                if (n >= kSamplers)
                    return Fail(std::format("sampler s{}", n));
                switch (usage & D3DSP_TEXTURETYPE_MASK) {
                case D3DSTT_2D: m_out.samplers[n] = TextureType::Texture2D; break;
                case D3DSTT_CUBE: m_out.samplers[n] = TextureType::Cube; break;
                case D3DSTT_VOLUME: m_out.samplers[n] = TextureType::Volume; break;
                default: return Fail(std::format("texture type of s{}", n));
                }
            } else if (type == D3DSPR_INPUT || (type == D3DSPR_TEXTURE && m_stage == Stage::Pixel)) {
                if (m_stage == Stage::Pixel && m_major < 3) {
                    declaration.usage = type == D3DSPR_TEXTURE ? D3DDECLUSAGE_TEXCOORD : D3DDECLUSAGE_COLOR;
                    declaration.usageIndex = n;
                    declaration.reg = n | (type == D3DSPR_TEXTURE ? 0x100 : 0);
                }
                m_inputs.push_back(declaration);
            } else if (type == D3DSPR_OUTPUT) {
                m_outputs.push_back(declaration);
            } else if (type == D3DSPR_MISCTYPE) {
                if (n == D3DSMO_POSITION)
                    m_usesPosition = true;
                else if (n == D3DSMO_FACE)
                    m_usesFace = true;
            }
        }
        i += 1 + length;
    }
    if (!declarationsDone)
        return Fail("no end token");
    return true;
}

bool Translator::Instruction(DWORD token, const DWORD* p, const DWORD* end)
{
    const DWORD opcode = Opcode(token);
    const bool predicated = (token & D3DSHADER_INSTRUCTION_PREDICATED) != 0;
    auto source = [&](const Operand& operand) { return Source(operand); };
    // Instructions with a destination: dst [, predicate], sources.
    auto withDestination = [&](int sourceCount, auto build) -> bool {
        Operand destination = Next(p);
        Operand predicate;
        if (predicated)
            predicate = Next(p);
        std::vector<Operand> sources;
        while (p < end && (sourceCount < 0 || static_cast<int>(sources.size()) < sourceCount))
            sources.push_back(Next(p));
        if (sources.empty() || (sourceCount >= 0 && static_cast<int>(sources.size()) != sourceCount))
            return Fail(std::format("opcode {} has too few operands", opcode));
        std::optional<std::string> value = build(destination, sources);
        if (!value)
            return false;
        if (predicated)
            m_usesPredicate = true;
        return Assign(destination, *value, predicated ? &predicate : nullptr);
    };
    auto unary = [&](const char* format) {
        return withDestination(1, [&](const Operand&, const std::vector<Operand>& s) -> std::optional<std::string> {
            auto a = source(s[0]);
            if (!a)
                return std::nullopt;
            return std::vformat(format, std::make_format_args(*a));
        });
    };
    auto binary = [&](const char* format) {
        return withDestination(2, [&](const Operand&, const std::vector<Operand>& s) -> std::optional<std::string> {
            auto a = source(s[0]);
            auto b = source(s[1]);
            if (!a || !b)
                return std::nullopt;
            return std::vformat(format, std::make_format_args(*a, *b));
        });
    };
    auto ternary = [&](const char* format) {
        return withDestination(3, [&](const Operand&, const std::vector<Operand>& s) -> std::optional<std::string> {
            auto a = source(s[0]);
            auto b = source(s[1]);
            auto c = source(s[2]);
            if (!a || !b || !c)
                return std::nullopt;
            return std::vformat(format, std::make_format_args(*a, *b, *c));
        });
    };
    auto matrix = [&](int rows, const char* swizzle) {
        return withDestination(2, [&](const Operand&, const std::vector<Operand>& s) -> std::optional<std::string> {
            auto a = source(s[0]);
            if (!a)
                return std::nullopt;
            std::string value = "float4(";
            for (int row = 0; row < 4; ++row) {
                if (row > 0)
                    value += ", ";
                if (row >= rows) {
                    value += "0";
                    continue;
                }
                auto b = Source(s[1], row);
                if (!b)
                    return std::nullopt;
                value += std::format("dot(({}){}, ({}){})", *a, swizzle, *b, swizzle);
            }
            return value + ")";
        });
    };
    auto condition = [&](const Operand& operand) -> std::optional<std::string> {
        if (RegisterType(operand.token) == D3DSPR_PREDICATE) {
            m_usesPredicate = true;
            return Predicate(operand, 0);
        }
        if (RegisterType(operand.token) == D3DSPR_CONSTBOOL)
            return BoolConstant(RegisterNumber(operand.token));
        Fail(std::format("condition register type {}", RegisterType(operand.token)));
        return std::nullopt;
    };
    auto compare = [&]() -> std::optional<std::string> {
        const char* op = Comparison(token);
        Operand a = Next(p);
        Operand b = Next(p);
        auto x = source(a);
        auto y = source(b);
        if (!op || !x || !y) {
            Fail("comparison");
            return std::nullopt;
        }
        return std::format("({}).x {} ({}).x", *x, op, *y);
    };

    switch (opcode) {
    case D3DSIO_NOP:
    case D3DSIO_DCL:
    case D3DSIO_DEF:
    case D3DSIO_DEFI:
    case D3DSIO_DEFB:
    case D3DSIO_COMMENT:
        return true;
    case D3DSIO_MOV: return unary("{}");
    case D3DSIO_MOVA: return unary("round({})");
    case D3DSIO_ADD: return binary("{} + {}");
    case D3DSIO_SUB: return binary("{} - {}");
    case D3DSIO_MUL: return binary("{} * {}");
    case D3DSIO_MAD: return ternary("{} * {} + {}");
    case D3DSIO_RCP: return unary("gmodpp_rcp(({}).x)");
    case D3DSIO_RSQ: return unary("gmodpp_rsq(({}).x)");
    case D3DSIO_DP3: return binary("dot(({}).xyz, ({}).xyz)");
    case D3DSIO_DP4: return binary("dot({}, {})");
    case D3DSIO_MIN: return binary("min({}, {})");
    case D3DSIO_MAX: return binary("max({}, {})");
    case D3DSIO_SLT: return binary("float4({} < {})");
    case D3DSIO_SGE: return binary("float4({} >= {})");
    case D3DSIO_EXP: return unary("exp2(({}).x)");
    case D3DSIO_LOG: return unary("gmodpp_log(({}).x)");
    case D3DSIO_LIT: return unary("gmodpp_lit({})");
    case D3DSIO_DST: return binary("float4(1, ({0}).y * ({1}).y, ({0}).z, ({1}).w)");
    case D3DSIO_LRP: return ternary("lerp({2}, {1}, {0})");
    case D3DSIO_FRC: return unary("frac({})");
    case D3DSIO_M4x4: return matrix(4, ".xyzw");
    case D3DSIO_M4x3: return matrix(3, ".xyzw");
    case D3DSIO_M3x4: return matrix(4, ".xyz");
    case D3DSIO_M3x3: return matrix(3, ".xyz");
    case D3DSIO_M3x2: return matrix(2, ".xyz");
    // Direct3D 9's pow is exp(src1 * log(src0)) with log(0) = -FLT_MAX (pow - vs, log - vs), so pow(0, 0) is 1;
    // HLSL leaves it to the GPU, and NaN dropped the bases of swaying grass ($treeswayfalloffexp 0).
    case D3DSIO_POW: return binary("gmodpp_pow(({}).x, ({}).x)");
    case D3DSIO_CRS: return binary("float4(cross(({}).xyz, ({}).xyz), 0)");
    case D3DSIO_SGN:
        return withDestination(-1, [&](const Operand&, const std::vector<Operand>& s) -> std::optional<std::string> {
            auto a = source(s[0]);
            return a ? std::optional(std::format("float4(sign({}))", *a)) : std::nullopt;
        });
    case D3DSIO_ABS: return unary("abs({})");
    case D3DSIO_NRM: return unary("gmodpp_nrm({})");
    case D3DSIO_SINCOS:
        return withDestination(-1, [&](const Operand&, const std::vector<Operand>& s) -> std::optional<std::string> {
            auto a = source(s[0]);
            return a ? std::optional(std::format("float4(cos(({0}).x), sin(({0}).x), 0, 0)", *a)) : std::nullopt;
        });
    case D3DSIO_CMP: return ternary("select({} >= 0, {}, {})");
    case D3DSIO_DP2ADD: return ternary("dot(({}).xy, ({}).xy) + ({}).x");
    case D3DSIO_DSX: return unary("ddx({})");
    case D3DSIO_DSY: return unary("ddy({})");
    case D3DSIO_SETP: {
        const char* op = Comparison(token);
        if (!op)
            return Fail("setp comparison");
        return withDestination(2, [&](const Operand&, const std::vector<Operand>& s) -> std::optional<std::string> {
            auto a = source(s[0]);
            auto b = source(s[1]);
            if (!a || !b)
                return std::nullopt;
            return std::format("{} {} {}", *a, op, *b);
        });
    }
    case D3DSIO_TEXKILL: {
        Operand operand = Next(p);
        auto reg = Register(operand, 0);
        if (!reg)
            return false;
        Line(std::format("if (any(({}).xyz < 0)) discard;", *reg));
        return true;
    }
    case D3DSIO_TEX:
    case D3DSIO_TEXLDL:
    case D3DSIO_TEXLDD: {
        if (m_major < 2)
            return Fail("shader model 1 texture instruction");
        const int sourceCount = opcode == D3DSIO_TEXLDD ? 4 : 2;
        return withDestination(sourceCount, [&](const Operand&, const std::vector<Operand>& s) -> std::optional<std::string> {
            auto coordinate = source(s[0]);
            if (!coordinate)
                return std::nullopt;
            if (RegisterType(s[1].token) != D3DSPR_SAMPLER) {
                Fail("texture instruction without a sampler");
                return std::nullopt;
            }
            const UINT sampler = RegisterNumber(s[1].token);
            const bool shadow = (m_variant.shadowSamplers >> sampler) & 1;
            const bool project = opcode == D3DSIO_TEX && (token & D3DSI_TEXLD_PROJECT);
            const bool bias = opcode == D3DSIO_TEX && (token & D3DSI_TEXLD_BIAS);
            std::string coords = project ? std::format("(({0}) / ({0}).w)", *coordinate) : "(" + *coordinate + ")";
            std::optional<std::string> sample;
            if (shadow)
                sample = Sample(sampler, SampleKind::Compare, coords, coords + ".z");
            else if (opcode == D3DSIO_TEXLDL)
                sample = Sample(sampler, SampleKind::Level, coords, "(" + *coordinate + ").w");
            else if (opcode == D3DSIO_TEXLDD) {
                auto dx = source(s[2]);
                auto dy = source(s[3]);
                if (!dx || !dy)
                    return std::nullopt;
                const char* dims = m_out.samplers[sampler] == TextureType::Texture2D ? ".xy" : ".xyz";
                sample = Sample(sampler, SampleKind::Gradient, coords, std::format("({}){}, ({}){}", *dx, dims, *dy, dims));
            } else if (bias)
                sample = Sample(sampler, SampleKind::Bias, coords, "(" + *coordinate + ").w");
            else
                sample = Sample(sampler, SampleKind::Plain, coords, "");
            if (!sample)
                return std::nullopt;
            const DWORD swizzle = (s[1].token >> D3DSP_SWIZZLE_SHIFT) & 0xFF;
            if (swizzle == 0xE4)
                return *sample;
            std::string swizzled = "(" + *sample + ").";
            for (int i = 0; i < 4; ++i)
                swizzled += kComponents[(swizzle >> (2 * i)) & 3];
            return swizzled;
        });
    }
    case D3DSIO_IF: {
        auto c = condition(Next(p));
        if (!c)
            return false;
        Line(std::format("if ({}) {{", *c));
        ++m_indent;
        return true;
    }
    case D3DSIO_IFC: {
        auto c = compare();
        if (!c)
            return false;
        Line(std::format("if ({}) {{", *c));
        ++m_indent;
        return true;
    }
    case D3DSIO_ELSE:
        --m_indent;
        Line("} else {");
        ++m_indent;
        return true;
    case D3DSIO_ENDIF:
    case D3DSIO_ENDREP:
        --m_indent;
        Line("}");
        return true;
    case D3DSIO_REP: {
        Operand count = Next(p);
        if (RegisterType(count.token) != D3DSPR_CONSTINT)
            return Fail("rep count register");
        const int depth = m_loopDepth++;
        Line(std::format("for (int gmodpp_rep{0} = 0; gmodpp_rep{0} < {1}.x; ++gmodpp_rep{0}) {{", depth, IntConstant(RegisterNumber(count.token))));
        ++m_indent;
        return true;
    }
    case D3DSIO_LOOP: {
        Next(p);
        Operand count = Next(p);
        if (RegisterType(count.token) != D3DSPR_CONSTINT)
            return Fail("loop count register");
        m_usesLoop = true;
        const int depth = m_loopDepth++;
        m_loops.push_back(depth);
        Line(std::format("{{ int4 gmodpp_loop{0} = {1}; int gmodpp_savedL{0} = aL; aL = gmodpp_loop{0}.y;", depth, IntConstant(RegisterNumber(count.token))));
        Line(std::format("for (int gmodpp_k{0} = 0; gmodpp_k{0} < gmodpp_loop{0}.x; ++gmodpp_k{0}, aL += gmodpp_loop{0}.z) {{", depth));
        ++m_indent;
        return true;
    }
    case D3DSIO_ENDLOOP: {
        if (m_loops.empty())
            return Fail("endloop without loop");
        const int depth = m_loops.back();
        m_loops.pop_back();
        --m_indent;
        Line(std::format("}} aL = gmodpp_savedL{}; }}", depth));
        return true;
    }
    case D3DSIO_BREAK:
        Line("break;");
        return true;
    case D3DSIO_BREAKC: {
        auto c = compare();
        if (!c)
            return false;
        Line(std::format("if ({}) break;", *c));
        return true;
    }
    case D3DSIO_BREAKP: {
        auto c = condition(Next(p));
        if (!c)
            return false;
        Line(std::format("if ({}) break;", *c));
        return true;
    }
    case D3DSIO_CALL:
        Line(std::format("gmodpp_label{}();", RegisterNumber(Next(p).token)));
        return true;
    case D3DSIO_CALLNZ: {
        const UINT label = RegisterNumber(Next(p).token);
        auto c = condition(Next(p));
        if (!c)
            return false;
        Line(std::format("if ({}) gmodpp_label{}();", *c, label));
        return true;
    }
    case D3DSIO_RET:
        Line("return;");
        return true;
    case D3DSIO_LABEL: {
        const UINT label = RegisterNumber(Next(p).token);
        m_body = &m_labels[label];
        m_indent = 1;
        return true;
    }
    default:
        break;
    }
    return Fail(std::format("unsupported opcode {}", opcode));
}

bool Translator::Emit()
{
    for (size_t i = 1; i < m_code.size();) {
        const DWORD token = m_code[i];
        if (token == kEndToken)
            break;
        const DWORD opcode = Opcode(token);
        const size_t length = opcode == D3DSIO_COMMENT ? (token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT
                                                       : (token & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
        const DWORD* params = &m_code[i + 1];
        if (!Instruction(token, params, params + length))
            return false;
        i += 1 + length;
    }
    if (!m_loops.empty())
        return Fail("unterminated loop");
    return true;
}

// The interpolant member a declaration maps to, or nothing for ones without a pixel shader counterpart.
std::optional<std::string> InterpolantMember(DWORD usage, UINT index)
{
    if (usage == D3DDECLUSAGE_TEXCOORD && index < kInterpolantTexcoords)
        return std::format("t{}", index);
    if (usage == D3DDECLUSAGE_COLOR && index < kInterpolantColors)
        return std::format("color{}", index);
    if (usage == D3DDECLUSAGE_FOG && index == 0)
        return std::string("fog");
    return std::nullopt;
}

std::string Interpolants(const std::set<std::string>& centroid)
{
    auto member = [&](const std::string& name, const std::string& semantic) {
        return std::format("    {}float4 {} : {};\n", centroid.contains(name) ? "centroid " : "", name, semantic);
    };
    std::string text = "struct gmodpp_Interpolants {\n    float4 position : SV_Position;\n";
    for (UINT i = 0; i < kInterpolantTexcoords; ++i)
        text += member(std::format("t{}", i), std::format("TEXCOORD{}", i));
    for (UINT i = 0; i < kInterpolantColors; ++i)
        text += member(std::format("color{}", i), std::format("COLOR{}", i));
    text += member("fog", "FOG");
    return text + "};\n";
}

std::string Translator::Assemble()
{
    std::string text = "// Translated by GarrysMod++ from D3D9 shader bytecode.\n";
    if (m_stage == Stage::Pixel && m_variant.forwardLighting != 0) {
        text += "#define GMODPP_GAME_DRAW 1\n";
        if (m_variant.opacityMicromaps != 0)
            text += "#define OPACITY_MICROMAPS 1\n";
        text += "#include \"forward_twin.hlsli\"\n";
    }
    const bool fogEpilogue = m_stage == Stage::Pixel && FogEpilogue(m_variant, m_constantTable);
    if (m_usesFogColor || m_usesFogParams || fogEpilogue)
        text += "#include \"game_fog.hlsli\"\n";
    const UINT first = m_floatMin == UINT_MAX ? 0 : m_floatMin;
    m_out.firstFloatConstant = first;
    m_out.floatConstantCount = m_floatMin == UINT_MAX ? 0 : m_floatMax - m_floatMin + 1;
    text += std::format("#define GMODPP_FIRST {}\n", first);
    if (m_out.floatConstantCount > 0)
        text += std::format("cbuffer gmodpp_floats : register(b0) {{ float4 gmodpp_c[{}]; }};\n", m_out.floatConstantCount);
    text += "cbuffer gmodpp_state : register(b1) {\n    int4 gmodpp_i[16];\n    uint4 gmodpp_b[4];\n    float4 gmodpp_halfPixel;\n"
            "    uint4 gmodpp_textures[4];\n    uint4 gmodpp_samplers[4];\n    float4 gmodpp_alpha;\n    float4 gmodpp_clipPlanes[6];\n};\n";
    text += kHelpers;

    for (const auto& [sampler, kind] : m_sampleHelpers) {
        const TextureType type = m_out.samplers[sampler];
        const char* texture = type == TextureType::Texture2D ? "Texture2D<float4>" : type == TextureType::Cube ? "TextureCube<float4>" : "Texture3D<float4>";
        const char* coordinate = type == TextureType::Texture2D ? "float2" : "float3";
        const std::string fetch = std::format("    {} t = ResourceDescriptorHeap[gmodpp_textures[{}].{}];\n", texture, sampler / 4, kComponents[sampler % 4]);
        const std::string samplerIndex = std::format("gmodpp_samplers[{}].{}", sampler / 4, kComponents[sampler % 4]);
        const std::string name = std::format("gmodpp_sample{}_{}", sampler, static_cast<int>(kind));
        // Variant::encodedSamplers: the colour the 8-bit target's bytes held.
        const bool encoded = m_stage == Stage::Pixel && ((m_variant.encodedSamplers >> sampler) & 1u) != 0;
        const char* open = encoded ? "gmodpp_encode(" : "(";
        switch (kind) {
        case SampleKind::Plain:
            text += std::format("float4 {}({} uv)\n{{\n{}    SamplerState s = SamplerDescriptorHeap[{}];\n    return {}t.Sample(s, uv));\n}}\n", name, coordinate, fetch, samplerIndex, open);
            break;
        case SampleKind::Bias:
            text += std::format("float4 {}({} uv, float bias)\n{{\n{}    SamplerState s = SamplerDescriptorHeap[{}];\n    return {}t.SampleBias(s, uv, bias));\n}}\n", name, coordinate, fetch, samplerIndex, open);
            break;
        case SampleKind::Level:
            text += std::format("float4 {}({} uv, float lod)\n{{\n{}    SamplerState s = SamplerDescriptorHeap[{}];\n    return {}t.SampleLevel(s, uv, lod));\n}}\n", name, coordinate, fetch, samplerIndex, open);
            break;
        case SampleKind::Gradient:
            text += std::format("float4 {0}({1} uv, {1} dx, {1} dy)\n{{\n{2}    SamplerState s = SamplerDescriptorHeap[{3}];\n    return {4}t.SampleGrad(s, uv, dx, dy));\n}}\n", name, coordinate, fetch, samplerIndex, open);
            break;
        case SampleKind::Compare:
            text += std::format("float4 {}({} uv, float reference)\n{{\n{}    SamplerComparisonState s = SamplerDescriptorHeap[{}];\n    return (float4)t.SampleCmpLevelZero(s, uv, reference);\n}}\n", name, coordinate, fetch, samplerIndex);
            break;
        }
    }

    for (const auto& [n, values] : m_floatDefs)
        text += std::format("static const float4 d{} = float4({}, {}, {}, {});\n", n, Float(values[0]), Float(values[1]), Float(values[2]), Float(values[3]));
    for (UINT n : m_temps)
        text += std::format("static float4 r{};\n", n);
    for (const std::string& global : m_globals)
        text += std::format("static float4 {};\n", global);
    if (m_usesAddress)
        text += "static int4 a0;\n";
    if (m_usesPredicate)
        text += "static bool4 p0;\n";
    if (m_usesLoop)
        text += "static int aL;\n";
    if (m_usesPosition)
        text += "static float4 vPos;\n";
    if (m_usesFace)
        text += "static float4 vFace;\n";
    if (m_usesFogColor)
        text += "static float4 gmodpp_fogColor;\n";
    if (m_usesFogParams)
        text += "static float4 gmodpp_fogParams;\n";
    for (const Declaration& input : m_inputs) {
        const bool texture = (input.reg & 0x100) != 0;
        text += std::format("static float4 {}{};\n", texture ? "t" : "v", input.reg & 0xFF);
    }

    for (const auto& [label, body] : m_labels)
        text += std::format("void gmodpp_label{}();\n", label);
    for (const auto& [label, body] : m_labels)
        text += std::format("void gmodpp_label{}()\n{{\n{}}}\n", label, body);
    text += "void gmodpp_main()\n{\n" + m_main + "}\n";

    if (m_stage == Stage::Vertex) {
        text += Interpolants({});
        text += "struct gmodpp_VertexInput {\n";
        for (const Declaration& input : m_inputs) {
            const char* semantic = UsageSemantic(input.usage);
            const bool integer = (m_variant.integerInputs >> input.reg) & 1;
            const bool signedInteger = (m_variant.signedInputs >> input.reg) & 1;
            text += std::format("    {} v{} : {}{};\n", integer ? (signedInteger ? "int4" : "uint4") : "float4", input.reg, semantic, input.usageIndex);
            m_out.inputs.push_back({ static_cast<BYTE>(input.usage), static_cast<BYTE>(input.usageIndex), static_cast<BYTE>(input.reg) });
        }
        if (m_inputs.empty())
            text += "    uint gmodpp_vertex : SV_VertexID;\n";
        const UINT clipPlanes = std::min<UINT>(m_variant.clipPlanes, 6);
        if (clipPlanes > 0) {
            // Clip distances follow the interpolants, so pixel shaders whose inputs end before them still link.
            text += "};\nstruct gmodpp_VertexOutput {\n    gmodpp_Interpolants interpolants;\n    float4 clip0 : SV_ClipDistance0;\n";
            if (clipPlanes > 4)
                text += "    float2 clip1 : SV_ClipDistance1;\n";
            text += "};\ngmodpp_VertexOutput main(gmodpp_VertexInput input)\n{\n";
        } else {
            text += "};\ngmodpp_Interpolants main(gmodpp_VertexInput input)\n{\n";
        }
        for (const Declaration& input : m_inputs)
            text += std::format("    v{0} = float4(input.v{0});\n", input.reg);
        text += "    gmodpp_main();\n    gmodpp_Interpolants output = (gmodpp_Interpolants)0;\n";
        if (m_major >= 3) {
            for (const Declaration& output : m_outputs) {
                if (output.usage == D3DDECLUSAGE_POSITION && output.usageIndex == 0) {
                    text += std::format("    output.position = o{};\n", output.reg);
                    continue;
                }
                auto member = InterpolantMember(output.usage, output.usageIndex);
                if (member)
                    text += std::format("    output.{}{} = o{}{};\n", *member, MaskString(output.mask), output.reg, MaskString(output.mask));
            }
        } else {
            for (const std::string& global : m_globals) {
                if (global == "oPos")
                    text += "    output.position = oPos;\n";
                else if (global == "oFog")
                    text += "    output.fog = oFog;\n";
                else if (global.starts_with("oT"))
                    text += std::format("    output.t{} = {};\n", global.substr(2), global);
                else if (global.starts_with("oD"))
                    text += std::format("    output.color{} = saturate({});\n", global.substr(2), global);
            }
        }
        if (clipPlanes > 0) {
            // D3D9 clips against the position the shader wrote, before the half-pixel offset; unused planes pass.
            text += "    gmodpp_VertexOutput result;\n    result.clip0 = float4(1, 1, 1, 1);\n";
            if (clipPlanes > 4)
                text += "    result.clip1 = float2(1, 1);\n";
            for (UINT i = 0; i < clipPlanes; ++i)
                text += std::format("    result.clip{}.{} = dot(gmodpp_clipPlanes[{}], output.position);\n", i / 4, kComponents[i % 4], i);
            text += "    output.position.xy += gmodpp_halfPixel.xy * output.position.w;\n    result.interpolants = output;\n    return result;\n}\n";
            return text;
        }
        text += "    output.position.xy += gmodpp_halfPixel.xy * output.position.w;\n    return output;\n}\n";
        return text;
    }

    std::set<std::string> centroid;
    for (const Declaration& input : m_inputs) {
        auto member = InterpolantMember(input.usage, input.usageIndex);
        if (member && input.centroid)
            centroid.insert(*member);
    }
    text += Interpolants(centroid);
    text += "void main(gmodpp_Interpolants input";
    if (m_usesFace)
        text += ", bool gmodpp_front : SV_IsFrontFace";
    const bool forward = m_variant.forwardLighting != 0;
    for (UINT i = 0; i < 4; ++i) {
        if ((m_out.colorOutputs & (1u << i)) && (!forward || i == 0))
            text += std::format(", out float4 gmodpp_target{0} : SV_Target{0}", i);
    }
    const bool clearCoverage = m_variant.clearCoverage != 0 && m_out.colorOutputs == 1;
    if (clearCoverage)
        text += ", out float4 gmodpp_coverage : SV_Target1";
    if (m_out.writesDepth)
        text += ", out float gmodpp_depth : SV_Depth";
    text += ")\n{\n";
    if (m_usesPosition)
        text += "    vPos = float4(input.position.xy - 0.5, 0, 0);\n";
    if (m_usesFace)
        text += "    vFace = gmodpp_front ? 1 : -1;\n";
    const std::string viewportSize = "float2(1.0 / gmodpp_halfPixel.x, -1.0 / gmodpp_halfPixel.y)";
    // gmodpp_alpha.w in the main view's translucent pass: 1 while the pass blends the scene's fog colour, 2 another colour.
    if (m_usesFogParams)
        text += std::format("    gmodpp_fogParams = gmodpp_alpha.w == 1 || gmodpp_alpha.w == 2 ? gmodpp_FogParams(gmodpp_c[{0} - GMODPP_FIRST], input.position, {1}) : "
                            "gmodpp_c[{0} - GMODPP_FIRST];\n", *m_fogParamsRegister, viewportSize);
    if (m_usesFogColor)
        text += std::format("    gmodpp_fogColor = gmodpp_alpha.w == 1 ? gmodpp_LitFogColor(gmodpp_c[{0} - GMODPP_FIRST], input.position, {1}) : "
                            "gmodpp_c[{0} - GMODPP_FIRST];\n", *m_fogColorRegister, viewportSize);
    for (const Declaration& input : m_inputs) {
        auto member = InterpolantMember(input.usage, input.usageIndex);
        const bool texture = (input.reg & 0x100) != 0;
        text += std::format("    {}{}{} = input.{}{};\n", texture ? "t" : "v", input.reg & 0xFF, MaskString(input.mask), *member, MaskString(input.mask));
    }
    text += "    gmodpp_main();\n";
    if (m_variant.alphaTest != 0 && m_variant.alphaTest != D3DCMP_ALWAYS && (m_out.colorOutputs & 1)) {
        // D3D9 tests the alpha written to oC0, at 8-bit precision, against ALPHAREF.
        static constexpr const char* kTests[] = { "true", "false", "a < r", "a == r", "a <= r", "a > r", "a != r", "a >= r" };
        text += std::format("    float a = round(saturate(oC0.a) * 255);\n    float r = gmodpp_alpha.x;\n    if (!({}))\n        discard;\n",
            kTests[m_variant.alphaTest]);
    }
    if (forward) {
        text += "    gmodpp_target0 = gmodpp_ForwardTwin(oC0, oC1, oC2, oC3, gmodpp_alpha.w != 0);\n";
    } else {
        for (UINT i = 0; i < 4; ++i) {
            if (m_out.colorOutputs & (1u << i))
                text += std::format("    gmodpp_target{0} = oC{0};\n", i);
        }
        if (m_variant.decodeOutput != 0 && (m_out.colorOutputs & 1u))
            text += "    gmodpp_target0.rgb = gmodpp_decode(gmodpp_target0.rgb);\n";
        if (fogEpilogue && (m_out.colorOutputs & 1u))
            text += std::format("    gmodpp_target0 = gmodpp_FogBlend(gmodpp_target0, input.position, {}, gmodpp_alpha.z);\n", viewportSize);
        if (m_variant.lightWarpSlot != 0 && (m_out.colorOutputs & 8u))
            text += "    gmodpp_target3.a = gmodpp_alpha.y;\n";
    }
    if (clearCoverage)
        text += m_variant.markMirror != 0 ? "    gmodpp_coverage = float4(0.0, 0.0, 1.0, 0.0);\n" : "    gmodpp_coverage = 0;\n";
    if (m_out.writesDepth)
        text += "    gmodpp_depth = oDepth.x;\n";
    return text + "}\n";
}

bool Translator::Run()
{
    if (m_code.empty())
        return Fail("empty shader");
    const DWORD version = m_code[0];
    const DWORD kind = version >> 16;
    if (kind == 0xFFFE)
        m_stage = Stage::Vertex;
    else if (kind == 0xFFFF)
        m_stage = Stage::Pixel;
    else
        return Fail(std::format("version token {:#x}", version));
    m_major = D3DSHADER_VERSION_MAJOR(version);
    if (m_major < 2 || m_major > 3)
        return Fail(std::format("shader model {}.{}", m_major, D3DSHADER_VERSION_MINOR(version)));
    m_out.stage = m_stage;
    m_constantTable = bytecode::ConstantTable(m_code);
    if (m_stage == Stage::Pixel) {
        m_fogColorRegister = LinearFogColorRegister(m_constantTable);
        m_fogParamsRegister = FogParamsRegister(m_constantTable);
    }
    if (!Scan())
        return false;
    for (const Declaration& input : m_inputs) {
        if (m_stage == Stage::Vertex && !UsageSemantic(input.usage))
            return Fail(std::format("vertex input usage {}", input.usage));
        if (m_stage == Stage::Pixel && !InterpolantMember(input.usage, input.usageIndex))
            return Fail(std::format("pixel shader input usage {} index {}", input.usage, input.usageIndex));
    }
    for (const Declaration& output : m_outputs) {
        const bool position = output.usage == D3DDECLUSAGE_POSITION && output.usageIndex == 0;
        if (!position && output.usage != D3DDECLUSAGE_PSIZE && !InterpolantMember(output.usage, output.usageIndex))
            return Fail(std::format("vertex output usage {} index {}", output.usage, output.usageIndex));
    }
    if (m_stage == Stage::Vertex && m_major >= 3) {
        for (const Declaration& output : m_outputs)
            m_globals.insert(std::format("o{}", output.reg));
    }
    if (!Emit())
        return false;
    m_out.hlsl = Assemble();
    return true;
}

}

const wchar_t* Profile(Stage stage, const Variant& variant)
{
    if (stage == Stage::Vertex)
        return L"vs_6_6";
    return variant.forwardLighting != 0 && variant.opacityMicromaps != 0 ? L"ps_6_9" : L"ps_6_6";
}

bool Translate(std::span<const DWORD> code, const Variant& variant, Translation& out, std::string& error)
{
    out = {};
    error.clear();
    return Translator(code, variant, out, error).Run();
}

bool UsesGameFog(std::span<const DWORD> code, const Variant& variant)
{
    if (code.empty() || (code[0] >> 16) != 0xFFFF)
        return false;
    const std::vector<bytecode::Constant> table = bytecode::ConstantTable(code);
    return LinearFogColorRegister(table) || FogParamsRegister(table) || FogEpilogue(variant, table);
}

}
