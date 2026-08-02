// A mod that shows the whole surface of the API: events, logging, and
// resolving a game function directly.

#include <cstdio>

#include "mcbe/events.h"
#include "mcbe/mod_api.h"

namespace {

const McbeApi* g_api = nullptr;
uint64_t g_ticks = 0;

void log_line(McbeLogLevel level, const char* text) {
    if (g_api != nullptr) g_api->log(level, text);
}

void on_ready(McbeEvent*, void*) { log_line(MCBE_LOG_INFO, "hello_mod: the game is up"); }

void on_tick(McbeEvent* event, void*) {
    const auto* tick = static_cast<McbeTickEvent*>(event->data);
    g_ticks = tick->tick_count;

    // 20 ticks per second, so this is one line per minute.
    if (g_ticks % 1200 == 0) {
        char message[96];
        snprintf(message, sizeof(message), "hello_mod: %llu ticks so far",
                 static_cast<unsigned long long>(g_ticks));
        log_line(MCBE_LOG_INFO, message);
    }
}

void on_hurt(McbeEvent* event, void*) {
    auto* damage = static_cast<McbeDamageEvent*>(event->data);

    char message[128];
    snprintf(message, sizeof(message), "hello_mod: actor %p about to take %.1f damage",
             damage->actor, static_cast<double>(damage->amount));
    log_line(MCBE_LOG_DEBUG, message);

    // Halving the damage shows that handlers can rewrite the payload.
    // Setting event->cancelled = 1 instead would block the hit entirely.
    damage->amount *= 0.5f;
}

void on_script_context(McbeEvent* event, void*) {
    const auto* script = static_cast<McbeScriptContextEvent*>(event->data);

    char message[160];
    snprintf(message, sizeof(message), "hello_mod: js context #%u (%s), isolate %p",
             script->sequence, script->is_game_script ? "game script" : "user interface",
             script->isolate);
    log_line(MCBE_LOG_INFO, message);

    // The engine entry points live here. On a retail build this is the only
    // foothold that survives, so a real mod would compile and run its own
    // JavaScript through these rather than hooking the game directly.
    const McbeV8* v8 = g_api->v8();
    if (v8->script_compile == nullptr || v8->string_new_utf8 == nullptr) {
        log_line(MCBE_LOG_WARN, "hello_mod: cannot run scripts, engine entry points missing");
    }
}

const McbeModInfo g_info = {
    MCBE_ABI_VERSION,
    "example.hello",
    "Hello Mod",
    "1.0.0",
    "1.26.23.1",
};

}  // namespace

extern "C" const McbeModInfo* mcbe_mod_info(void) { return &g_info; }

extern "C" McbeResult mcbe_mod_init(const McbeApi* api) {
    if (api == nullptr || api->abi_version != MCBE_ABI_VERSION) return MCBE_ERR_ABI_MISMATCH;
    g_api = api;

    api->subscribe(MCBE_EVENT_READY, on_ready, nullptr);
    api->subscribe(MCBE_EVENT_LEVEL_TICK, on_tick, nullptr);
    api->subscribe(MCBE_EVENT_ACTOR_HURT, on_hurt, nullptr);
    api->subscribe(MCBE_EVENT_SCRIPT_CONTEXT, on_script_context, nullptr);

    char message[160];
    snprintf(message, sizeof(message), "hello_mod: loaded against game %s, data in %s",
             api->game_version(), api->data_directory());
    log_line(MCBE_LOG_INFO, message);

    return MCBE_OK;
}

extern "C" void mcbe_mod_shutdown(void) { log_line(MCBE_LOG_INFO, "hello_mod: shutting down"); }
