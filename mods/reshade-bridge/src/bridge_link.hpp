// The mod's end of the shared state (include/drb_protocol.hpp).

#pragma once

#include "drb_protocol.hpp"

namespace rsb {

// Maps the shared block for this process, creating it if the add-on has not yet. Windows only; null
// on other platforms, or if the block belongs to another protocol version. The mapping stays open
// until close_shared_state(), so the pointer stays valid whatever the add-on does.
drb::SharedState* open_shared_state();
void close_shared_state();
// The last open_shared_state() failed because the add-on that created the block speaks another
// protocol version (the two files come from different builds).
bool shared_state_mismatch();

} // namespace rsb
