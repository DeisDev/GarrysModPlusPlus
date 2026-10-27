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

bool ModulatesSampleBy2x(std::span<const DWORD> code, UINT index)
{
    std::array<std::array<float, 4>, 256> defs = {};
    std::array<bool, 256> defined = {};
    // The temporary register texld wrote the sample to, once it has.
    int sampled = -1;
    std::optional<bool> result;
    const auto constantValue = [&](DWORD source, float& value) {
        const DWORD reg = source & D3DSP_REGNUM_MASK;
        if (RegisterType(source) != D3DSPR_CONST || reg >= defs.size() || !defined[reg])
            return false;
        value = defs[reg][(source >> D3DSP_SWIZZLE_SHIFT) & 3];
        return true;
    };
    ForEachInstruction(code, [&](size_t at, size_t length) {
        const DWORD opcode = Opcode(code[at]);
        if (result || opcode == D3DSIO_COMMENT || opcode == D3DSIO_DCL || length == 0)
            return;
        if (opcode == D3DSIO_DEF && length == 5) {
            const DWORD reg = code[at + 1] & D3DSP_REGNUM_MASK;
            if (reg < defs.size()) {
                std::memcpy(defs[reg].data(), &code[at + 2], sizeof(float) * 4);
                defined[reg] = true;
            }
            return;
        }
        if (sampled < 0) {
            if (opcode == D3DSIO_TEX && length == 3 && RegisterType(code[at + 3]) == D3DSPR_SAMPLER && (code[at + 3] & D3DSP_REGNUM_MASK) == index &&
                RegisterType(code[at + 1]) == D3DSPR_TEMP)
                sampled = static_cast<int>(code[at + 1] & D3DSP_REGNUM_MASK);
            return;
        }
        // Compilers reuse the sample register's unused .w, so only uses of its colour channels count.
        const DWORD destination = code[at + 1];
        const DWORD writeMask = (destination & D3DSP_WRITEMASK_ALL) >> 16;
        const auto readsColor = [&](DWORD source) {
            if (RegisterType(source) != D3DSPR_TEMP || static_cast<int>(source & D3DSP_REGNUM_MASK) != sampled)
                return false;
            for (DWORD k = 0; k < 4; ++k) {
                if ((writeMask & (1u << k)) && ((source >> (D3DSP_SWIZZLE_SHIFT + 2 * k)) & 3) < 3)
                    return true;
            }
            return false;
        };
        bool reads = false;
        for (size_t k = at + 2; k <= at + length; ++k)
            reads = reads || readsColor(code[k]);
        if (!reads) {
            if (RegisterType(destination) == D3DSPR_TEMP && static_cast<int>(destination & D3DSP_REGNUM_MASK) == sampled && (writeMask & 7) != 0)
                result = false;
            return;
        }
        float a = 0.0f;
        float b = 0.0f;
        float bias = 0.0f;
        result = opcode == D3DSIO_MAD && length == 4 && constantValue(code[at + 4], bias) && bias == -1.0f &&
            ((readsColor(code[at + 2]) && constantValue(code[at + 3], b) && b == 2.0f) ||
                (readsColor(code[at + 3]) && constantValue(code[at + 2], a) && a == 2.0f));
    });
    return result.value_or(false);
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

int DetailBlendMode(std::span<const DWORD> code, UINT index, UINT factorRegister, size_t lateGap)
{
    if (ModulatesSampleBy2x(code, index))
        return 0;
    int sampled = -1;
    size_t sampledAt = 0;
    size_t instruction = 0;
    std::optional<int> mode;
    const auto isFactor = [&](DWORD source) {
        // The constant's .w on every component: swizzle www(w).
        return RegisterType(source) == D3DSPR_CONST && (source & D3DSP_REGNUM_MASK) == factorRegister &&
            ((source & D3DSP_SWIZZLE_MASK) >> D3DSP_SWIZZLE_SHIFT) == 0xFF;
    };
    ForEachInstruction(code, [&](size_t at, size_t length) {
        const DWORD opcode = Opcode(code[at]);
        if (mode || opcode == D3DSIO_COMMENT || opcode == D3DSIO_DCL || opcode == D3DSIO_DEF || length == 0)
            return;
        ++instruction;
        if (sampled < 0) {
            if (opcode == D3DSIO_TEX && length == 3 && RegisterType(code[at + 3]) == D3DSPR_SAMPLER && (code[at + 3] & D3DSP_REGNUM_MASK) == index &&
                RegisterType(code[at + 1]) == D3DSPR_TEMP) {
                sampled = static_cast<int>(code[at + 1] & D3DSP_REGNUM_MASK);
                sampledAt = instruction;
            }
            return;
        }
        const auto readsSample = [&](DWORD source) {
            return RegisterType(source) == D3DSPR_TEMP && static_cast<int>(source & D3DSP_REGNUM_MASK) == sampled;
        };
        bool reads = false;
        for (size_t k = at + 2; k <= at + length; ++k)
            reads = reads || readsSample(code[k]);
        if (!reads)
            return;
        const bool late = instruction - sampledAt > lateGap;
        if ((opcode == D3DSIO_MAD && length == 4 && ((readsSample(code[at + 2]) && isFactor(code[at + 3])) || (isFactor(code[at + 2]) && readsSample(code[at + 3])))) ||
            (opcode == D3DSIO_MUL && length == 3 && ((readsSample(code[at + 2]) && isFactor(code[at + 3])) || (isFactor(code[at + 2]) && readsSample(code[at + 3])))))
            mode = late ? 5 : 1;
        else if (opcode == D3DSIO_LRP && length == 4 && isFactor(code[at + 2]) && readsSample(code[at + 3]))
            mode = 3;
        else
            mode = -1;
    });
    return mode.value_or(-1);
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
