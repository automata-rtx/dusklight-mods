#include "global.h"

#include "d/d_com_inf_game.h"

#include "mods/svc/hook.hpp"
#include "mods/svc/hook.h"

#include "game_hooks.hpp"

// dComIfGd_drawXluListDark: dComIfGd is the game's draw-list facade (d_com_inf Game draw), "Xlu"
// its translucent lists. This one is the last world list the frame draws; "Dark" is the game's own
// list name and what it classifies is not established (docs/japanese-naming.md, "The draw-list
// taxonomy"). DUSK_NOINLINE in the PC build (d_com_inf_game.h), so it has a symbol to hook.
DEFINE_HOOK(dComIfGd_drawXluListDark, DrawXluListDark);

namespace rsb {
namespace {

void (*g_afterTranslucent)() = nullptr;

void on_draw_xlu_list_dark_post(ModContext*, void*, void*, void*) {
    if (g_afterTranslucent != nullptr) {
        g_afterTranslucent();
    }
}

} // namespace

bool install_after_translucent_hook(void (*callback)()) {
    g_afterTranslucent = callback;
    return mods::hook::add_post<DrawXluListDark>(svc_hook, on_draw_xlu_list_dark_post) == MOD_OK;
}

} // namespace rsb
