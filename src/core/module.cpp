#include "module.h"

#include <elf.h>
#include <link.h>

#include <cstring>

#include "log.h"

namespace mcbe {
namespace {

struct SearchContext {
    std::string soname;
    LoadedModule* out;
};

bool path_matches(const char* path, const std::string& soname) {
    if (path == nullptr || *path == '\0') return false;
    const std::string full(path);
    if (full.size() < soname.size()) return false;
    // Match on the trailing component so both `/data/.../libfoo.so` and
    // `/data/.../base.apk!/lib/arm64-v8a/libfoo.so` resolve.
    return full.compare(full.size() - soname.size(), soname.size(), soname) == 0;
}

int visit(struct dl_phdr_info* info, size_t, void* data) {
    auto* context = static_cast<SearchContext*>(data);
    if (!path_matches(info->dlpi_name, context->soname)) return 0;

    context->out->valid = true;
    context->out->path = info->dlpi_name;
    context->out->load_bias = static_cast<uintptr_t>(info->dlpi_addr);

    for (uint16_t index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr)& header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) continue;
        ExecutableRange range;
        range.start = static_cast<uintptr_t>(info->dlpi_addr + header.p_vaddr);
        range.length = static_cast<size_t>(header.p_memsz);
        context->out->executable.push_back(range);
    }
    return 1;  // stop iterating
}

}  // namespace

LoadedModule LoadedModule::find(const std::string& soname, bool quiet) {
    LoadedModule module;
    SearchContext context{soname, &module};
    dl_iterate_phdr(visit, &context);

    if (!module.valid) {
        // Startup polls for the game library before it is mapped, and saying
        // so every time would make a normal launch look broken.
        if (!quiet) MCBE_LOGE("module %s is not loaded", soname.c_str());
    } else if (!quiet) {
        MCBE_LOGI("found %s at bias 0x%llx with %zu executable ranges", soname.c_str(),
                  static_cast<unsigned long long>(module.load_bias), module.executable.size());
    }
    return module;
}

}  // namespace mcbe
