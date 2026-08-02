// Reads the symbol tables of a shared object that is already loaded.
//
// dlsym only sees exported dynamic symbols. Minecraft's prologue functions are
// frequently local, so we parse the on-disk ELF ourselves and add the load
// bias to turn a symbol value into a runtime address.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace mcbe {

class ElfImage {
public:
    ~ElfImage();

    ElfImage(const ElfImage&) = delete;
    ElfImage& operator=(const ElfImage&) = delete;
    ElfImage(ElfImage&&) noexcept;
    ElfImage& operator=(ElfImage&&) noexcept;

    // `path` accepts both a plain file and the `<archive>.apk!/entry` form the
    // Android linker reports for libraries mapped straight out of an APK.
    static ElfImage* open(const std::string& path);

    // Symbol value as stored in the file, before the load bias is applied.
    // Returns 0 when the symbol is absent.
    uint64_t symbol_offset(const std::string& mangled_name) const;

    size_t symbol_count() const { return symbols_.size(); }

    // Every symbol whose name contains `needle`, for discovery tooling.
    std::unordered_map<std::string, uint64_t> find_containing(const std::string& needle) const;

private:
    ElfImage() = default;
    bool parse();

    void* mapping_ = nullptr;
    size_t mapping_size_ = 0;
    size_t image_offset_ = 0;  // where the ELF starts inside the mapping
    std::unordered_map<std::string, uint64_t> symbols_;
};

}  // namespace mcbe
