#include "shader_bytecode.h"

#include <d3d9types.h>

#include <bit>
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
