#include "symbols.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>

#include "log.h"

namespace mcbe {
namespace {

struct SignatureByte {
    uint8_t value = 0;
    bool wildcard = false;
};

std::vector<SignatureByte> parse_signature(const std::string& pattern) {
    std::vector<SignatureByte> bytes;
    std::istringstream stream(pattern);
    std::string token;
    while (stream >> token) {
        SignatureByte byte;
        if (token == "?" || token == "??") {
            byte.wildcard = true;
        } else {
            byte.value = static_cast<uint8_t>(strtoul(token.c_str(), nullptr, 16));
        }
        bytes.push_back(byte);
    }
    return bytes;
}

}  // namespace

SymbolResolver::~SymbolResolver() {
    if (dl_handle_ != nullptr) dlclose(dl_handle_);
}

bool SymbolResolver::initialise(const std::string& soname) {
    module_ = LoadedModule::find(soname);
    if (!module_.valid) return false;

    // RTLD_NOLOAD gives a handle to the already loaded library without
    // triggering a second load.
    dl_handle_ = dlopen(soname.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (dl_handle_ == nullptr) {
        MCBE_LOGW("dlopen(%s, RTLD_NOLOAD) failed: %s", soname.c_str(), dlerror());
    }

    image_.reset(ElfImage::open(module_.path));
    if (image_ == nullptr) {
        MCBE_LOGW("symbol tables unavailable; falling back to offset map and signatures");
    }
    return true;
}

bool SymbolResolver::load_offset_map(const std::string& path) {
    std::ifstream file(path);
    if (!file) return false;

    std::string line;
    size_t count = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string name = line.substr(0, tab);
        const uint64_t offset = strtoull(line.c_str() + tab + 1, nullptr, 0);
        if (offset == 0) continue;
        offset_map_[name] = offset;
        ++count;
    }
    MCBE_LOGI("offset map: %zu entries from %s", count, path.c_str());
    return count > 0;
}

void* SymbolResolver::resolve(const std::string& mangled_name) {
    if (!module_.valid) return nullptr;

    const auto cached = resolved_.find(mangled_name);
    if (cached != resolved_.end()) return cached->second;

    void* address = nullptr;

    if (dl_handle_ != nullptr) {
        address = dlsym(dl_handle_, mangled_name.c_str());
    }

    if (address == nullptr && image_ != nullptr) {
        const uint64_t offset = image_->symbol_offset(mangled_name);
        if (offset != 0) address = reinterpret_cast<void*>(module_.load_bias + offset);
    }

    if (address == nullptr) {
        const auto entry = offset_map_.find(mangled_name);
        if (entry != offset_map_.end()) {
            address = reinterpret_cast<void*>(module_.load_bias + entry->second);
        }
    }

    if (address != nullptr) resolved_[mangled_name] = address;
    return address;
}

void* SymbolResolver::resolve_containing(const std::string& needle) {
    const std::vector<std::string> matches = find_containing(needle);
    if (matches.empty()) return nullptr;
    if (matches.size() > 1) {
        MCBE_LOGW("%s matches %zu symbols; refusing to guess", needle.c_str(), matches.size());
        for (size_t index = 0; index < matches.size() && index < 4; ++index) {
            MCBE_LOGW("    %s", matches[index].c_str());
        }
        return nullptr;
    }
    return resolve(matches[0]);
}

std::vector<std::string> SymbolResolver::find_containing(const std::string& needle) const {
    std::vector<std::string> names;
    if (image_ != nullptr) {
        for (const auto& [name, _value] : image_->find_containing(needle)) names.push_back(name);
    }
    for (const auto& [name, _offset] : offset_map_) {
        if (name.find(needle) != std::string::npos) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

void* SymbolResolver::scan_signature(const std::string& pattern) const {
    const std::vector<SignatureByte> bytes = parse_signature(pattern);
    if (bytes.empty()) return nullptr;

    for (const ExecutableRange& range : module_.executable) {
        if (range.length < bytes.size()) continue;
        const auto* start = reinterpret_cast<const uint8_t*>(range.start);
        const size_t last = range.length - bytes.size();

        for (size_t offset = 0; offset <= last; ++offset) {
            bool matched = true;
            for (size_t index = 0; index < bytes.size(); ++index) {
                if (bytes[index].wildcard) continue;
                if (start[offset + index] != bytes[index].value) {
                    matched = false;
                    break;
                }
            }
            if (matched) return const_cast<uint8_t*>(start + offset);
        }
    }
    return nullptr;
}

}  // namespace mcbe
