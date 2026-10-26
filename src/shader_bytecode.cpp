#include "shader_bytecode.h"

#include <d3d9types.h>

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
