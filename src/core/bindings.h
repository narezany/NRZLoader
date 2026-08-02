// Wires concrete game functions to loader events.
//
// Every binding is defined by a symbol name plus a detour written against the
// signature that symbol is expected to have. Names live in a config file
// rather than in the code, because they change with every game build; a
// binding whose symbol cannot be resolved is skipped with a warning instead
// of taking the whole loader down.
#pragma once

#include <cstddef>
#include <string>

namespace mcbe {

class Loader;

namespace bindings {

// Total number of bindings the loader knows about.
size_t binding_count();

// Reads `config_path` (optional) and installs everything that resolves.
// Returns the number of bindings that became active.
size_t install_all(Loader& loader, const std::string& config_path);

}  // namespace bindings
}  // namespace mcbe
