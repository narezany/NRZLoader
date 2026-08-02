// Turns a name from the target build into a runtime address.
//
// Four strategies are tried in order, because no single one survives every
// Minecraft build:
//   1. dlsym, for symbols the library exports;
//   2. the on-disk symbol tables, for local symbols dlsym cannot see;
//   3. an offset map generated offline by tools/symgen.py, for stripped builds;
//   4. a byte signature scan, for anything still missing.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "elf_image.h"
#include "module.h"

namespace mcbe {

class SymbolResolver {
public:
    ~SymbolResolver();

    bool initialise(const std::string& soname);

    // Loads `name<tab>hex_offset` lines produced by tools/symgen.py.
    // Offsets are relative to the module, not absolute addresses.
    bool load_offset_map(const std::string& path);

    // Returns nullptr when the symbol cannot be found by any strategy.
    void* resolve(const std::string& mangled_name);

    // Pattern syntax: "1F 20 03 D5 ?? ?? ?? 94". Returns the address of the
    // first match inside the module's executable segments.
    void* scan_signature(const std::string& pattern) const;

    const LoadedModule& module() const { return module_; }
    bool ready() const { return module_.valid; }

    // Resolves a symbol by a distinctive part of its name. Long C++ names are
    // awkward to carry around in full and easy to mistype, and a fragment like
    // "FunctionTemplate3NewE" identifies one uniquely. Returns null when the
    // fragment matches nothing, or more than one thing.
    void* resolve_containing(const std::string& needle);

    // Every known symbol whose name contains `needle`. Used to find entry
    // points whose exact name varies between builds.
    std::vector<std::string> find_containing(const std::string& needle) const;

    // Diagnostics for the startup report.
    size_t resolved_count() const { return resolved_.size(); }

private:
    LoadedModule module_;
    std::unique_ptr<ElfImage> image_;
    void* dl_handle_ = nullptr;
    std::unordered_map<std::string, uint64_t> offset_map_;
    std::unordered_map<std::string, void*> resolved_;
};

}  // namespace mcbe
