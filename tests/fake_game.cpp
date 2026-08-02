// A stand-in for libminecraftpe.so.
//
// Real C++ classes so the compiler produces genuinely mangled names, plus one
// deliberately local function that dlsym cannot see but the ELF symbol table
// still lists.

#include <cstdint>

struct Level {
    int counter = 0;
    void tick();
    int addPlayer(int player_id);
};

void Level::tick() { counter += 1; }

int Level::addPlayer(int player_id) { return player_id + counter; }

struct Actor {
    float health = 20.0f;
    bool hurt(const void* source, float amount, bool knock_back, bool ignite);
};

bool Actor::hurt(const void*, float amount, bool, bool) {
    health -= amount;
    return health > 0.0f;
}

// Local: absent from .dynsym, present in .symtab.
__attribute__((noinline, used)) static int internal_compute(int value) { return value + 77; }

extern "C" int fake_game_call_internal(int value) { return internal_compute(value); }

extern "C" Level* fake_game_new_level() {
    static Level level;
    return &level;
}
