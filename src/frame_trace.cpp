#include "frame_trace.h"

#include "mod.h"

namespace gmodpp::trace {

namespace {

const char* g_pendingKind = nullptr;
UINT g_pendingDraws = 0;
UINT g_pendingPrimitives = 0;
uint64_t g_presents = 0;

}

void OnFrameBoundary()
{
    if (Active()) {
        Line("---- end of traced frame ----");
        g_active = false;
        return;
    }
    ++g_presents;
    const Options& options = GetOptions();
    if (!options.devMode || g_presents % 30 != 0)
        return;
    std::filesystem::path request = options.gameDirectory / L"gmodpp_trace.request";
    if (DeleteFileW(request.c_str())) {
        g_active = true;
        Line("---- begin traced frame ----");
    }
}

std::string Describe(IDirect3DSurface9* surface)
{
    if (surface == nullptr)
        return "null";
    D3DSURFACE_DESC desc;
    if (FAILED(surface->GetDesc(&desc)))
        return std::format("{}", static_cast<void*>(surface));
    return std::format("{} {}x{} fmt {:#x} msaa {} usage {:#x}", static_cast<void*>(surface), desc.Width, desc.Height,
        static_cast<unsigned>(desc.Format), static_cast<int>(desc.MultiSampleType), desc.Usage);
}

void Draw(const char* kind, UINT count)
{
    if (kind != nullptr && kind == g_pendingKind) {
        ++g_pendingDraws;
        g_pendingPrimitives += count;
        return;
    }
    if (g_pendingKind != nullptr)
        log::Write(std::format("TRACE   {} x{} ({} prims)", g_pendingKind, g_pendingDraws, g_pendingPrimitives));
    g_pendingKind = kind;
    g_pendingDraws = kind ? 1 : 0;
    g_pendingPrimitives = kind ? count : 0;
}

}
