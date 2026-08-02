#include "overlay.h"

#include "jni_bridge.h"
#include "log.h"
#include "screen_fx.h"
#include "vtable_probe.h"

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace mcbe::overlay {
namespace {

#if defined(__ANDROID__)

constexpr const char* kClassName = "ru/narezany/nrzloader/Overlay";

jclass g_class = nullptr;  // global reference
jmethodID g_allowed = nullptr;
jmethodID g_open = nullptr;
jmethodID g_close = nullptr;
jmethodID g_close_all = nullptr;
jmethodID g_set_html = nullptr;
jmethodID g_eval = nullptr;
jmethodID g_move = nullptr;
jmethodID g_poll = nullptr;
jmethodID g_list = nullptr;

/**
 * A JNIEnv for whichever thread asked, attaching it when it is not a java
 * thread already, and detaching only what it attached.
 */
class Scoped {
public:
    Scoped() {
        auto* vm = static_cast<JavaVM*>(jni::java_vm());
        if (vm == nullptr) return;

        if (vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) == JNI_OK) return;
        if (vm->AttachCurrentThread(&env_, nullptr) == JNI_OK) {
            vm_ = vm;
        } else {
            env_ = nullptr;
        }
    }

    ~Scoped() {
        if (env_ != nullptr && env_->ExceptionCheck() == JNI_TRUE) env_->ExceptionClear();
        if (vm_ != nullptr) vm_->DetachCurrentThread();
    }

    Scoped(const Scoped&) = delete;
    Scoped& operator=(const Scoped&) = delete;

    JNIEnv* env() const { return env_; }
    bool ok() const { return env_ != nullptr && g_class != nullptr; }

private:
    JNIEnv* env_ = nullptr;
    JavaVM* vm_ = nullptr;
};

/** Reports and clears a pending exception so the next call starts clean. */
bool failed(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck() == JNI_FALSE) return false;
    env->ExceptionClear();
    MCBE_LOGW("overlay: %s failed", what);
    return true;
}

std::string take_string(JNIEnv* env, jstring text) {
    if (text == nullptr) return std::string();

    const char* chars = env->GetStringUTFChars(text, nullptr);
    std::string result = chars == nullptr ? std::string() : chars;
    if (chars != nullptr) env->ReleaseStringUTFChars(text, chars);
    env->DeleteLocalRef(text);
    return result;
}

/** A local reference that is released however the call ends. */
class LocalString {
public:
    LocalString(JNIEnv* env, const std::string& text) : env_(env), value_(env->NewStringUTF(text.c_str())) {}

    ~LocalString() {
        if (value_ != nullptr) env_->DeleteLocalRef(value_);
    }

    LocalString(const LocalString&) = delete;
    LocalString& operator=(const LocalString&) = delete;

    jstring get() const { return value_; }

private:
    JNIEnv* env_;
    jstring value_;
};

#endif  // __ANDROID__

}  // namespace

#if defined(__ANDROID__)

void attach(void* raw_env) {
    auto* env = static_cast<JNIEnv*>(raw_env);
    if (env == nullptr || g_class != nullptr) return;

    jclass found = env->FindClass(kClassName);
    if (found == nullptr || failed(env, "finding the window class")) {
        MCBE_LOGW("no window class in this package; floating windows are off");
        return;
    }

    g_class = static_cast<jclass>(env->NewGlobalRef(found));
    env->DeleteLocalRef(found);

    g_allowed = env->GetStaticMethodID(g_class, "allowed", "()Z");
    g_open = env->GetStaticMethodID(
        g_class, "open", "(Ljava/lang/String;Ljava/lang/String;IIIIZ)Ljava/lang/String;");
    g_set_html = env->GetStaticMethodID(
        g_class, "setHtml", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    g_close = env->GetStaticMethodID(g_class, "close", "(Ljava/lang/String;)Ljava/lang/String;");
    g_close_all = env->GetStaticMethodID(g_class, "closeAll", "()Ljava/lang/String;");
    g_eval = env->GetStaticMethodID(g_class, "eval",
                                    "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    g_move = env->GetStaticMethodID(g_class, "move", "(Ljava/lang/String;IIII)Ljava/lang/String;");
    g_poll = env->GetStaticMethodID(g_class, "poll", "(Ljava/lang/String;)Ljava/lang/String;");
    g_list = env->GetStaticMethodID(g_class, "list", "()Ljava/lang/String;");

    if (failed(env, "looking up the window methods") || g_open == nullptr) {
        MCBE_LOGW("the window class is not the one this loader expects");
        env->DeleteGlobalRef(g_class);
        g_class = nullptr;
        return;
    }

    // The class needs the context to reach the window manager, and this is the
    // one moment the loader is certain to have one.
    jmethodID attach_method =
        env->GetStaticMethodID(g_class, "attach", "(Landroid/content/Context;)V");
    if (attach_method != nullptr) {
        env->CallStaticVoidMethod(g_class, attach_method,
                                  static_cast<jobject>(jni::app_context()));
        failed(env, "handing the context to the window class");
    }

    MCBE_LOGI("floating windows ready (%s)",
              available() ? "allowed" : "waiting for permission");
}

bool available() {
    Scoped scoped;
    if (!scoped.ok() || g_allowed == nullptr) return false;

    const jboolean allowed = scoped.env()->CallStaticBooleanMethod(g_class, g_allowed);
    if (failed(scoped.env(), "checking the window permission")) return false;
    return allowed == JNI_TRUE;
}

std::string trouble() {
    if (g_class == nullptr) return "this build of the game has no window support";
    if (!available()) return "the game is not allowed to draw over other apps";
    return std::string();
}

std::string open(const std::string& id, const std::string& html, int x, int y, int width,
                 int height, bool touchable) {
    Scoped scoped;
    if (!scoped.ok()) return "no java side";

    JNIEnv* env = scoped.env();
    LocalString name(env, id);
    LocalString document(env, html);

    auto result = static_cast<jstring>(env->CallStaticObjectMethod(
        g_class, g_open, name.get(), document.get(), x, y, width, height,
        touchable ? JNI_TRUE : JNI_FALSE));
    if (failed(env, "opening a window")) return "failed";
    return take_string(env, result);
}

std::string set_html(const std::string& id, const std::string& html) {
    Scoped scoped;
    if (!scoped.ok() || g_set_html == nullptr) return "no java side";

    JNIEnv* env = scoped.env();
    LocalString name(env, id);
    LocalString document(env, html);
    auto result = static_cast<jstring>(
        env->CallStaticObjectMethod(g_class, g_set_html, name.get(), document.get()));
    if (failed(env, "replacing a window's contents")) return "failed";
    return take_string(env, result);
}

std::string close(const std::string& id) {
    Scoped scoped;
    if (!scoped.ok() || g_close == nullptr) return "no java side";

    JNIEnv* env = scoped.env();
    LocalString name(env, id);
    auto result = static_cast<jstring>(env->CallStaticObjectMethod(g_class, g_close, name.get()));
    if (failed(env, "closing a window")) return "failed";
    return take_string(env, result);
}

std::string close_all() {
    Scoped scoped;
    if (!scoped.ok() || g_close_all == nullptr) return "no java side";

    JNIEnv* env = scoped.env();
    auto result = static_cast<jstring>(env->CallStaticObjectMethod(g_class, g_close_all));
    if (failed(env, "closing every window")) return "failed";
    return take_string(env, result);
}

std::string eval(const std::string& id, const std::string& script) {
    Scoped scoped;
    if (!scoped.ok() || g_eval == nullptr) return "no java side";

    JNIEnv* env = scoped.env();
    LocalString name(env, id);
    LocalString code(env, script);
    auto result = static_cast<jstring>(
        env->CallStaticObjectMethod(g_class, g_eval, name.get(), code.get()));
    if (failed(env, "running script in a window")) return "failed";
    return take_string(env, result);
}

std::string move(const std::string& id, int x, int y, int width, int height) {
    Scoped scoped;
    if (!scoped.ok() || g_move == nullptr) return "no java side";

    JNIEnv* env = scoped.env();
    LocalString name(env, id);
    auto result = static_cast<jstring>(
        env->CallStaticObjectMethod(g_class, g_move, name.get(), x, y, width, height));
    if (failed(env, "moving a window")) return "failed";
    return take_string(env, result);
}

std::string poll(const std::string& id) {
    Scoped scoped;
    if (!scoped.ok() || g_poll == nullptr) return std::string();

    JNIEnv* env = scoped.env();
    LocalString name(env, id);
    auto result = static_cast<jstring>(env->CallStaticObjectMethod(g_class, g_poll, name.get()));
    if (failed(env, "collecting window messages")) return std::string();
    return take_string(env, result);
}

std::string list() {
    Scoped scoped;
    if (!scoped.ok() || g_list == nullptr) return std::string();

    JNIEnv* env = scoped.env();
    auto result = static_cast<jstring>(env->CallStaticObjectMethod(g_class, g_list));
    if (failed(env, "listing windows")) return std::string();
    return take_string(env, result);
}

#else

void attach(void*) {}
bool available() { return false; }
std::string trouble() { return "windows need Android"; }
std::string open(const std::string&, const std::string&, int, int, int, int, bool) {
    return "unsupported";
}
std::string set_html(const std::string&, const std::string&) { return "unsupported"; }
std::string close(const std::string&) { return "unsupported"; }
std::string close_all() { return "unsupported"; }
std::string eval(const std::string&, const std::string&) { return "unsupported"; }
std::string move(const std::string&, int, int, int, int) { return "unsupported"; }
std::string poll(const std::string&) { return std::string(); }
std::string list() { return std::string(); }

#endif

}  // namespace mcbe::overlay

#if defined(__ANDROID__)
/**
 * A command sent by a page inside one of the windows.
 *
 * Nothing here goes near the mod: a page that wants to talk to its mod sends
 * an ordinary message and the mod collects it. This is for the things a page
 * can usefully do by itself, so that a window of buttons keeps working even in
 * an engine that gives a mod no way to run code on a timer.
 *
 *   mark <text>  put a labelled marker in the probe's timeline
 *   fx <spec>    turn screen effects on, as "crt=0.6,glitch=0.2"
 *   close        close the window the command came from
 *   log <text>   write a line to the loader's log
 */
extern "C" JNIEXPORT void JNICALL Java_ru_narezany_nrzloader_Overlay_nativeCommand(
    JNIEnv* env, jclass, jstring id, jstring command) {
    const char* id_chars = id == nullptr ? nullptr : env->GetStringUTFChars(id, nullptr);
    const char* command_chars =
        command == nullptr ? nullptr : env->GetStringUTFChars(command, nullptr);

    const std::string window = id_chars == nullptr ? std::string() : id_chars;
    const std::string text = command_chars == nullptr ? std::string() : command_chars;

    if (id_chars != nullptr) env->ReleaseStringUTFChars(id, id_chars);
    if (command_chars != nullptr) env->ReleaseStringUTFChars(command, command_chars);

    const size_t space = text.find(' ');
    const std::string verb = text.substr(0, space);
    const std::string rest = space == std::string::npos ? std::string() : text.substr(space + 1);

    if (verb == "mark") {
        mcbe::probe::mark(rest);
    } else if (verb == "fx") {
        mcbe::fx::set(rest);
    } else if (verb == "close") {
        mcbe::overlay::close(window);
    } else if (verb == "log") {
        MCBE_LOGI("[window %s] %s", window.c_str(), rest.c_str());
    } else {
        MCBE_LOGW("[window %s] unknown command: %s", window.c_str(), verb.c_str());
    }
}
#endif
