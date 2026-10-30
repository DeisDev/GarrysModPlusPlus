#include "shader_bytecode.h"

#include <d3d9types.h>

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <cstring>

namespace gmodpp::bytecode {

namespace {

constexpr DWORD kEndToken = 0x0000FFFF;
constexpr DWORD kConstantTableFourCC = MAKEFOURCC('C', 'T', 'A', 'B');

// D3DXSHADER_CONSTANTTABLE and D3DXSHADER_CONSTANTINFO from the DirectX SDK's d3dx9shader.h.
struct ConstantTableHeader {
    DWORD size;
    DWORD creator;
    DWORD version;
    DWORD constants;
    DWORD constantInfo;
    DWORD flags;
    DWORD target;
};

struct ConstantInfo {
    DWORD name;
    WORD registerSet;
    WORD registerIndex;
    WORD registerCount;
    WORD reserved;
    DWORD typeInfo;
    DWORD defaultValue;
};

DWORD Opcode(DWORD token) { return token & D3DSI_OPCODE_MASK; }

DWORD CommentLength(DWORD token) { return (token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT; }

DWORD InstructionLength(DWORD token) { return (token & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT; }

DWORD RegisterType(DWORD token)
{
    return ((token & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) | ((token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2);
}

// Calls `visit(instructionIndex, length)` for every instruction after the version token; comments are
// passed with their opcode intact so callers can skip or read them.
template <class Visit>
bool ForEachInstruction(std::span<const DWORD> code, Visit visit)
{
    size_t i = 1;
    while (i < code.size()) {
        const DWORD token = code[i];
        if (token == kEndToken)
            return true;
        const size_t length = Opcode(token) == D3DSIO_COMMENT ? CommentLength(token) : InstructionLength(token);
        if (i + 1 + length > code.size())
            return false;
        visit(i, length);
        i += 1 + length;
    }
    return false;
}

}

size_t TokenCount(const DWORD* code)
{
    // Instruction tokens only carry their length from shader model 2.0 on.
    if (D3DSHADER_VERSION_MAJOR(code[0]) < 2)
        return 0;
    constexpr size_t kLimit = 1 << 20;
    size_t i = 1;
    while (i < kLimit) {
        const DWORD token = code[i];
        if (token == kEndToken)
            return i + 1;
        i += 1 + (Opcode(token) == D3DSIO_COMMENT ? CommentLength(token) : InstructionLength(token));
    }
    return 0;
}

std::vector<Constant> ConstantTable(std::span<const DWORD> code)
{
    std::vector<Constant> constants;
    ForEachInstruction(code, [&](size_t at, size_t length) {
        if (Opcode(code[at]) != D3DSIO_COMMENT || length < 1 + sizeof(ConstantTableHeader) / sizeof(DWORD) || code[at + 1] != kConstantTableFourCC)
            return;
        const auto* table = reinterpret_cast<const std::byte*>(&code[at + 2]);
        const size_t tableSize = (length - 1) * sizeof(DWORD);
        ConstantTableHeader header;
        std::memcpy(&header, table, sizeof(header));
        if (header.constantInfo > tableSize || header.constants > (tableSize - header.constantInfo) / sizeof(ConstantInfo))
            return;
        for (DWORD c = 0; c < header.constants; ++c) {
            ConstantInfo info;
            std::memcpy(&info, table + header.constantInfo + c * sizeof(ConstantInfo), sizeof(info));
            if (info.name >= tableSize || info.registerSet > static_cast<WORD>(RegisterSet::Sampler))
                continue;
            const char* name = reinterpret_cast<const char*>(table + info.name);
            constants.push_back({std::string(name, strnlen(name, tableSize - info.name)), static_cast<RegisterSet>(info.registerSet),
                info.registerIndex, info.registerCount});
        }
    });
    return constants;
}

bool WritesColorOutput(std::span<const DWORD> code, UINT index)
{
    bool writes = false;
    ForEachInstruction(code, [&](size_t at, size_t length) {
        const DWORD opcode = Opcode(code[at]);
        if (opcode == D3DSIO_COMMENT || opcode == D3DSIO_DEF || opcode == D3DSIO_DEFI || opcode == D3DSIO_DEFB || opcode == D3DSIO_DCL || length == 0)
            return;
        // Colour outputs are write-only, so only a destination token can name one.
        const DWORD destination = code[at + 1];
        if (RegisterType(destination) == D3DSPR_COLOROUT && (destination & D3DSP_REGNUM_MASK) == index)
            writes = true;
    });
    return writes;
}

bool ReadsSampledColor(std::span<const DWORD> code, UINT sampler)
{
    constexpr DWORD kRgb = 7;
    std::array<DWORD, 32> live = {};
    bool reads = false;
    ForEachInstruction(code, [&](size_t at, size_t length) {
        const DWORD opcode = Opcode(code[at]);
        if (reads || opcode == D3DSIO_COMMENT || opcode == D3DSIO_DEF || opcode == D3DSIO_DEFI || opcode == D3DSIO_DEFB || opcode == D3DSIO_DCL ||
            length == 0)
            return;
        const bool destination = opcode != D3DSIO_TEXKILL && opcode != D3DSIO_IF && opcode != D3DSIO_IFC && opcode != D3DSIO_BREAKC &&
            opcode != D3DSIO_BREAKP && opcode != D3DSIO_CALLNZ && opcode != D3DSIO_REP && opcode != D3DSIO_LOOP && opcode != D3DSIO_CALL;
        for (size_t p = destination ? 2 : 1; p <= length; ++p) {
            const DWORD source = code[at + p];
            const DWORD number = source & D3DSP_REGNUM_MASK;
            if (RegisterType(source) != D3DSPR_TEMP || number >= live.size() || live[number] == 0)
                continue;
            for (DWORD c = 0; c < 4; ++c) {
                if ((live[number] >> ((source >> (D3DSP_SWIZZLE_SHIFT + 2 * c)) & 3)) & 1)
                    reads = true;
            }
        }
        if (!destination)
            return;
        const DWORD target = code[at + 1];
        const DWORD number = target & D3DSP_REGNUM_MASK;
        if (RegisterType(target) != D3DSPR_TEMP || number >= live.size())
            return;
        const DWORD mask = (target & D3DSP_WRITEMASK_ALL) >> 16;
        live[number] &= ~mask;
        if (opcode == D3DSIO_TEX && length >= 3 && RegisterType(code[at + 3]) == D3DSPR_SAMPLER && (code[at + 3] & D3DSP_REGNUM_MASK) == sampler)
            live[number] |= mask & kRgb;
    });
    return reads;
}

namespace {

constexpr DWORD kPixelShader30 = 0xFFFF0300;
// ps_2_b, which Source uses where it has no ps_3_0 combination.
constexpr DWORD kPixelShader2x = 0xFFFF0201;
constexpr UINT kTempRegisters = 32;
constexpr UINT kSamplers = 16;
constexpr UINT kMaxFloatConstants = 224;

bool IsDeclaration(DWORD opcode)
{
    return opcode == D3DSIO_COMMENT || opcode == D3DSIO_DCL || opcode == D3DSIO_DEF || opcode == D3DSIO_DEFI || opcode == D3DSIO_DEFB;
}

DWORD RegisterNumber(DWORD token) { return token & D3DSP_REGNUM_MASK; }

DWORD WithRegister(DWORD token, DWORD type, DWORD number)
{
    token &= ~(D3DSP_REGTYPE_MASK | D3DSP_REGTYPE_MASK2 | D3DSP_REGNUM_MASK);
    return token | ((type << D3DSP_REGTYPE_SHIFT) & D3DSP_REGTYPE_MASK) | ((type << D3DSP_REGTYPE_SHIFT2) & D3DSP_REGTYPE_MASK2) | (number & D3DSP_REGNUM_MASK);
}

bool IsRelative(DWORD token) { return (token & D3DSHADER_ADDRESSMODE_MASK) == D3DSHADER_ADDRMODE_RELATIVE; }

// The parameter tokens of an instruction that name registers: a dcl's second token, a def's first, every one of
// other instructions (relative-address tokens included; they name address registers, which nothing remaps).
// `visit(token, destination)` may change the token.
template <class Visit>
void ForEachRegister(std::span<DWORD> code, size_t at, size_t length, Visit visit)
{
    const DWORD opcode = Opcode(code[at]);
    if (opcode == D3DSIO_COMMENT)
        return;
    if (opcode == D3DSIO_DCL) {
        if (length >= 2)
            visit(code[at + 2], true);
        return;
    }
    if (opcode == D3DSIO_DEF || opcode == D3DSIO_DEFI || opcode == D3DSIO_DEFB) {
        if (length >= 1)
            visit(code[at + 1], true);
        return;
    }
    for (size_t k = 1; k <= length; ++k)
        visit(code[at + k], k == 1);
}

}

std::optional<ForwardPatch> AppendForwardLighting(std::span<const DWORD> shader, std::span<const DWORD> snippet, UINT uniformBase, const char*& reason)
{
    const DWORD version = shader.empty() ? 0 : shader[0];
    if ((version != kPixelShader30 && version != kPixelShader2x) || snippet.empty() || snippet[0] != version || shader.back() != kEndToken ||
        snippet.back() != kEndToken) {
        reason = "unsupported shader model";
        return std::nullopt;
    }
    const bool sm3 = version == kPixelShader30;
    const UINT constantCount = sm3 ? kMaxFloatConstants : 32;
    std::vector<DWORD> original(shader.begin(), shader.end());
    std::vector<DWORD> added(snippet.begin(), snippet.end());

    // What the shader uses.
    int maxTemp = -1;
    std::array<bool, kSamplers> samplerUsed = {};
    std::array<bool, kMaxFloatConstants> constantUsed = {};
    bool relativeConstant = false;
    size_t firstExecutable = 0;
    bool ok = ForEachInstruction(original, [&](size_t at, size_t length) {
        if (!IsDeclaration(Opcode(original[at])) && firstExecutable == 0)
            firstExecutable = at;
        ForEachRegister(original, at, length, [&](DWORD& token, bool) {
            const DWORD type = RegisterType(token);
            const DWORD number = RegisterNumber(token);
            if (type == D3DSPR_TEMP)
                maxTemp = std::max(maxTemp, static_cast<int>(number));
            else if (type == D3DSPR_SAMPLER && number < kSamplers)
                samplerUsed[number] = true;
            else if (type == D3DSPR_CONST && number < kMaxFloatConstants)
                constantUsed[number] = true;
            relativeConstant = relativeConstant || (type == D3DSPR_CONST && IsRelative(token));
        });
    });
    for (const Constant& c : ConstantTable(original)) {
        if (c.set != RegisterSet::Float4)
            continue;
        for (UINT r = c.index; r < c.index + c.count && r < kMaxFloatConstants; ++r)
            constantUsed[r] = true;
    }
    if (!ok || firstExecutable == 0 || relativeConstant) {
        reason = relativeConstant ? "relative constant addressing" : "malformed instructions";
        return std::nullopt;
    }

    // What the snippet needs: its inputs by TEXCOORD index (v# in ps_3_0, t# in ps_2_b), temporaries, samplers,
    // literal constants and the caller's uniforms.
    std::array<int, 16> inputSlot;
    inputSlot.fill(-1);
    const DWORD inputType = sm3 ? D3DSPR_INPUT : D3DSPR_TEXTURE;
    int snippetMaxTemp = -1;
    std::vector<std::pair<size_t, size_t>> snippetDeclarations;
    std::vector<std::pair<size_t, size_t>> snippetCode;
    std::array<bool, kMaxFloatConstants> defined = {};
    std::array<bool, kMaxFloatConstants> referenced = {};
    ok = ForEachInstruction(added, [&](size_t at, size_t length) {
        const DWORD opcode = Opcode(added[at]);
        if (opcode == D3DSIO_COMMENT)
            return;
        if (opcode == D3DSIO_DEFI || opcode == D3DSIO_DEFB) {
            ok = false;
            return;
        }
        if (opcode == D3DSIO_DCL && length >= 2 && RegisterType(added[at + 2]) == inputType) {
            const DWORD usage = added[at + 1];
            const DWORD reg = RegisterNumber(added[at + 2]);
            if (reg < inputSlot.size())
                inputSlot[reg] = sm3 ? ((usage & D3DSP_DCL_USAGE_MASK) == D3DDECLUSAGE_TEXCOORD ? static_cast<int>((usage & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT) : -1)
                                     : static_cast<int>(reg);
            return;
        }
        (IsDeclaration(opcode) ? snippetDeclarations : snippetCode).emplace_back(at, length);
        ForEachRegister(added, at, length, [&](DWORD& token, bool) {
            const DWORD type = RegisterType(token);
            const DWORD number = RegisterNumber(token);
            if (type == D3DSPR_TEMP)
                snippetMaxTemp = std::max(snippetMaxTemp, static_cast<int>(number));
            if (type == D3DSPR_CONST && number < kMaxFloatConstants) {
                (opcode == D3DSIO_DEF ? defined : referenced)[number] = true;
                ok = ok && !IsRelative(token);
            }
        });
    }) && ok;
    // oC0-oC2 (albedo, normal, position) and oC3 (dropped) move to temporaries after the shader's own.
    const DWORD movedBase = static_cast<DWORD>(maxTemp + 1);
    const DWORD snippetTempBase = movedBase + 4;
    if (!ok || snippetTempBase + snippetMaxTemp + 1 > kTempRegisters) {
        reason = ok ? "too few free temporary registers" : "unsupported snippet";
        return std::nullopt;
    }

    ForwardPatch patch;
    for (UINT s = 0, free = kSamplers; s < 3; ++s) {
        do {
            if (free == 0) {
                reason = "too few free samplers";
                return std::nullopt;
            }
            --free;
        } while (samplerUsed[free]);
        patch.samplers[s] = free;
    }
    // Uniforms keep their registers where the shader leaves them free; everything else takes the highest free ones.
    std::array<int, kMaxFloatConstants> constantMap;
    constantMap.fill(-1);
    auto take = [&](UINT preferred) {
        if (preferred < constantCount && !constantUsed[preferred]) {
            constantUsed[preferred] = true;
            return static_cast<int>(preferred);
        }
        for (int r = static_cast<int>(constantCount) - 1; r >= 0; --r) {
            if (!constantUsed[r]) {
                constantUsed[r] = true;
                return r;
            }
        }
        return -1;
    };
    for (UINT i = 0; i < kForwardUniforms; ++i) {
        const int reg = take(uniformBase + i);
        if (reg < 0) {
            reason = "too few free constants";
            return std::nullopt;
        }
        patch.uniforms[i] = static_cast<UINT>(reg);
        if (uniformBase + i < kMaxFloatConstants)
            constantMap[uniformBase + i] = reg;
    }
    for (UINT r = 0; r < kMaxFloatConstants; ++r) {
        if (!defined[r] && !referenced[r])
            continue;
        if (!defined[r] && constantMap[r] < 0) {
            reason = "the snippet reads constants it neither defines nor takes";
            return std::nullopt;
        }
        if (defined[r] && (constantMap[r] = take(kMaxFloatConstants)) < 0) {
            reason = "too few free constants";
            return std::nullopt;
        }
    }

    bool writesOutputs = false;
    ForEachInstruction(original, [&](size_t at, size_t length) {
        if (at < firstExecutable)
            return;
        ForEachRegister(original, at, length, [&](DWORD& token, bool destination) {
            if (destination && RegisterType(token) == D3DSPR_COLOROUT && RegisterNumber(token) < 4) {
                token = WithRegister(token, D3DSPR_TEMP, movedBase + RegisterNumber(token));
                writesOutputs = true;
            }
        });
    });
    bool remapped = true;
    for (const auto* list : {&snippetDeclarations, &snippetCode}) {
        for (const auto& [at, length] : *list) {
            ForEachRegister(added, at, length, [&](DWORD& token, bool) {
                const DWORD type = RegisterType(token);
                const DWORD number = RegisterNumber(token);
                if (type == D3DSPR_TEMP) {
                    token = WithRegister(token, D3DSPR_TEMP, snippetTempBase + number);
                } else if (type == inputType) {
                    if (number >= inputSlot.size() || inputSlot[number] < 0 || inputSlot[number] > 2)
                        remapped = false;
                    else
                        token = WithRegister(token, D3DSPR_TEMP, movedBase + inputSlot[number]);
                } else if (type == D3DSPR_SAMPLER) {
                    if (number >= 3)
                        remapped = false;
                    else
                        token = WithRegister(token, D3DSPR_SAMPLER, patch.samplers[number]);
                } else if (type == D3DSPR_CONST) {
                    if (number >= kMaxFloatConstants || constantMap[number] < 0)
                        remapped = false;
                    else
                        token = WithRegister(token, D3DSPR_CONST, static_cast<DWORD>(constantMap[number]));
                }
            });
        }
    }
    if (!writesOutputs || !remapped) {
        reason = writesOutputs ? "unsupported snippet registers" : "no colour outputs";
        return std::nullopt;
    }

    auto append = [&](const std::vector<DWORD>& from, size_t at, size_t length) {
        patch.code.insert(patch.code.end(), from.begin() + at, from.begin() + at + 1 + length);
    };
    patch.code.assign(original.begin(), original.begin() + firstExecutable);
    for (const auto& [at, length] : snippetDeclarations)
        append(added, at, length);
    patch.code.insert(patch.code.end(), original.begin() + firstExecutable, original.end() - 1);
    for (const auto& [at, length] : snippetCode)
        append(added, at, length);
    patch.code.push_back(kEndToken);
    return patch;
}

std::optional<std::vector<DWORD>> TintColorOutput(std::span<const DWORD> shader, UINT tintRegister, UINT scaleRegister)
{
    const DWORD version = shader.empty() ? 0 : shader[0];
    if ((version != kPixelShader30 && version != kPixelShader2x) || shader.back() != kEndToken)
        return std::nullopt;
    std::vector<DWORD> code(shader.begin(), shader.end());
    int maxTemp = -1;
    const bool ok = ForEachInstruction(code, [&](size_t at, size_t length) {
        ForEachRegister(code, at, length, [&](DWORD& token, bool) {
            if (RegisterType(token) == D3DSPR_TEMP)
                maxTemp = std::max(maxTemp, static_cast<int>(RegisterNumber(token)));
        });
    });
    const DWORD color = static_cast<DWORD>(maxTemp + 1);
    const DWORD tint = color + 1;
    if (!ok || tint >= kTempRegisters)
        return std::nullopt;
    bool writesOutput = false;
    ForEachInstruction(code, [&](size_t at, size_t length) {
        if (IsDeclaration(Opcode(code[at])) || length == 0)
            return;
        DWORD& destination = code[at + 1];
        if (RegisterType(destination) == D3DSPR_COLOROUT && RegisterNumber(destination) == 0) {
            destination = WithRegister(destination, D3DSPR_TEMP, color);
            writesOutput = true;
        }
    });
    if (!writesOutput)
        return std::nullopt;

    const auto instruction = [](DWORD opcode, DWORD length) { return opcode | (length << D3DSI_INSTLENGTH_SHIFT); };
    const auto destination = [](DWORD type, DWORD number, DWORD mask) { return WithRegister(0x80000000 | mask, type, number); };
    const auto source = [](DWORD type, DWORD number, DWORD swizzle) { return WithRegister(0x80000000 | swizzle, type, number); };
    constexpr DWORD kRgb = D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 | D3DSP_WRITEMASK_2;
    code.pop_back();
    code.insert(code.end(), {
        instruction(D3DSIO_RCP, 2), destination(D3DSPR_TEMP, tint, D3DSP_WRITEMASK_0), source(D3DSPR_CONST, scaleRegister, D3DSP_REPLICATEGREEN),
        instruction(D3DSIO_MUL, 3), destination(D3DSPR_TEMP, tint, kRgb), source(D3DSPR_CONST, tintRegister, D3DSP_NOSWIZZLE),
            source(D3DSPR_TEMP, tint, D3DSP_REPLICATERED),
        instruction(D3DSIO_MUL, 3), destination(D3DSPR_TEMP, color, kRgb), source(D3DSPR_TEMP, color, D3DSP_NOSWIZZLE),
            source(D3DSPR_TEMP, tint, D3DSP_NOSWIZZLE),
        instruction(D3DSIO_MOV, 2), destination(D3DSPR_COLOROUT, 0, D3DSP_WRITEMASK_ALL), source(D3DSPR_TEMP, color, D3DSP_NOSWIZZLE),
        kEndToken,
    });
    return code;
}

int ReplaceDefLiteral(std::span<DWORD> code, float from, float to)
{
    int replaced = 0;
    const DWORD fromBits = std::bit_cast<DWORD>(from);
    const DWORD toBits = std::bit_cast<DWORD>(to);
    ForEachInstruction(code, [&](size_t at, size_t length) {
        if (Opcode(code[at]) != D3DSIO_DEF || length != 5)
            return;
        for (size_t k = at + 2; k < at + 6; ++k) {
            if (code[k] == fromBits) {
                code[k] = toBits;
                ++replaced;
            }
        }
    });
    return replaced;
}

}
