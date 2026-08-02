// Reads files out of the game's own APK at runtime.
//
// The interface is described by JSON files shipped inside the APK. Reading
// them from the running game, rather than baking a copy into the loader, means
// a game update changes what we build on instead of breaking it.
#pragma once

#include <string>

namespace mcbe {

class ApkAssets {
public:
    // `path` is the APK, or a path in the `base.apk!/entry` form the Android
    // linker reports; anything after the separator is ignored.
    static bool open(const std::string& path, ApkAssets& out);

    // Returns the entry's contents, decompressing when needed. Empty on
    // failure; `ok` distinguishes an empty file from a missing one.
    std::string read(const std::string& entry, bool* ok = nullptr) const;

    bool contains(const std::string& entry) const;

    // First entry whose name ends with `suffix`, for files whose exact path
    // moves between versions. Empty when nothing matches.
    std::string find_ending_with(const std::string& suffix) const;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

}  // namespace mcbe
