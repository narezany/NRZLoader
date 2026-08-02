#include "apk_assets.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "log.h"

namespace mcbe {
namespace {

struct Mapping {
    const uint8_t* data = nullptr;
    size_t size = 0;
    int fd = -1;

    ~Mapping() {
        if (data != nullptr) munmap(const_cast<uint8_t*>(data), size);
        if (fd >= 0) close(fd);
    }
};

bool map_file(const std::string& path, Mapping& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size <= 0) {
        close(fd);
        return false;
    }

    void* memory = mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (memory == MAP_FAILED) {
        close(fd);
        return false;
    }

    out.data = static_cast<const uint8_t*>(memory);
    out.size = static_cast<size_t>(info.st_size);
    out.fd = fd;
    return true;
}

struct Entry {
    std::string name;
    uint16_t method = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint32_t local_offset = 0;
};

// Walks the central directory. `visit` returning true stops the walk.
template <typename Visitor>
bool walk_entries(const Mapping& map, Visitor visit) {
    if (map.size < 22) return false;

    const size_t scan_limit = map.size < 65558 ? map.size : 65558;
    size_t eocd = 0;
    bool found = false;
    for (size_t back = 22; back <= scan_limit; ++back) {
        const size_t offset = map.size - back;
        if (memcmp(map.data + offset, "PK\x05\x06", 4) == 0) {
            eocd = offset;
            found = true;
            break;
        }
    }
    if (!found) return false;

    uint16_t count;
    uint32_t directory_offset;
    memcpy(&count, map.data + eocd + 10, 2);
    memcpy(&directory_offset, map.data + eocd + 16, 4);

    size_t cursor = directory_offset;
    for (uint16_t index = 0; index < count; ++index) {
        if (cursor + 46 > map.size) return false;
        if (memcmp(map.data + cursor, "PK\x01\x02", 4) != 0) return false;

        Entry entry;
        uint16_t name_length, extra_length, comment_length;
        memcpy(&entry.method, map.data + cursor + 10, 2);
        memcpy(&entry.compressed_size, map.data + cursor + 20, 4);
        memcpy(&entry.uncompressed_size, map.data + cursor + 24, 4);
        memcpy(&name_length, map.data + cursor + 28, 2);
        memcpy(&extra_length, map.data + cursor + 30, 2);
        memcpy(&comment_length, map.data + cursor + 32, 2);
        memcpy(&entry.local_offset, map.data + cursor + 42, 4);
        entry.name.assign(reinterpret_cast<const char*>(map.data + cursor + 46), name_length);

        if (visit(entry)) return true;
        cursor += 46 + name_length + extra_length + comment_length;
    }
    return false;
}

// The local header repeats the name and extra field, and only it gives the
// real offset of the payload.
const uint8_t* payload_of(const Mapping& map, const Entry& entry) {
    if (entry.local_offset + 30 > map.size) return nullptr;
    uint16_t name_length, extra_length;
    memcpy(&name_length, map.data + entry.local_offset + 26, 2);
    memcpy(&extra_length, map.data + entry.local_offset + 28, 2);

    const size_t start = entry.local_offset + 30 + name_length + extra_length;
    if (start + entry.compressed_size > map.size) return nullptr;
    return map.data + start;
}

bool inflate_raw(const uint8_t* input, size_t input_size, std::string& out, size_t expected) {
    out.assign(expected, '\0');

    z_stream stream {};
    // A negative window size selects raw deflate, which is what zip stores.
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;

    stream.next_in = const_cast<Bytef*>(input);
    stream.avail_in = static_cast<uInt>(input_size);
    stream.next_out = reinterpret_cast<Bytef*>(&out[0]);
    stream.avail_out = static_cast<uInt>(expected);

    const int result = inflate(&stream, Z_FINISH);
    const uLong produced = stream.total_out;
    inflateEnd(&stream);

    if (result != Z_STREAM_END) return false;
    out.resize(produced);
    return true;
}

}  // namespace

bool ApkAssets::open(const std::string& path, ApkAssets& out) {
    std::string file = path;
    const size_t separator = path.find("!/");
    if (separator != std::string::npos) file = path.substr(0, separator);

    Mapping map;
    if (!map_file(file, map)) {
        MCBE_LOGW("cannot open %s", file.c_str());
        return false;
    }

    size_t entries = 0;
    walk_entries(map, [&entries](const Entry&) {
        ++entries;
        return false;
    });
    if (entries == 0) {
        MCBE_LOGW("%s has no zip entries", file.c_str());
        return false;
    }

    out.path_ = file;
    MCBE_LOGI("game package %s: %zu entries", file.c_str(), entries);
    return true;
}

bool ApkAssets::contains(const std::string& entry) const {
    Mapping map;
    if (!map_file(path_, map)) return false;
    return walk_entries(map, [&entry](const Entry& candidate) { return candidate.name == entry; });
}

std::string ApkAssets::find_ending_with(const std::string& suffix) const {
    Mapping map;
    std::string found;
    if (!map_file(path_, map)) return found;

    walk_entries(map, [&suffix, &found](const Entry& candidate) {
        if (candidate.name.size() < suffix.size()) return false;
        if (candidate.name.compare(candidate.name.size() - suffix.size(), suffix.size(), suffix) !=
            0) {
            return false;
        }
        found = candidate.name;
        return true;
    });
    return found;
}

std::string ApkAssets::read(const std::string& entry, bool* ok) const {
    if (ok != nullptr) *ok = false;
    std::string contents;

    Mapping map;
    if (!map_file(path_, map)) return contents;

    Entry wanted;
    const bool found = walk_entries(map, [&entry, &wanted](const Entry& candidate) {
        if (candidate.name != entry) return false;
        wanted = candidate;
        return true;
    });
    if (!found) return contents;

    const uint8_t* payload = payload_of(map, wanted);
    if (payload == nullptr) return contents;

    if (wanted.method == 0) {
        contents.assign(reinterpret_cast<const char*>(payload), wanted.uncompressed_size);
    } else if (wanted.method == 8) {
        if (!inflate_raw(payload, wanted.compressed_size, contents, wanted.uncompressed_size)) {
            MCBE_LOGW("could not decompress %s", entry.c_str());
            return std::string();
        }
    } else {
        MCBE_LOGW("%s uses compression method %u", entry.c_str(), wanted.method);
        return contents;
    }

    if (ok != nullptr) *ok = true;
    return contents;
}

}  // namespace mcbe
