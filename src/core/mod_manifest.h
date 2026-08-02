// What a mod says about itself, and whether the loader should run it.
//
// A packaged mod carries a mod.json next to its code. Two things in it matter
// before anything is loaded: which loader versions it was written for, and
// whether the user switched it off. Both are checked here rather than inside
// the mod, because a mod that is wrong about the loader cannot be trusted to
// notice that itself.
#pragma once

#include <string>
#include <vector>

namespace mcbe::mods {

struct Manifest {
    bool found = false;
    std::string id;
    std::string name;
    std::string version;
    std::string min_loader;  // empty means "any"
    std::string max_loader;  // empty means "any"
};

// Reads mod.json out of a package directory. A missing or unreadable file is
// not an error: a mod may be a single loose file with no manifest at all.
Manifest read_manifest(const std::string& package_directory);

// Compares dotted version numbers a and b, returning -1, 0 or 1. Missing
// parts count as zero, so "1.2" and "1.2.0" are equal, and anything that is
// not a number is treated as zero rather than rejected.
int compare_versions(const std::string& a, const std::string& b);

// The mod ids the user switched off, read from config/disabled.txt.
std::vector<std::string> disabled_ids(const std::string& config_directory);

// The id the loader knows a file by: the manifest's id when there is one,
// otherwise the file's own name.
std::string id_for(const std::string& file_path, const std::string& mods_directory);

// Whether a mod file should run. On false, `reason` says why, in a sentence
// meant for the log.
bool should_run(const std::string& file_path, const std::string& mods_directory,
                const std::string& config_directory, const std::string& loader_version,
                std::string& reason);

}  // namespace mcbe::mods
