// Reproduction harness for the "tried to hook undeclared target" failure reported when our
// standalone-built mods run against a mainline Dusklight build (commit 30def24).
//
// This host stands in for the game: it exports a few functions, loads a *real* MSVC-built mod
// DLL (mod.cpp, built exactly like our shipping mods), then replays the loader's hook-resolution
// and declared-target logic. The resolve_import_thunk / resolve_target functions below are copied
// VERBATIM from extern/dusklight/src/dusk/mods/svc/hook.cpp so the harness exercises the identical
// address math the game uses. If a standalone-built mod's DEFINE_HOOK records do not resolve
// self-consistently, this prints "REPRODUCED"; otherwise it prints "OK".
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "mods/api.h"

// Stand-in "game" functions the mod hooks. dllexport gives each a real export + a mod-side import
// thunk, mirroring how mods link against dusklight.exe's curated export surface. Non-trivial bodies
// keep the linker from folding or discarding them.
extern "C" __declspec(dllexport) void hooked_free_fn(int a, float b) {
    volatile int sink = a + static_cast<int>(b);
    (void)sink;
}
extern "C" __declspec(dllexport) void hooked_free_fn2(int a, float b) {
    volatile int sink = a * 2 + static_cast<int>(b);
    (void)sink;
}
extern "C" __declspec(dllexport) void hooked_free_fn3(int a, float b) {
    volatile int sink = a * 3 - static_cast<int>(b);
    (void)sink;
}

// ---- copied verbatim from src/dusk/mods/svc/hook.cpp (resolve_import_thunk / resolve_target) ----
static void* resolve_import_thunk(void* addr) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    for (int i = 0; i < 8; ++i) {
        const auto* p = static_cast<const uint8_t*>(addr);
        if (p[0] == 0x48 && p[1] == 0xFF && p[2] == 0x25) {  // lld emits a REX.W prefix
            ++p;
        }
        if (p[0] == 0xFF && p[1] == 0x25) {
            int32_t offset;
            std::memcpy(&offset, p + 2, 4);
            addr = const_cast<void*>(*reinterpret_cast<const void* const*>(p + 6 + offset));
            break;
        }
        if (p[0] == 0xE9) {
            int32_t offset;
            std::memcpy(&offset, p + 1, 4);
            addr = const_cast<uint8_t*>(p) + 5 + offset;
        } else {
            break;
        }
    }
#endif
    return addr;
}

static void* resolve_target(void* addr) {
    for (int i = 0; i < 8; ++i) {
        void* next = resolve_import_thunk(addr);
        if (next == addr) {
            break;
        }
        addr = next;
    }
    return addr;
}
// ---- end verbatim ----

int main() {
    HMODULE dll = LoadLibraryA("test_mod.dll");
    if (dll == nullptr) {
        std::printf("FAIL: LoadLibrary(test_mod.dll) err=%lu\n", GetLastError());
        return 3;
    }
    auto* meta = reinterpret_cast<const ModMeta*>(GetProcAddress(dll, "mod_meta"));
    if (meta == nullptr) {
        std::printf("FAIL: mod_meta symbol not found\n");
        return 3;
    }
    const auto* begin = static_cast<const uint8_t*>(meta->records_begin);
    const auto* end = static_cast<const uint8_t*>(meta->records_end);
    std::printf("mod_meta: struct_size=%u begin=%p end=%p span=%lld bytes\n", meta->struct_size,
        meta->records_begin, meta->records_end, static_cast<long long>(end - begin));
    std::printf("exports: hooked_free_fn=%p hooked_free_fn2=%p hooked_free_fn3=%p\n",
        reinterpret_cast<void*>(&hooked_free_fn), reinterpret_cast<void*>(&hooked_free_fn2),
        reinterpret_cast<void*>(&hooked_free_fn3));

    // Replay hook_resolve_mod_records (loader side): walk records, resolve each, populate declared.
    std::vector<ModMetaHookFn*> hookFns;
    const auto* cursor = begin;
    while (cursor < end) {
        uint64_t first = 0;
        std::memcpy(&first, cursor, sizeof(first));
        if (first == 0) {  // linker padding / bounds sentinel
            cursor += 8;
            continue;
        }
        const auto* rec = reinterpret_cast<const ModMetaRecord*>(cursor);
        std::printf("  record @+%lld kind=%u size=%u\n",
            static_cast<long long>(cursor - begin), rec->kind, rec->size);
        if (rec->size < 8 || rec->size % 8 != 0) {
            std::printf("FAIL: bad record size (walk desynced)\n");
            return 3;
        }
        if (rec->kind == MOD_META_HOOK_FN) {
            hookFns.push_back(reinterpret_cast<ModMetaHookFn*>(const_cast<uint8_t*>(cursor)));
        }
        cursor += rec->size;
    }
    std::printf("HOOK_FN records found: %zu\n", hookFns.size());

    std::vector<uintptr_t> declared;
    for (auto* h : hookFns) {
        void* r = resolve_target(h->target);
        h->resolved = r;
        declared.push_back(reinterpret_cast<uintptr_t>(r));
        std::printf("  declare: target=%p -> resolved=%p\n", h->target, r);
    }

    // Replay the mod's install path (service side): resolve_target(record.resolved), check declared.
    int undeclared = 0;
    for (auto* h : hookFns) {
        void* installAddr = resolve_target(h->resolved);
        const bool ok = std::find(declared.begin(), declared.end(),
                            reinterpret_cast<uintptr_t>(installAddr)) != declared.end();
        std::printf("  install: pass=%p resolved=%p -> %s\n", h->resolved, installAddr,
            ok ? "declared" : "UNDECLARED");
        if (!ok) {
            ++undeclared;
        }
    }

    // Diagnostic: scan the whole loaded image for HOOK_FN record signatures. If records exist
    // OUTSIDE [records_begin, records_end), MSVC placed the DEFINE_HOOK template statics outside the
    // modmeta section (ignoring __declspec(allocate)) and the loader can never see them.
    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), dll, &mi, sizeof(mi))) {
        const auto* imgBase = static_cast<const uint8_t*>(mi.lpBaseOfDll);
        const auto* imgEnd = imgBase + mi.SizeOfImage;
        int outside = 0;
        const uint8_t* p = imgBase;
        MEMORY_BASIC_INFORMATION mbi{};
        while (p < imgEnd && VirtualQuery(p, &mbi, sizeof(mbi)) != 0) {
            const auto* rstart = static_cast<const uint8_t*>(mbi.BaseAddress);
            const auto* rend = rstart + mbi.RegionSize;
            const bool readable = mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0 &&
                (mbi.Protect & PAGE_GUARD) == 0;
            if (readable) {
                const uint8_t* scanEnd = rend < imgEnd ? rend : imgEnd;
                for (const uint8_t* q = rstart; q + sizeof(ModMetaHookFn) <= scanEnd; q += 8) {
                    const auto* rec = reinterpret_cast<const ModMetaRecord*>(q);
                    if (rec->kind == MOD_META_HOOK_FN && rec->size == sizeof(ModMetaHookFn)) {
                        const auto* h = reinterpret_cast<const ModMetaHookFn*>(q);
                        const bool inSection = q >= begin && q < end;
                        if (!inSection && h->target != nullptr) {
                            ++outside;
                            std::printf("  MISPLACED HOOK_FN record @%p target=%p (outside modmeta)\n",
                                static_cast<const void*>(q), h->target);
                        }
                    }
                }
            }
            p = rend;
        }
        std::printf("HOOK_FN records located OUTSIDE the modmeta section: %d\n", outside);
    }

    if (hookFns.empty()) {
        std::printf("RESULT: INCONCLUSIVE (no HOOK_FN records in modmeta section)\n");
        return 2;
    }
    if (undeclared != 0) {
        std::printf("RESULT: REPRODUCED (%d undeclared)\n", undeclared);
        return 1;
    }
    std::printf("RESULT: OK (all hook targets declared)\n");
    return 0;
}
