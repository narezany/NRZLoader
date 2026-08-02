#include "mod_manifest.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace mcbe::mods {
namespace {

/**
 * Pulls one string field out of a small json document.
 *
 * A manifest is a handful of fields written by hand, so a whole json parser
 * would be more code than the thing it reads. This looks for the key as a
 * quoted name followed by a colon and a quoted value, which is the only shape
 * a manifest field takes.
 */
std::string field(const std::string& text, const std::string& key) {
    const std::string quoted = "\"" + key + "\"";

    size_t at = text.find(quoted);
    while (at != std::string::npos) {
        size_t cursor = at + quoted.size();
        while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
        if (cursor >= text.size() || text[cursor] != ':') {
            at = text.find(quoted, at + 1);
            continue;
        }

        ++cursor;
        while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
        if (cursor >= text.size() || text[cursor] != '"') return std::string();

        ++cursor;
        std::string value;
        while (cursor < text.size() && text[cursor] != '"') {
            // A backslash escapes whatever follows it, which for these fields
            // is only ever a quote or another backslash.
            if (text[cursor] == '\\' && cursor + 1 < text.size()) ++cursor;
            value += text[cursor];
            ++cursor;
        }
        return value;
    }
    return std::string();
}

std::string trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string parent_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string name_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/** The package directory a file belongs to, or empty when it is loose. */
std::string package_of(const std::string& file_path, const std::string& mods_directory) {
    const std::string parent = parent_of(file_path);
    if (parent.empty() || parent == mods_directory) return std::string();
    return parent;
}

}  // namespace

Manifest read_manifest(const std::string& package_directory) {
    Manifest manifest;
    if (package_directory.empty()) return manifest;

    std::ifstream file(package_directory + "/mod.json");
    if (!file) return manifest;

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    manifest.found = true;
    manifest.id = field(text, "id");
    manifest.name = field(text, "name");
    manifest.version = field(text, "version");
    manifest.min_loader = field(text, "minLoader");
    manifest.max_loader = field(text, "maxLoader");

    if (manifest.id.empty()) manifest.id = name_of(package_directory);
    return manifest;
}

int compare_versions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& text) {
        std::vector<long> numbers;
        size_t cursor = 0;
        while (cursor <= text.size()) {
            const size_t dot = text.find('.', cursor);
            const std::string piece =
                text.substr(cursor, dot == std::string::npos ? std::string::npos : dot - cursor);
            numbers.push_back(strtol(piece.c_str(), nullptr, 10));
            if (dot == std::string::npos) break;
            cursor = dot + 1;
        }
        return numbers;
    };

    const std::vector<long> left = parts(a);
    const std::vector<long> right = parts(b);
    const size_t count = std::max(left.size(), right.size());

    for (size_t index = 0; index < count; ++index) {
        const long one = index < left.size() ? left[index] : 0;
        const long two = index < right.size() ? right[index] : 0;
        if (one != two) return one < two ? -1 : 1;
    }
    return 0;
}

std::vector<std::string> disabled_ids(const std::string& config_directory) {
    std::vector<std::string> ids;

    std::ifstream file(config_directory + "/disabled.txt");
    if (!file) return ids;

    std::string line;
    while (std::getline(file, line)) {
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);

        const std::string id = trim(line);
        if (!id.empty()) ids.push_back(id);
    }
    return ids;
}

std::string id_for(const std::string& file_path, const std::string& mods_directory) {
    const std::string package = package_of(file_path, mods_directory);
    if (package.empty()) return name_of(file_path);

    const Manifest manifest = read_manifest(package);
    return manifest.id.empty() ? name_of(package) : manifest.id;
}

bool should_run(const std::string& file_path, const std::string& mods_directory,
                const std::string& config_directory, const std::string& loader_version,
                std::string& reason) {
    reason.clear();

    const std::string id = id_for(file_path, mods_directory);

    const std::vector<std::string> off = disabled_ids(config_directory);
    if (std::find(off.begin(), off.end(), id) != off.end()) {
        reason = "switched off";
        return false;
    }

    const std::string package = package_of(file_path, mods_directory);
    if (package.empty()) return true;

    const Manifest manifest = read_manifest(package);
    if (!manifest.found) return true;

    if (!manifest.min_loader.empty() &&
        compare_versions(loader_version, manifest.min_loader) < 0) {
        reason = "needs loader " + manifest.min_loader + " or newer, this is " + loader_version;
        return false;
    }
    if (!manifest.max_loader.empty() &&
        compare_versions(loader_version, manifest.max_loader) > 0) {
        reason = "was written for loader " + manifest.max_loader + " or older, this is " +
                 loader_version;
        return false;
    }
    return true;
}

}  // namespace mcbe::mods
