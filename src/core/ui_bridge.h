// Interface and renderer hooks. See ui_bridge.cpp for why these are reachable
// where the game's own functions are not.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mcbe {

class Loader;

namespace ui {

// Hooks what it can find. Returns the number of hooks installed.
size_t install(Loader& loader);

// Raises a named interface event, if a view has been seen. Returns false when
// there is nothing to raise it on yet.
bool trigger_event(const char* name);

uint64_t frame_count();
uint64_t ui_event_count();
void* view();

}  // namespace ui
}  // namespace mcbe
