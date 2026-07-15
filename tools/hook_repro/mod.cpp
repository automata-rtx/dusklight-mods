// Minimal test mod for the hook-resolution reproduction harness. Built the same way our shipping
// mods are (MSVC, MultiThreadedDLL, _ITERATOR_DEBUG_LEVEL=0, modmeta section, links against the
// host's import library). It declares a service import plus three free-function hooks so the
// modmeta section carries mixed record kinds, exactly like deferred_fog / realtime_sun_shadows.
#include "mods/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/log.h"

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);

// "Game" functions, imported from the host via its import library.
extern "C" void hooked_free_fn(int, float);
extern "C" void hooked_free_fn2(int, float);
extern "C" void hooked_free_fn3(int, float);

DEFINE_HOOK(hooked_free_fn, FreeHook);
DEFINE_HOOK(hooked_free_fn2, FreeHook2);
DEFINE_HOOK(hooked_free_fn3, FreeHook3);

MOD_EXPORT ModResult mod_initialize(ModError*) { return MOD_OK; }
MOD_EXPORT ModResult mod_shutdown(ModError*) { return MOD_OK; }
