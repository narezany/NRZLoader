// Hooks the user interface and the renderer.
//
// Minecraft's interface is HTML rendered by Coherent Gameface, and neither
// that engine nor its renderer had their names stripped. They are therefore
// reachable by name on a retail build, unlike the game's own code, and they
// keep working on a server where the game logic is not ours to touch.

#include "ui_bridge.h"

#include <atomic>

#include "loader.h"
#include "log.h"
#include "mcbe/events.h"

namespace mcbe::ui {
namespace {

void* g_original_trigger_event = nullptr;
void* g_original_begin_frame = nullptr;

void* g_view = nullptr;  // a cohtml::ViewImpl the game handed us
void* g_trigger_event = nullptr;

std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_ui_events{0};

// void cohtml::ViewImpl::TriggerEvent(const char*)
//
// Hooked as much to capture the view as to observe events: an instance is
// needed to raise events ourselves later, and this is where one turns up.
void detour_trigger_event(void* view, const char* name) {
    if (g_view == nullptr && view != nullptr) {
        g_view = view;
        MCBE_LOGI("captured a user interface view: %p", view);
    }

    const uint64_t total = g_ui_events.fetch_add(1) + 1;
    if (total <= 5) {
        MCBE_LOGI("ui event: %s", name != nullptr ? name : "(unnamed)");
    }

    McbeUiEvent payload;
    payload.view = view;
    payload.name = name;
    Loader::instance().events().dispatch(MCBE_EVENT_UI, &payload);

    auto original = reinterpret_cast<void (*)(void*, const char*)>(g_original_trigger_event);
    if (original != nullptr) original(view, name);
}

// void renoir::LibraryImpl::BeginFrame(unsigned long, renoir::SceneImpl*)
void detour_begin_frame(void* library, unsigned long frame_id, void* scene) {
    const uint64_t total = g_frames.fetch_add(1) + 1;

    McbeFrameEvent payload;
    payload.frame = total;
    Loader::instance().events().dispatch(MCBE_EVENT_FRAME, &payload);

    // Roughly once a minute at sixty frames a second: enough to show the hook
    // is alive without filling the log.
    if (total % 3600 == 0) {
        MCBE_LOGI("rendered %llu frames", static_cast<unsigned long long>(total));
    }

    auto original = reinterpret_cast<void (*)(void*, unsigned long, void*)>(g_original_begin_frame);
    if (original != nullptr) original(library, frame_id, scene);
}

}  // namespace

uint64_t frame_count() { return g_frames.load(); }
uint64_t ui_event_count() { return g_ui_events.load(); }
void* view() { return g_view; }

bool trigger_event(const char* name) {
    if (g_view == nullptr || g_trigger_event == nullptr || name == nullptr) return false;
    auto trigger = reinterpret_cast<void (*)(void*, const char*)>(g_trigger_event);
    trigger(g_view, name);
    return true;
}

size_t install(Loader& loader) {
    size_t hooks = 0;

    g_trigger_event = loader.symbols().resolve("_ZN6cohtml8ViewImpl12TriggerEventEPKc");
    if (g_trigger_event != nullptr) {
        if (loader.install_hook(g_trigger_event, reinterpret_cast<void*>(&detour_trigger_event),
                                &g_original_trigger_event) == MCBE_OK) {
            MCBE_LOGI("ui  ViewImpl::TriggerEvent      -> %p", g_trigger_event);
            ++hooks;
        }
    } else {
        MCBE_LOGW("ui  ViewImpl::TriggerEvent      missing");
    }

    void* begin_frame = loader.symbols().resolve_containing("11LibraryImpl10BeginFrameE");
    if (begin_frame != nullptr) {
        if (loader.install_hook(begin_frame, reinterpret_cast<void*>(&detour_begin_frame),
                                &g_original_begin_frame) == MCBE_OK) {
            MCBE_LOGI("ui  LibraryImpl::BeginFrame    -> %p", begin_frame);
            ++hooks;
        }
    } else {
        MCBE_LOGW("ui  LibraryImpl::BeginFrame    missing");
    }

    return hooks;
}

}  // namespace mcbe::ui
