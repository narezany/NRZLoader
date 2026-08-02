// Native functions callable from the game's JavaScript.
//
// This is what separates the loader from an add-on. Scripts the game runs
// normally are sandboxed: no files, no settings of their own, no way to reach
// the loader. A function implemented here is ordinary native code.
//
// Arguments travel through properties on the global object rather than through
// the callback structure the engine hands a native function. That structure's
// layout is an engine internal which the exported interface does not describe,
// and guessing it wrong crashes the game; reading a global uses only entry
// points this build is known to export. A small script shim hides the
// arrangement, so a mod just calls nrz.readFile(path).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mcbe {

class Loader;

namespace js {

// Resolves the engine entry points. Returns how many were found.
size_t resolve(Loader& loader);

// True when a native function can be installed and called at all.
bool ready();

// True when arguments and return values can travel too, which needs a few more
// entry points than a bare call.
bool arguments_ready();

// Installs the native entry point and the script shim on a context's global
// object. Must run while the engine has a live handle scope.
bool install(void* isolate, void* context);

uint64_t call_count();

// The shim, exposed so it can be checked without a device.
const char* shim_source();

}  // namespace js
}  // namespace mcbe
