#include "bridge_link.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#endif

namespace rsb {

#ifdef _WIN32

namespace {
HANDLE g_mapping = nullptr;
drb::SharedState* g_state = nullptr;
bool g_mismatch = false;
} // namespace

drb::SharedState* open_shared_state() {
    if (g_state != nullptr) {
        return g_state;
    }
    const std::wstring name = drb::kMappingPrefix + std::to_wstring(GetCurrentProcessId());
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, drb::kMappingSize, name.c_str());
    if (g_mapping == nullptr) {
        return nullptr;
    }
    void* view = MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, drb::kMappingSize);
    if (view == nullptr) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return nullptr;
    }
    auto* state = static_cast<drb::SharedState*>(view);
    g_mismatch = !drb::claim(state);
    if (g_mismatch) {
        UnmapViewOfFile(view);
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return nullptr;
    }
    g_state = state;
    return g_state;
}

bool shared_state_mismatch() { return g_mismatch; }

void close_shared_state() {
    if (g_state != nullptr) {
        UnmapViewOfFile(g_state);
        g_state = nullptr;
    }
    if (g_mapping != nullptr) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
}

#else

drb::SharedState* open_shared_state() { return nullptr; }
void close_shared_state() {}
bool shared_state_mismatch() { return false; }

#endif

} // namespace rsb
