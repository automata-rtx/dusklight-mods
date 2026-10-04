// The mod's only game-linked code: a hook that gives the runtime an insertion point between the
// world's translucent geometry and the game's own post-processing. Kept in its own file so the game
// headers it needs never meet the runtime's.

#pragma once

namespace rsp {

// Installs a post-hook on dComIfGd_drawXluListDark, the last translucent world list the frame
// draws (m_Do_graphic.cpp, mDoGph_Painter). After it the frame draws particles, motion blur
// (motionBlure), depth of field (drawDepth2), screen-space particles and bloom. Runs `callback` on
// the game thread after every call. Returns false if the hook could not be installed.
bool install_after_translucent_hook(void (*callback)());

} // namespace rsp
