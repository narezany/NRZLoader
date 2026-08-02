// Picks a working directory for the loader and creates its layout.
//
// Where the loader may write depends on the Android version and on which
// permissions the patched app actually holds, so several locations are tried
// in order and the first usable one wins.
#pragma once

#include <string>
#include <vector>

namespace mcbe::paths {

struct Layout {
    bool valid = false;
    std::string root;      // e.g. /sdcard/NRZLoader
    std::string mods;      // root/mods
    std::string config;    // root/config
    std::string log_file;  // root/loader.log
};

// Creates `path` and any missing parent, then checks it is writable.
bool ensure_directory(const std::string& path);

// Returns the layout rooted at the first candidate that works.
Layout choose(const std::vector<std::string>& candidates);

// The locations the loader tries on a device, in order of preference.
std::vector<std::string> default_candidates();

// Every mod file with the given extension, both loose in the mods folder and
// one level down inside a packaged mod's own directory.
//
// A package carries its name, description and icon next to its code, which is
// why it is a directory rather than a bare file; the loader only cares about
// the code, wherever it sits.
std::vector<std::string> collect_mod_files(const std::string& mods_directory,
                                           const std::string& extension);

}  // namespace mcbe::paths
