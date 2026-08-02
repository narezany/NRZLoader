// Reaches the Java side of the app from native code.
//
// The loader arrives as an ELF dependency rather than through
// System.loadLibrary, so its JNI_OnLoad is never called and it starts with no
// JNIEnv. The way in is to hook one of the game's own JNI entry points: the
// first argument of any Java_* function is a JNIEnv, and for an activity's
// init function the second is the activity itself.
//
// That activity is a Context, which is all that is needed to ask the user for
// all-files access without touching any hidden platform API.
#pragma once

#include <cstddef>
#include <string>

namespace mcbe {

class Loader;

namespace jni {

// Hooks the activity entry point so a JNIEnv can be captured when the game
// calls it. Returns true when the hook went in.
bool install(Loader& loader, const std::string& config_path);

// True once a context has been seen.
bool have_activity();

// Stores the context the bootstrap provider passes in. Preferred over
// anything caught later from the game's own entry points.
void attach_context(void* env, void* context);

// The virtual machine, once anything has been seen; null before that. Callers
// that already hold a JNIEnv should use it rather than attaching again.
void* java_vm();

// The app context: whatever was captured, as a global reference.
void* app_context();

// Opens the system screen where the user grants access to all files.
// Does nothing below Android 11, when access is already granted, or when no
// activity has been captured. Safe to call from any thread; the work happens
// on a thread of its own after a short delay, so the game finishes starting
// before the screen appears.
void request_all_files_access();

}  // namespace jni
}  // namespace mcbe
