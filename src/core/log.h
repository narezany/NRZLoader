// Logging that goes to logcat on device and to stderr in host tests.
#pragma once

namespace mcbe::log {

enum class Level { Debug, Info, Warn, Error };

void write(Level level, const char* tag, const char* format, ...) __attribute__((format(printf, 3, 4)));

// Mirrors everything to a file as well, so the log can be read with a plain
// file manager instead of a connected computer. Messages emitted before this
// call are replayed into the file.
void set_file(const char* path);

void close_file();

}  // namespace mcbe::log

#define MCBE_LOGD(...) ::mcbe::log::write(::mcbe::log::Level::Debug, "NRZLoader", __VA_ARGS__)
#define MCBE_LOGI(...) ::mcbe::log::write(::mcbe::log::Level::Info, "NRZLoader", __VA_ARGS__)
#define MCBE_LOGW(...) ::mcbe::log::write(::mcbe::log::Level::Warn, "NRZLoader", __VA_ARGS__)
#define MCBE_LOGE(...) ::mcbe::log::write(::mcbe::log::Level::Error, "NRZLoader", __VA_ARGS__)
