#include "log.h"

#include <time.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#if defined(__ANDROID__) && !defined(MCBE_NO_ANDROID_LOG)
#define MCBE_USE_ANDROID_LOG 1
#include <android/log.h>
#endif

namespace mcbe::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

// Startup happens before a writable directory is known, and those first lines
// are the interesting ones when something goes wrong, so they are kept until
// a file is available.
std::vector<std::string>& backlog() {
    static std::vector<std::string> lines;
    return lines;
}

const char* level_name(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
    }
    return "INFO";
}

std::string timestamp() {
    timespec now {};
    clock_gettime(CLOCK_REALTIME, &now);
    tm parts {};
    localtime_r(&now.tv_sec, &parts);

    char text[32];
    snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d", parts.tm_hour, parts.tm_min, parts.tm_sec,
             static_cast<int>(now.tv_nsec / 1000000));
    return text;
}

}  // namespace

void set_file(const char* path) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_file != nullptr) fclose(g_file);

    g_file = fopen(path, "w");
    if (g_file == nullptr) return;

    for (const std::string& line : backlog()) {
        fputs(line.c_str(), g_file);
    }
    backlog().clear();
    fflush(g_file);
}

void close_file() {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_file != nullptr) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void write(Level level, const char* tag, const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

#if defined(MCBE_USE_ANDROID_LOG)
    int priority = ANDROID_LOG_INFO;
    switch (level) {
        case Level::Debug: priority = ANDROID_LOG_DEBUG; break;
        case Level::Info: priority = ANDROID_LOG_INFO; break;
        case Level::Warn: priority = ANDROID_LOG_WARN; break;
        case Level::Error: priority = ANDROID_LOG_ERROR; break;
    }
    __android_log_write(priority, tag, buffer);
#else
    fprintf(stderr, "[%s/%s] %s\n", level_name(level), tag, buffer);
#endif

    char line[1200];
    snprintf(line, sizeof(line), "%s %-5s %s\n", timestamp().c_str(), level_name(level), buffer);

    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_file != nullptr) {
        fputs(line, g_file);
        // Flushed every time on purpose: if the game crashes, the last line
        // before the crash is the one worth having.
        fflush(g_file);
    } else if (backlog().size() < 512) {
        backlog().emplace_back(line);
    }
}

}  // namespace mcbe::log
