// Entry point of the injected library.
//
// The loader may end up mapped before or after libminecraftpe.so depending on
// how the APK was patched, so startup waits for the game library to appear
// instead of assuming it is already there.

#include <dlfcn.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "loader.h"
#include "log.h"
#include "module.h"

namespace {

constexpr int kMaxWaitSeconds = 60;
constexpr int kPollIntervalMs = 100;

void wait_for_game_and_start() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kMaxWaitSeconds);

    while (std::chrono::steady_clock::now() < deadline) {
        if (mcbe::LoadedModule::find("libminecraftpe.so", /*quiet=*/true).valid) {
            // The library is mapped, but its static initialisers may still be
            // running. A short settle time costs nothing and avoids hooking
            // code that is about to be relocated.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            mcbe::Loader::instance().start();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }

    MCBE_LOGE("libminecraftpe.so did not appear within %d seconds", kMaxWaitSeconds);
}

}  // namespace

__attribute__((constructor)) static void mcbe_loader_entry() {
    MCBE_LOGI("injected into pid %d", getpid());
    // Never block the linker: it is running under a lock the game will need.
    std::thread(wait_for_game_and_start).detach();
}

__attribute__((destructor)) static void mcbe_loader_exit() { mcbe::Loader::instance().stop(); }

#if defined(__ANDROID__)
#include <jni.h>

// Present so the library is also valid as a System.loadLibrary target; the
// real work still happens in the constructor above.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    MCBE_LOGI("JNI_OnLoad");
    return JNI_VERSION_1_6;
}
#endif
