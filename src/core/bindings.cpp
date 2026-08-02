#include "bindings.h"

#include <atomic>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "loader.h"
#include "log.h"
#include "mcbe/events.h"

namespace mcbe::bindings {
namespace {

// Default symbol names for the 1.21.13x line. Treat them as a starting point,
// not as gospel: run tools/symgen.py against your own libminecraftpe.so and
// put the confirmed names in bindings.conf.
struct BindingSpec {
    const char* event;
    const char* default_symbol;
    const char* signature_note;
};

// Storage for the original function of each binding, indexed like g_specs.
void* g_originals[8] = {};
std::atomic<uint64_t> g_tick_count{0};

// --- Detours ---------------------------------------------------------------
//
// These are written as free functions taking the object pointer explicitly,
// which matches the AArch64 calling convention for a non-virtual member
// function: `this` arrives in x0 exactly like a first argument.

void detour_level_tick(void* level) {
    McbeTickEvent payload;
    payload.level = level;
    payload.tick_count = g_tick_count.fetch_add(1) + 1;
    Loader::instance().events().dispatch(MCBE_EVENT_LEVEL_TICK, &payload);

    auto original = reinterpret_cast<void (*)(void*)>(g_originals[0]);
    if (original != nullptr) original(level);
}

// bool Actor::hurt(ActorDamageSource const&, float, bool, bool)
bool detour_actor_hurt(void* actor, void* source, float amount, bool knock_back, bool ignite) {
    McbeDamageEvent payload;
    payload.actor = actor;
    payload.source = source;
    payload.amount = amount;

    const bool cancelled = Loader::instance().events().dispatch(MCBE_EVENT_ACTOR_HURT, &payload);
    if (cancelled) return false;

    auto original = reinterpret_cast<bool (*)(void*, void*, float, bool, bool)>(g_originals[1]);
    if (original == nullptr) return false;
    // A handler may have rewritten the amount.
    return original(actor, source, payload.amount, knock_back, ignite);
}

void detour_player_added(void* level, void* player) {
    McbePlayerEvent payload;
    payload.player = player;
    payload.level = level;
    Loader::instance().events().dispatch(MCBE_EVENT_PLAYER_JOIN, &payload);

    auto original = reinterpret_cast<void (*)(void*, void*)>(g_originals[2]);
    if (original != nullptr) original(level, player);
}

void detour_player_removed(void* level, void* player) {
    McbePlayerEvent payload;
    payload.player = player;
    payload.level = level;
    Loader::instance().events().dispatch(MCBE_EVENT_PLAYER_LEAVE, &payload);

    auto original = reinterpret_cast<void (*)(void*, void*)>(g_originals[3]);
    if (original != nullptr) original(level, player);
}

const BindingSpec g_specs[] = {
    {MCBE_EVENT_LEVEL_TICK, "_ZN5Level4tickEv", "void Level::tick()"},
    {MCBE_EVENT_ACTOR_HURT, "_ZN5Actor4hurtERK16ActorDamageSourcefbb",
     "bool Actor::hurt(ActorDamageSource const&, float, bool, bool)"},
    {MCBE_EVENT_PLAYER_JOIN, "_ZN5Level9addPlayerEP6Player", "void Level::addPlayer(Player*)"},
    {MCBE_EVENT_PLAYER_LEAVE, "_ZN5Level12removePlayerEP6Player", "void Level::removePlayer(Player*)"},
};

void* const g_detours[] = {
    reinterpret_cast<void*>(&detour_level_tick),
    reinterpret_cast<void*>(&detour_actor_hurt),
    reinterpret_cast<void*>(&detour_player_added),
    reinterpret_cast<void*>(&detour_player_removed),
};

constexpr size_t kBindingCount = sizeof(g_specs) / sizeof(g_specs[0]);

// Reads `event = symbol` lines. Unknown events are reported and ignored.
std::unordered_map<std::string, std::string> read_overrides(const std::string& path) {
    std::unordered_map<std::string, std::string> overrides;
    std::ifstream file(path);
    if (!file) return overrides;

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

        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (!key.empty() && !value.empty()) overrides[key] = value;
    }
    MCBE_LOGI("bindings.conf: %zu overrides from %s", overrides.size(), path.c_str());
    return overrides;
}

}  // namespace

size_t binding_count() { return kBindingCount; }

size_t install_all(Loader& loader, const std::string& config_path) {
    const auto overrides = read_overrides(config_path);
    size_t active = 0;

    for (size_t index = 0; index < kBindingCount; ++index) {
        const BindingSpec& spec = g_specs[index];

        std::string symbol = spec.default_symbol;
        const auto override_entry = overrides.find(spec.event);
        if (override_entry != overrides.end()) symbol = override_entry->second;

        void* target = nullptr;
        if (symbol.rfind("sig:", 0) == 0) {
            target = loader.symbols().scan_signature(symbol.substr(4));
        } else {
            target = loader.symbols().resolve(symbol);
        }

        if (target == nullptr) {
            MCBE_LOGW("binding %-14s inactive: %s not found (%s)", spec.event, symbol.c_str(),
                      spec.signature_note);
            continue;
        }

        const McbeResult result = loader.install_hook(target, g_detours[index], &g_originals[index]);
        if (result != MCBE_OK) {
            MCBE_LOGE("binding %-14s failed to hook %p", spec.event, target);
            continue;
        }

        MCBE_LOGI("binding %-14s -> %p", spec.event, target);
        ++active;
    }
    return active;
}

}  // namespace mcbe::bindings
