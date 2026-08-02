// Payload layouts for the events the loader raises.
//
// Handles into the game are opaque pointers on purpose. Field offsets inside
// Minecraft's own classes change between builds and are not part of this ABI;
// a mod that wants them must resolve accessors itself.
#ifndef MCBE_EVENTS_H
#define MCBE_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Raised once per game tick. data: McbeTickEvent*
#define MCBE_EVENT_LEVEL_TICK "level.tick"

// Raised when a player joins the level. data: McbePlayerEvent*
#define MCBE_EVENT_PLAYER_JOIN "player.join"

// Raised when a player leaves. data: McbePlayerEvent*
#define MCBE_EVENT_PLAYER_LEAVE "player.leave"

// Raised before a chat message is sent. Cancellable. data: McbeChatEvent*
#define MCBE_EVENT_CHAT "player.chat"

// Raised when an actor takes damage. Cancellable. data: McbeDamageEvent*
#define MCBE_EVENT_ACTOR_HURT "actor.hurt"

// Raised after the loader finishes wiring every binding it could resolve.
// data: NULL. Useful as a "the game is fully up" signal.
#define MCBE_EVENT_READY "loader.ready"

// A JavaScript context was created inside the game. data: McbeScriptContextEvent*
//
// Retail builds strip the names of the game's own C++ code, so this is the
// practical way in: the scripting engine keeps its public interface exported,
// and Minecraft's own script API runs on top of it.
#define MCBE_EVENT_SCRIPT_CONTEXT "script.context"

// A script is about to be executed. data: McbeScriptRunEvent*
#define MCBE_EVENT_SCRIPT_RUN "script.run"

// JavaScript called __mcbe_ping(). data: NULL
#define MCBE_EVENT_NATIVE_CALL "script.native_call"

// The interface raised an event. data: McbeUiEvent*
#define MCBE_EVENT_UI "ui.event"

// A frame is about to be drawn. data: McbeFrameEvent*
// Raised on the render thread, sixty times a second or so: keep handlers
// short and do not block.
#define MCBE_EVENT_FRAME "render.frame"

typedef struct McbeTickEvent {
    void* level;        // Level*
    uint64_t tick_count;  // counted by the loader, not the game
} McbeTickEvent;

typedef struct McbePlayerEvent {
    void* player;  // Player*
    void* level;   // Level*, may be NULL
} McbePlayerEvent;

typedef struct McbeChatEvent {
    void* player;
    const char* message;  // valid only for the duration of the callback
} McbeChatEvent;

typedef struct McbeDamageEvent {
    void* actor;
    void* source;   // ActorDamageSource*
    float amount;   // writable: change it to alter the damage dealt
} McbeDamageEvent;

// A context belongs either to the game's script engine or to the HTML based
// user interface, which embeds the same engine. `is_game_script` is the
// loader's best guess; check it before assuming which one you have.
typedef struct McbeScriptContextEvent {
    void* isolate;          // v8::Isolate*
    void* context;          // v8::Local<v8::Context> as an opaque value
    int is_game_script;
    uint32_t sequence;      // 1 for the first context seen, then upwards
} McbeScriptContextEvent;

typedef struct McbeUiEvent {
    void* view;        // cohtml::ViewImpl*
    const char* name;  // event name, valid only during the callback
} McbeUiEvent;

typedef struct McbeFrameEvent {
    uint64_t frame;
} McbeFrameEvent;

typedef struct McbeScriptRunEvent {
    void* script;   // v8::Script*
    void* context;  // v8::Local<v8::Context>
} McbeScriptRunEvent;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MCBE_EVENTS_H
