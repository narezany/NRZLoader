// Draws over the finished frame, just before the game shows it.
//
// The game's own rendering code is unreachable — a retail build keeps no names
// for it — but the moment the frame is handed to the system is a public one:
// eglSwapBuffers. Copying the finished picture into a texture there and drawing
// it back through a shader means effects can be added without knowing anything
// about how the game drew it.
#pragma once

#include <string>

namespace mcbe {

class Loader;

namespace fx {

// Hooks the frame handover. Returns true when the hook went in; false only
// means no effects, never a broken game.
bool install(Loader& loader);

// Sets which effects are on, as "name=amount" pairs: "crt=0.6,glitch=0.2".
// An empty string turns everything off. Unknown names are reported and
// ignored. Returns how many were recognised.
size_t set(const std::string& spec);

// The effects currently on, in the same form set() takes.
std::string describe();

// The names an effect can have, comma separated.
std::string names();

// True once at least one frame has been drawn through the shader, which is
// how a mod can tell the game is on OpenGL rather than Vulkan.
bool running();

}  // namespace fx
}  // namespace mcbe
