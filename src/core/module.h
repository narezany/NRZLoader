// Locates a loaded shared object and describes its executable ranges.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mcbe {

struct ExecutableRange {
    uintptr_t start = 0;
    size_t length = 0;
};

struct LoadedModule {
    bool valid = false;
    std::string path;          // may use the `base.apk!/lib/...` form
    uintptr_t load_bias = 0;   // add this to a symbol value
    std::vector<ExecutableRange> executable;

    // Finds a module whose path ends with `soname`. `quiet` suppresses the
    // log lines, for callers that poll and expect misses.
    static LoadedModule find(const std::string& soname, bool quiet = false);
};

}  // namespace mcbe
