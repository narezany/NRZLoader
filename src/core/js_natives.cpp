#include "js_natives.h"

#include <dirent.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "loader.h"
#include "log.h"
#include "mcbe/events.h"
#include "overlay.h"
#include "screen_fx.h"
#include "v8_bridge.h"

namespace mcbe::js {
namespace {

// Fragments that identify each entry point uniquely. Full C++ names run to
// hundreds of characters, and the middle of one is enough to find it.
struct Entry {
    const char* key;
    const char* fragment;
    void** slot;
    bool required;
};

void* g_template_new = nullptr;
void* g_template_get_function = nullptr;
void* g_object_set = nullptr;
void* g_object_get = nullptr;
void* g_context_global = nullptr;
void* g_string_new = nullptr;
void* g_value_to_string = nullptr;
void* g_string_write_utf8 = nullptr;

std::atomic<uint64_t> g_calls{0};

const Entry kEntries[] = {
    {"FunctionTemplate::New", "16FunctionTemplate3NewE", &g_template_new, true},
    {"FunctionTemplate::GetFunction", "16FunctionTemplate11GetFunctionE",
     &g_template_get_function, true},
    {"Object::Set", "_ZN2v86Object3SetENS_5LocalINS_7ContextEEENS1_INS_5ValueEEES5_",
     &g_object_set, true},
    {"Context::Global", "_ZN2v87Context6GlobalEv", &g_context_global, true},
    {"String::NewFromUtf8", "_ZN2v86String11NewFromUtf8EPNS_7IsolateEPKcNS_13NewStringTypeEi",
     &g_string_new, true},

    // Only these three more are needed to carry arguments and results.
    {"Object::Get", "_ZN2v86Object3GetENS_5LocalINS_7ContextEEENS1_INS_5ValueEEE",
     &g_object_get, false},
    {"Value::ToString", "_ZNK2v85Value8ToStringENS_5LocalINS_7ContextEEE",
     &g_value_to_string, false},
    {"String::WriteUtf8", "6String9WriteUtf8E", &g_string_write_utf8, false},
};

constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

// Property names the shim and the native side agree on.
constexpr const char* kFunctionProperty = "__nrz_fn";
constexpr const char* kArgumentProperty = "__nrz_a";
constexpr const char* kSecondProperty = "__nrz_b";
constexpr const char* kResultProperty = "__nrz_out";

constexpr size_t kMaxTransferBytes = 4u * 1024 * 1024;

// ---------------------------------------------------------------------------
// Talking to the engine
// ---------------------------------------------------------------------------

void* make_string(void* isolate, const char* text) {
    auto create = reinterpret_cast<void* (*)(void*, const char*, int, int)>(g_string_new);
    return create(isolate, text, 0 /* NewStringType::kNormal */, -1);
}

void* global_of(void* context) {
    auto global = reinterpret_cast<void* (*)(void*)>(g_context_global);
    return global(context);
}

std::string to_utf8(void* isolate, void* context, void* value) {
    if (value == nullptr || g_value_to_string == nullptr || g_string_write_utf8 == nullptr) {
        return std::string();
    }

    auto to_string = reinterpret_cast<void* (*)(void*, void*)>(g_value_to_string);
    void* text = to_string(value, context);
    if (text == nullptr) return std::string();

    auto write = reinterpret_cast<int (*)(void*, void*, char*, int, int*, int)>(g_string_write_utf8);

    // Most values are short, so a modest buffer is tried first and only grown
    // when the result filled it.
    std::string result(4096, '\0');
    int written = write(text, isolate, &result[0], static_cast<int>(result.size()), nullptr, 2);
    if (written >= static_cast<int>(result.size()) - 1) {
        result.assign(kMaxTransferBytes, '\0');
        written = write(text, isolate, &result[0], static_cast<int>(result.size()), nullptr, 2);
    }
    if (written < 0) return std::string();

    result.resize(static_cast<size_t>(written));
    return result;
}

std::string read_global(void* isolate, void* context, const char* name) {
    if (g_object_get == nullptr) return std::string();

    void* object = global_of(context);
    void* key = make_string(isolate, name);
    if (object == nullptr || key == nullptr) return std::string();

    auto get = reinterpret_cast<void* (*)(void*, void*, void*)>(g_object_get);
    return to_utf8(isolate, context, get(object, context, key));
}

void write_global(void* isolate, void* context, const char* name, const std::string& value) {
    void* object = global_of(context);
    void* key = make_string(isolate, name);
    void* text = make_string(isolate, value.c_str());
    if (object == nullptr || key == nullptr || text == nullptr) return;

    auto set = reinterpret_cast<int (*)(void*, void*, void*, void*)>(g_object_set);
    set(object, context, key, text);
}

// ---------------------------------------------------------------------------
// What a mod may actually do
// ---------------------------------------------------------------------------

/**
 * Everything a mod touches stays under the loader's own folder.
 *
 * A mod is code the user chose to install, but that is no reason to hand it
 * the whole device by default.
 */
std::string resolve_path(const std::string& relative, bool& allowed) {
    allowed = false;
    if (relative.empty()) return std::string();
    if (relative.find("..") != std::string::npos) return std::string();

    const std::string root = Loader::instance().data_directory();
    const std::string full = relative[0] == '/' ? relative : root + "/" + relative;

    if (full.size() < root.size() || full.compare(0, root.size(), root) != 0) {
        return std::string();
    }

    allowed = true;
    return full;
}

std::string read_file(const std::string& path) {
    bool allowed = false;
    const std::string full = resolve_path(path, allowed);
    if (!allowed) return std::string();

    std::ifstream file(full, std::ios::binary);
    if (!file) return std::string();

    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string contents = buffer.str();
    if (contents.size() > kMaxTransferBytes) contents.resize(kMaxTransferBytes);
    return contents;
}

std::string write_file(const std::string& path, const std::string& contents) {
    bool allowed = false;
    const std::string full = resolve_path(path, allowed);
    if (!allowed) return "denied";

    // The directories leading up to it are created, so a mod can keep its own
    // files without ceremony.
    const size_t slash = full.find_last_of('/');
    if (slash != std::string::npos) {
        const std::string directory = full.substr(0, slash);
        for (size_t index = 1; index <= directory.size(); ++index) {
            if (index != directory.size() && directory[index] != '/') continue;
            mkdir(directory.substr(0, index).c_str(), 0775);
        }
    }

    std::ofstream file(full, std::ios::binary | std::ios::trunc);
    if (!file) return "failed";
    file << contents;
    return file.good() ? "ok" : "failed";
}

std::string list_directory(const std::string& path) {
    bool allowed = false;
    const std::string full = resolve_path(path.empty() ? "." : path, allowed);
    if (!allowed) return std::string();

    DIR* handle = opendir(full.c_str());
    if (handle == nullptr) return std::string();

    std::string result;
    while (dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        result += name;
        result += "\n";
    }
    closedir(handle);
    return result;
}

std::string delete_file(const std::string& path) {
    bool allowed = false;
    const std::string full = resolve_path(path, allowed);
    if (!allowed) return "denied";
    return remove(full.c_str()) == 0 ? "ok" : "failed";
}

/**
 * Reads the numbers at the front of a window request.
 *
 * The bridge carries two strings and nothing else, so a call needing six
 * values puts them on the first line and the document after it. Keeping the
 * split here rather than inventing a second argument channel means the bridge
 * stays as simple as it was.
 */
struct Geometry {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool touchable = false;
    std::string rest;
};

Geometry parse_geometry(const std::string& packed) {
    Geometry result;

    const size_t newline = packed.find('\n');
    const std::string head = packed.substr(0, newline);
    if (newline != std::string::npos) result.rest = packed.substr(newline + 1);

    int touchable = 0;
    sscanf(head.c_str(), "%d,%d,%d,%d,%d", &result.x, &result.y, &result.width, &result.height,
           &touchable);
    result.touchable = touchable != 0;
    return result;
}

/** Turns anything a shader log may contain into something json survives. */
std::string escape_json(const std::string& text) {
    std::string result;
    for (char character : text) {
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': break;
            case '\t': result += " "; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20) break;
                result += character;
        }
    }
    return result;
}

std::string describe_screen() {
    std::ostringstream out;
    out << "{\"names\":\"" << fx::names() << "\""
        << ",\"active\":\"" << fx::describe() << "\""
        << ",\"shader\":" << (fx::shader_active() ? "true" : "false")
        << ",\"error\":\"" << escape_json(fx::shader_error()) << "\""
        << ",\"drawing\":" << (fx::running() ? "true" : "false") << "}";
    return out.str();
}

std::string describe_windows() {
    const std::string trouble = overlay::trouble();

    std::ostringstream out;
    out << "{\"available\":" << (trouble.empty() ? "true" : "false")
        << ",\"reason\":\"" << trouble << "\"}";
    return out.str();
}

std::string describe() {
    const Loader& loader = Loader::instance();
    const v8bridge::Resolved& engine = v8bridge::resolved();

    std::ostringstream out;
    out << "{\"loader\":\"NRZLoader\""
        << ",\"dataDir\":\"" << loader.data_directory() << "\""
        << ",\"modsDir\":\"" << loader.mods_directory() << "\""
        << ",\"contexts\":" << engine.contexts_seen
        << ",\"calls\":" << g_calls.load()
        << ",\"arguments\":" << (arguments_ready() ? "true" : "false")
        << "}";
    return out.str();
}

std::string dispatch(const std::string& function, const std::string& first,
                     const std::string& second) {
    if (function == "log") {
        MCBE_LOGI("[js] %s", first.c_str());
        return "ok";
    }
    if (function == "readFile") return read_file(first);
    if (function == "writeFile") return write_file(first, second);
    if (function == "listDir") return list_directory(first);
    if (function == "deleteFile") return delete_file(first);
    if (function == "modsDir") return Loader::instance().mods_directory();
    if (function == "dataDir") return Loader::instance().data_directory();
    if (function == "info") return describe();

    // Effects laid over the finished frame.
    if (function == "fxSet") {
        fx::set(first);
        return fx::describe();
    }
    if (function == "fxInfo") return describe_screen();
    if (function == "fxShader") {
        if (first.empty()) {
            fx::clear_shader();
            return "cleared";
        }
        fx::set_shader(first);
        return "building";
    }
    if (function == "fxUniform") {
        float value = 0.0f;
        sscanf(second.c_str(), "%f", &value);
        fx::set_uniform(first, value);
        return "ok";
    }

    // Windows floating above the game.
    if (function == "windowOpen") {
        const Geometry where = parse_geometry(second);
        return overlay::open(first, where.rest, where.x, where.y, where.width, where.height,
                             where.touchable);
    }
    if (function == "windowMove") {
        const Geometry where = parse_geometry(second);
        return overlay::move(first, where.x, where.y, where.width, where.height);
    }
    if (function == "windowSetHtml") return overlay::set_html(first, second);
    if (function == "windowEval") return overlay::eval(first, second);
    if (function == "windowClose") return overlay::close(first);
    if (function == "windowCloseAll") return overlay::close_all();
    if (function == "windowPoll") return overlay::poll(first);
    if (function == "windowList") return overlay::list();
    if (function == "windowInfo") return describe_windows();

    MCBE_LOGW("[js] unknown native function: %s", function.c_str());
    return std::string();
}

/**
 * The single native function the engine sees.
 *
 * Its arguments and its result live on the global object, so nothing here
 * depends on how the engine lays out a callback.
 */
void native_call(void* /*callback_info*/) {
    const uint64_t total = g_calls.fetch_add(1) + 1;

    void* isolate = v8bridge::resolved().last_isolate;
    void* context = v8bridge::resolved().last_context;
    if (isolate == nullptr || context == nullptr) return;

    if (!arguments_ready()) {
        MCBE_LOGI("javascript called into native code (%llu so far)",
                  static_cast<unsigned long long>(total));
        Loader::instance().events().dispatch(MCBE_EVENT_NATIVE_CALL, nullptr);
        return;
    }

    const std::string function = read_global(isolate, context, kFunctionProperty);
    const std::string first = read_global(isolate, context, kArgumentProperty);
    const std::string second = read_global(isolate, context, kSecondProperty);

    write_global(isolate, context, kResultProperty, dispatch(function, first, second));
    Loader::instance().events().dispatch(MCBE_EVENT_NATIVE_CALL, nullptr);
}

// What mods actually program against. Everything it does is put two strings
// where native code can read them.
constexpr const char* kShim = R"JS(
(function () {
  if (typeof globalThis.__nrz_native !== 'function') return;
  var call = function (fn, a, b) {
    globalThis.__nrz_fn = String(fn);
    globalThis.__nrz_a = a === undefined || a === null ? '' : String(a);
    globalThis.__nrz_b = b === undefined || b === null ? '' : String(b);
    globalThis.__nrz_out = '';
    globalThis.__nrz_native();
    return globalThis.__nrz_out;
  };
  globalThis.nrz = {
    call: call,
    log: function (text) { return call('log', text); },
    readFile: function (path) { return call('readFile', path); },
    writeFile: function (path, text) { return call('writeFile', path, text); },
    deleteFile: function (path) { return call('deleteFile', path); },
    listDir: function (path) {
      var raw = call('listDir', path || '.');
      return raw ? raw.split('\n').filter(function (n) { return n.length; }) : [];
    },
    modsDir: function () { return call('modsDir'); },
    dataDir: function () { return call('dataDir'); },
    info: function () { try { return JSON.parse(call('info')); } catch (e) { return {}; } },
    readJson: function (path) {
      var text = call('readFile', path);
      if (!text) return null;
      try { return JSON.parse(text); } catch (e) { return null; }
    },
    writeJson: function (path, value) { return call('writeFile', path, JSON.stringify(value)); },
  };

  // Effects laid over the finished picture, as name-to-strength pairs where
  // the strength runs from 0 to 1.
  globalThis.nrz.fx = {
    set: function (effects) {
      var pairs = [];
      if (typeof effects === 'string') {
        pairs.push(effects);
      } else if (effects) {
        for (var name in effects) {
          if (Object.prototype.hasOwnProperty.call(effects, name)) {
            pairs.push(name + '=' + Number(effects[name]));
          }
        }
      }
      return call('fxSet', pairs.join(','));
    },
    clear: function () { return call('fxSet', ''); },
    // Says which effects are on, which names exist, whether a mod's own
    // shader is running, and whether any of it reaches the screen.
    info: function () { try { return JSON.parse(call('fxInfo')); } catch (e) { return {}; } },

    // A shader of the mod's own, which replaces the built-in effects.
    //
    // Write `vec3 effect(vec2 uv)` and return a colour; uTex, uRes, uTime and
    // vUv are declared already. Declare any other uniform yourself and set it
    // with uniform() below. Building happens on the drawing thread, so a
    // mistake in the shader shows up in info().error a frame later.
    shader: function (source) { return call('fxShader', source || ''); },
    noShader: function () { return call('fxShader', ''); },
    uniform: function (name, value) { return call('fxUniform', name, Number(value)); },
  };

  var pack = function (options) {
    options = options || {};
    return [
      options.x | 0,
      options.y | 0,
      options.width | 0,
      options.height | 0,
      options.touchable ? 1 : 0,
    ].join(',') + '\n' + (options.html === undefined ? '' : String(options.html));
  };

  // Windows floating above the game, each described in html and named so it
  // can be changed or closed later.
  globalThis.nrz.window = {
    open: function (id, options) { return call('windowOpen', id, pack(options)); },
    move: function (id, options) { return call('windowMove', id, pack(options)); },
    setHtml: function (id, html) { return call('windowSetHtml', id, html); },
    eval: function (id, script) { return call('windowEval', id, script); },
    close: function (id) { return call('windowClose', id); },
    closeAll: function () { return call('windowCloseAll'); },
    poll: function (id) {
      var raw = call('windowPoll', id);
      return raw ? raw.split('\n').filter(function (m) { return m.length; }) : [];
    },
    list: function () {
      var raw = call('windowList');
      return raw ? raw.split('\n').filter(function (m) { return m.length; }) : [];
    },
    info: function () { try { return JSON.parse(call('windowInfo')); } catch (e) { return {}; } },
  };
})();
)JS";

}  // namespace

const char* shim_source() { return kShim; }

size_t resolve(Loader& loader) {
    size_t found = 0;
    for (size_t index = 0; index < kEntryCount; ++index) {
        const Entry& entry = kEntries[index];
        void* address = loader.symbols().resolve_containing(entry.fragment);
        *entry.slot = address;

        if (address == nullptr) {
            MCBE_LOGW("js native %-30s missing%s", entry.key,
                      entry.required ? "" : " (arguments disabled)");
        } else {
            MCBE_LOGI("js native %-30s -> %p", entry.key, address);
            ++found;
        }
    }
    return found;
}

bool ready() {
    return g_template_new != nullptr && g_template_get_function != nullptr &&
           g_object_set != nullptr && g_context_global != nullptr && g_string_new != nullptr;
}

bool arguments_ready() {
    return ready() && g_object_get != nullptr && g_value_to_string != nullptr &&
           g_string_write_utf8 != nullptr;
}

uint64_t call_count() { return g_calls.load(); }

bool install(void* isolate, void* context) {
    if (!ready() || isolate == nullptr || context == nullptr) return false;

    // The full parameter list is passed even though older engines take fewer:
    // arguments a function does not declare are ignored, whereas passing too
    // few would leave it reading whatever is in the remaining registers.
    auto create_template = reinterpret_cast<void* (*)(void*, void (*)(void*), void*, void*, int,
                                                      int, int, void*, int, int, int)>(
        g_template_new);
    void* function_template = create_template(isolate, &native_call, nullptr, nullptr, 0,
                                              1 /* ConstructorBehavior::kAllow */,
                                              0 /* SideEffectType::kHasSideEffect */, nullptr, 0,
                                              0, 0);
    if (function_template == nullptr) {
        MCBE_LOGE("could not build a function template");
        return false;
    }

    auto get_function = reinterpret_cast<void* (*)(void*, void*)>(g_template_get_function);
    void* function = get_function(function_template, context);
    void* global = global_of(context);
    void* name = make_string(isolate, "__nrz_native");
    if (function == nullptr || global == nullptr || name == nullptr) {
        MCBE_LOGE("could not install the native entry point");
        return false;
    }

    auto set = reinterpret_cast<int (*)(void*, void*, void*, void*)>(g_object_set);
    set(global, context, name, function);

    MCBE_LOGI("native bridge installed (%s)",
              arguments_ready() ? "with arguments" : "call only");
    return true;
}

}  // namespace mcbe::js
