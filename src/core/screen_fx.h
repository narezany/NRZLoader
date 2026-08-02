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

// Takes over the picture with a shader the mod wrote.
//
// The source is a fragment shader missing its preamble: `vUv`, `uTex`, `uRes`
// and `uTime` are declared already, and anything else the shader wants it
// declares itself. Defining `vec3 effect(vec2 uv)` is enough; a shader that
// writes its own `void main` gets it left alone.
//
// Compiling needs the graphics context, which only exists on the thread that
// draws, so the source is handed over and taken up on the next frame. Whether
// it compiled is reported by shader_error().
void set_shader(const std::string& source);

// Goes back to the built-in effects.
void clear_shader();

// Sets a value the mod's own shader reads, by the name it declared. Values
// for names the shader does not use are kept but do nothing.
void set_uniform(const std::string& name, float value);

// What went wrong with the last shader the mod supplied, or empty when it
// compiled. Only meaningful after a frame has been drawn.
std::string shader_error();

// True while the mod's own shader is the one drawing.
bool shader_active();

// True once at least one frame has been drawn through the shader, which is
// how a mod can tell the game is on OpenGL rather than Vulkan.
bool running();

}  // namespace fx
}  // namespace mcbe
