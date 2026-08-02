#include "jni_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

#include "loader.h"
#include "log.h"
#include "overlay.h"

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace mcbe::jni {
namespace {

#if defined(__ANDROID__)

// Called once while the app starts, with (JNIEnv*, jobject activity, ...).
constexpr const char* kDefaultSymbol =
    "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode";

// The startup call happens early enough that the loader can miss it entirely.
// Lifecycle callbacks keep coming for as long as the app runs, and they carry
// the activity in the same place, so hooking a few of them makes the capture
// a matter of when rather than if.
constexpr const char* kLifecyclePatterns[] = {
    "Java_com_google_androidgamesdk_GameActivity_on",
    "Java_com_mojang_minecraftpe_MainActivity_native",
};

constexpr size_t kMaxSlots = 8;

JavaVM* g_vm = nullptr;
jobject g_activity = nullptr;  // global reference, an Activity or the app
bool g_from_bootstrap = false;
std::atomic<bool> g_requested{false};
void* g_originals[kMaxSlots] = {};
size_t g_slots_used = 0;

// All eight arguments are pointer sized and arrive in registers, so passing
// them straight through is correct without knowing the real signature.
void capture(void* env, void* activity) {
    // The bootstrap's context arrives first and is the better one; a later
    // activity must not displace it.
    if (g_activity != nullptr || env == nullptr || activity == nullptr) return;

    auto* jni_env = static_cast<JNIEnv*>(env);
    JavaVM* vm = nullptr;
    if (jni_env->GetJavaVM(&vm) != JNI_OK || vm == nullptr) return;

    g_vm = vm;
    g_activity = jni_env->NewGlobalRef(static_cast<jobject>(activity));
    MCBE_LOGI("captured the java side: vm=%p activity=%p", vm, g_activity);
}

template <size_t Slot>
void* detour(void* env, void* activity, void* a3, void* a4, void* a5, void* a6, void* a7,
             void* a8) {
    capture(env, activity);
    auto original = reinterpret_cast<void* (*)(void*, void*, void*, void*, void*, void*, void*,
                                               void*)>(g_originals[Slot]);
    return original == nullptr ? nullptr : original(env, activity, a3, a4, a5, a6, a7, a8);
}

// One detour per slot, because each has to reach its own trampoline.
void* const kDetours[kMaxSlots] = {
    reinterpret_cast<void*>(&detour<0>), reinterpret_cast<void*>(&detour<1>),
    reinterpret_cast<void*>(&detour<2>), reinterpret_cast<void*>(&detour<3>),
    reinterpret_cast<void*>(&detour<4>), reinterpret_cast<void*>(&detour<5>),
    reinterpret_cast<void*>(&detour<6>), reinterpret_cast<void*>(&detour<7>),
};

// Clears and reports any pending exception, so one failed call cannot poison
// the calls that follow.
bool failed(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck() == JNI_FALSE) return false;
    env->ExceptionClear();
    MCBE_LOGW("java call failed: %s", what);
    return true;
}

int android_version(JNIEnv* env) {
    jclass version = env->FindClass("android/os/Build$VERSION");
    if (version == nullptr || failed(env, "Build.VERSION")) return 0;
    jfieldID field = env->GetStaticFieldID(version, "SDK_INT", "I");
    if (field == nullptr || failed(env, "SDK_INT")) return 0;
    return env->GetStaticIntField(version, field);
}

bool already_granted(JNIEnv* env) {
    jclass environment = env->FindClass("android/os/Environment");
    if (environment == nullptr || failed(env, "Environment")) return false;
    jmethodID method =
        env->GetStaticMethodID(environment, "isExternalStorageManager", "()Z");
    if (method == nullptr || failed(env, "isExternalStorageManager")) return false;
    const jboolean granted = env->CallStaticBooleanMethod(environment, method);
    if (failed(env, "isExternalStorageManager call")) return false;
    return granted == JNI_TRUE;
}

void open_permission_screen(JNIEnv* env) {
    jclass context_class = env->GetObjectClass(g_activity);
    // Starting an activity from a non-activity context needs its own task,
    // which the flag below already sets, so both kinds of context work here.
    jmethodID get_package =
        env->GetMethodID(context_class, "getPackageName", "()Ljava/lang/String;");
    if (get_package == nullptr || failed(env, "getPackageName id")) return;

    auto package = static_cast<jstring>(env->CallObjectMethod(g_activity, get_package));
    if (package == nullptr || failed(env, "getPackageName")) return;

    const char* package_chars = env->GetStringUTFChars(package, nullptr);
    const std::string uri_text = std::string("package:") + (package_chars ? package_chars : "");
    if (package_chars != nullptr) env->ReleaseStringUTFChars(package, package_chars);

    jclass uri_class = env->FindClass("android/net/Uri");
    jmethodID parse =
        env->GetStaticMethodID(uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jstring uri_string = env->NewStringUTF(uri_text.c_str());
    jobject uri = env->CallStaticObjectMethod(uri_class, parse, uri_string);
    if (uri == nullptr || failed(env, "Uri.parse")) return;

    jclass intent_class = env->FindClass("android/content/Intent");
    jmethodID constructor =
        env->GetMethodID(intent_class, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    jstring action = env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
    jobject intent = env->NewObject(intent_class, constructor, action, uri);
    if (intent == nullptr || failed(env, "new Intent")) return;

    jmethodID add_flags = env->GetMethodID(intent_class, "addFlags", "(I)Landroid/content/Intent;");
    env->CallObjectMethod(intent, add_flags, 0x10000000);  // FLAG_ACTIVITY_NEW_TASK
    if (failed(env, "addFlags")) return;

    jmethodID start =
        env->GetMethodID(context_class, "startActivity", "(Landroid/content/Intent;)V");
    if (start == nullptr || failed(env, "startActivity id")) return;

    env->CallVoidMethod(g_activity, start, intent);
    if (failed(env, "startActivity")) return;

    MCBE_LOGI("asked the user for all-files access");
}

void request_worker() {
    // The entry points that carry an activity are mostly input handlers, so on
    // a build where the startup ones have already run before the loader is up,
    // the first one to arrive can be a back press minutes later. Asking for a
    // permission at that moment is worse than not asking: the launcher offers
    // the same screen, at a time the user chose.
    constexpr int kMaxWaitSeconds = 45;
    for (int waited = 0; waited < kMaxWaitSeconds * 2 && g_activity == nullptr; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (g_activity == nullptr || g_vm == nullptr) {
        MCBE_LOGW("no activity within %d seconds; leaving storage access to the launcher",
                  kMaxWaitSeconds);
        return;
    }

    // A moment more so the request does not land mid-launch, where the system
    // is more likely to drop it.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
        MCBE_LOGE("could not attach to the java vm");
        return;
    }

    const int version = android_version(env);
    if (version < 30) {
        MCBE_LOGI("android %d predates all-files access; nothing to ask for", version);
    } else if (already_granted(env)) {
        MCBE_LOGI("all-files access is already granted");
    } else {
        MCBE_LOGI("all-files access is not granted; opening the settings screen");
        open_permission_screen(env);
    }

    g_vm->DetachCurrentThread();
}

std::string symbol_from_config(const std::string& path) {
    std::ifstream file(path);
    if (!file) return kDefaultSymbol;

    std::string line;
    while (std::getline(file, line)) {
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;

        auto trim = [](std::string text) {
            const size_t begin = text.find_first_not_of(" \t\r\n");
            const size_t end = text.find_last_not_of(" \t\r\n");
            return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
        };
        if (trim(line.substr(0, equals)) == "jni.activity_init") {
            const std::string value = trim(line.substr(equals + 1));
            if (!value.empty()) return value;
        }
    }
    return kDefaultSymbol;
}

#endif  // __ANDROID__

}  // namespace

#if defined(__ANDROID__)

bool install(Loader& loader, const std::string& config_path) {
    std::vector<std::string> targets;
    targets.push_back(symbol_from_config(config_path));

    for (const char* pattern : kLifecyclePatterns) {
        for (const std::string& name : loader.symbols().find_containing(pattern)) {
            if (targets.size() >= kMaxSlots) break;
            if (std::find(targets.begin(), targets.end(), name) == targets.end()) {
                targets.push_back(name);
            }
        }
    }

    for (const std::string& symbol : targets) {
        if (g_slots_used >= kMaxSlots) break;

        void* address = loader.symbols().resolve(symbol);
        if (address == nullptr) continue;
        if (loader.install_hook(address, kDetours[g_slots_used], &g_originals[g_slots_used]) !=
            MCBE_OK) {
            continue;
        }

        MCBE_LOGI("jni entry point %s -> %p", symbol.c_str(), address);
        ++g_slots_used;
    }

    if (g_slots_used == 0) {
        MCBE_LOGW("no jni entry point hooked; storage access cannot be requested");
        return false;
    }

    MCBE_LOGI("jni: %zu entry points hooked", g_slots_used);
    return true;
}

bool have_activity() { return g_activity != nullptr; }

void* java_vm() { return g_vm; }

void* app_context() { return g_activity; }

void attach_context(void* env, void* context) {
    if (env == nullptr || context == nullptr) return;

    auto* jni_env = static_cast<JNIEnv*>(env);
    JavaVM* vm = nullptr;
    if (jni_env->GetJavaVM(&vm) != JNI_OK || vm == nullptr) return;

    if (g_activity != nullptr) jni_env->DeleteGlobalRef(g_activity);
    g_vm = vm;
    g_activity = jni_env->NewGlobalRef(static_cast<jobject>(context));
    g_from_bootstrap = true;
    MCBE_LOGI("context handed over by the bootstrap: vm=%p context=%p", vm, g_activity);
}

void request_all_files_access() {
    if (g_requested.exchange(true)) return;
    std::thread(request_worker).detach();
}

#else

bool install(Loader&, const std::string&) { return false; }
bool have_activity() { return false; }
void* java_vm() { return nullptr; }
void* app_context() { return nullptr; }
void request_all_files_access() {}

#endif

}  // namespace mcbe::jni

#if defined(__ANDROID__)
/**
 * Called by the bootstrap provider right after it loads this library.
 *
 * Exported under the name the platform derives from the java class, which is
 * why it sits outside the namespace and carries default visibility.
 */
extern "C" JNIEXPORT void JNICALL
Java_ru_narezany_nrzloader_Bootstrap_nativeAttach(JNIEnv* env, jclass, jobject context) {
    mcbe::jni::attach_context(env, context);

    // Here and nowhere else: a native method called from java can look up the
    // app's own classes by name, which a thread the loader attaches later
    // cannot.
    mcbe::overlay::attach(env);
}
#endif
