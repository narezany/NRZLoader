#include "elf_image.h"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

#include "log.h"

namespace mcbe {
namespace {

struct ZipEntryLocation {
    bool found = false;
    uint64_t data_offset = 0;
    uint64_t size = 0;
};

// Minimal zip reader: enough to find an uncompressed entry's payload.
// A library the linker mapped from an APK is always stored, never deflated,
// so we only need the stored case.
ZipEntryLocation locate_zip_entry(const uint8_t* data, size_t size, const std::string& name) {
    ZipEntryLocation result;
    if (size < 22) return result;

    // End of central directory: scan backwards for the signature.
    const size_t max_comment = 64 * 1024;
    const size_t scan_limit = size < max_comment + 22 ? size : max_comment + 22;
    size_t eocd = 0;
    bool have_eocd = false;
    for (size_t back = 22; back <= scan_limit; ++back) {
        const size_t offset = size - back;
        if (memcmp(data + offset, "PK\x05\x06", 4) == 0) {
            eocd = offset;
            have_eocd = true;
            break;
        }
    }
    if (!have_eocd) return result;

    uint32_t directory_offset;
    uint16_t entry_count;
    memcpy(&entry_count, data + eocd + 10, 2);
    memcpy(&directory_offset, data + eocd + 16, 4);

    size_t cursor = directory_offset;
    for (uint16_t index = 0; index < entry_count; ++index) {
        if (cursor + 46 > size) return result;
        if (memcmp(data + cursor, "PK\x01\x02", 4) != 0) return result;

        uint16_t method, name_length, extra_length, comment_length;
        uint32_t compressed_size, uncompressed_size, local_offset;
        memcpy(&method, data + cursor + 10, 2);
        memcpy(&compressed_size, data + cursor + 20, 4);
        memcpy(&uncompressed_size, data + cursor + 24, 4);
        memcpy(&name_length, data + cursor + 28, 2);
        memcpy(&extra_length, data + cursor + 30, 2);
        memcpy(&comment_length, data + cursor + 32, 2);
        memcpy(&local_offset, data + cursor + 42, 4);

        const std::string entry_name(reinterpret_cast<const char*>(data + cursor + 46), name_length);
        if (entry_name == name) {
            if (method != 0) {
                MCBE_LOGE("zip entry %s is compressed; the linker cannot map it either",
                          name.c_str());
                return result;
            }
            if (local_offset + 30 > size) return result;
            uint16_t local_name_length, local_extra_length;
            memcpy(&local_name_length, data + local_offset + 26, 2);
            memcpy(&local_extra_length, data + local_offset + 28, 2);
            result.found = true;
            result.data_offset = local_offset + 30 + local_name_length + local_extra_length;
            result.size = uncompressed_size;
            return result;
        }
        cursor += 46 + name_length + extra_length + comment_length;
    }
    return result;
}

}  // namespace

ElfImage::~ElfImage() {
    if (mapping_ != nullptr) munmap(mapping_, mapping_size_);
}

ElfImage::ElfImage(ElfImage&& other) noexcept { *this = std::move(other); }

ElfImage& ElfImage::operator=(ElfImage&& other) noexcept {
    if (this != &other) {
        if (mapping_ != nullptr) munmap(mapping_, mapping_size_);
        mapping_ = other.mapping_;
        mapping_size_ = other.mapping_size_;
        image_offset_ = other.image_offset_;
        symbols_ = std::move(other.symbols_);
        other.mapping_ = nullptr;
        other.mapping_size_ = 0;
    }
    return *this;
}

ElfImage* ElfImage::open(const std::string& path) {
    std::string file_path = path;
    std::string zip_entry;

    const size_t separator = path.find("!/");
    if (separator != std::string::npos) {
        file_path = path.substr(0, separator);
        zip_entry = path.substr(separator + 2);
    }

    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        MCBE_LOGE("cannot open %s", file_path.c_str());
        return nullptr;
    }

    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size <= 0) {
        close(fd);
        return nullptr;
    }

    void* mapping = mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
        MCBE_LOGE("cannot map %s", file_path.c_str());
        return nullptr;
    }

    auto* image = new ElfImage();
    image->mapping_ = mapping;
    image->mapping_size_ = static_cast<size_t>(info.st_size);

    if (!zip_entry.empty()) {
        const ZipEntryLocation entry =
            locate_zip_entry(static_cast<const uint8_t*>(mapping), image->mapping_size_, zip_entry);
        if (!entry.found) {
            MCBE_LOGE("entry %s not found inside %s", zip_entry.c_str(), file_path.c_str());
            delete image;
            return nullptr;
        }
        image->image_offset_ = entry.data_offset;
    }

    if (!image->parse()) {
        delete image;
        return nullptr;
    }
    return image;
}

bool ElfImage::parse() {
    const auto* base = static_cast<const uint8_t*>(mapping_) + image_offset_;
    const size_t available = mapping_size_ - image_offset_;
    if (available < sizeof(Elf64_Ehdr)) return false;

    const auto* header = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0) {
        MCBE_LOGE("not an ELF image");
        return false;
    }
    if (header->e_ident[EI_CLASS] != ELFCLASS64) {
        MCBE_LOGE("only 64-bit images are supported");
        return false;
    }
    if (header->e_shoff == 0 || header->e_shnum == 0) {
        MCBE_LOGW("image has no section headers; symbol lookup unavailable");
        return true;
    }
    if (header->e_shoff + static_cast<uint64_t>(header->e_shnum) * sizeof(Elf64_Shdr) > available) {
        return false;
    }

    const auto* sections = reinterpret_cast<const Elf64_Shdr*>(base + header->e_shoff);

    for (uint16_t index = 0; index < header->e_shnum; ++index) {
        const Elf64_Shdr& section = sections[index];
        if (section.sh_type != SHT_SYMTAB && section.sh_type != SHT_DYNSYM) continue;
        if (section.sh_link >= header->e_shnum) continue;
        if (section.sh_entsize == 0) continue;

        const Elf64_Shdr& strings = sections[section.sh_link];
        if (section.sh_offset + section.sh_size > available) continue;
        if (strings.sh_offset + strings.sh_size > available) continue;

        const auto* symbols = reinterpret_cast<const Elf64_Sym*>(base + section.sh_offset);
        const char* string_table = reinterpret_cast<const char*>(base + strings.sh_offset);
        const size_t count = section.sh_size / section.sh_entsize;

        for (size_t symbol_index = 0; symbol_index < count; ++symbol_index) {
            const Elf64_Sym& symbol = symbols[symbol_index];
            if (symbol.st_value == 0 || symbol.st_name == 0) continue;
            if (symbol.st_name >= strings.sh_size) continue;
            const char* name = string_table + symbol.st_name;
            // .symtab wins over .dynsym when both carry the name; the values
            // agree, so insertion order does not matter.
            symbols_.emplace(name, symbol.st_value);
        }
    }

    MCBE_LOGI("parsed %zu symbols", symbols_.size());
    return true;
}

uint64_t ElfImage::symbol_offset(const std::string& mangled_name) const {
    const auto entry = symbols_.find(mangled_name);
    return entry == symbols_.end() ? 0 : entry->second;
}

std::unordered_map<std::string, uint64_t> ElfImage::find_containing(const std::string& needle) const {
    std::unordered_map<std::string, uint64_t> matches;
    for (const auto& [name, value] : symbols_) {
        if (name.find(needle) != std::string::npos) matches.emplace(name, value);
    }
    return matches;
}

}  // namespace mcbe
