#include "hud.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "loader.h"
#include "log.h"
#include "overlay.h"
#include "vtable_probe.h"

namespace mcbe::hud {
namespace {

/**
 * Одна строка окошка.
 *
 * `light` — то, что либо происходит прямо сейчас, либо нет: прыжок, плавание.
 * Остальное — счётчики за всю сессию.
 */
struct Signal {
    const char* key;      // ключ в настройках
    const char* label;    // что видит человек
    const char* fallback; // номера слотов по умолчанию
    bool light;
    std::vector<size_t> slots;
    uint64_t previous = 0;
    uint64_t total = 0;
    bool active = false;
};

// Значения по умолчанию — то, что нашлось разведкой на 1.26.23.1. На другой
// сборке номера будут другими, поэтому их можно переопределить в настройках.
Signal g_signals[] = {
    {"break", "ломает", "111", true, {}},
    {"place", "ставит", "103,112", true, {}},
    {"hit", "бьёт", "144", true, {}},
    {"eat", "ест", "68,69", true, {}},
    {"jump", "прыгает", "62,63", true, {}},
    {"swim", "плывёт", "37", true, {}},
    {"use", "использует", "149", true, {}},
    {"look", "крутит камерой", "114,187", true, {}},
};

constexpr size_t kSignalCount = sizeof(g_signals) / sizeof(g_signals[0]);

// Эти два не сигналы, а скорости: по ним видно, жива ли игра вообще.
std::vector<size_t> g_frame_slots;
std::vector<size_t> g_tick_slots;
uint64_t g_frame_previous = 0;
uint64_t g_tick_previous = 0;

bool g_running = false;
std::chrono::steady_clock::time_point g_started;
std::chrono::steady_clock::time_point g_last;

std::string trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string setting(const std::string& path, const std::string& key,
                    const std::string& fallback) {
    std::ifstream file(path);
    if (!file) return fallback;

    std::string line;
    while (std::getline(file, line)) {
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        if (trim(line.substr(0, equals)) == key) {
            const std::string value = trim(line.substr(equals + 1));
            if (!value.empty()) return value;
        }
    }
    return fallback;
}

std::vector<size_t> parse_slots(const std::string& list) {
    std::vector<size_t> slots;
    std::istringstream reader(list);
    std::string piece;

    while (std::getline(reader, piece, ',')) {
        const std::string text = trim(piece);
        if (text.empty()) continue;
        slots.push_back(static_cast<size_t>(strtoul(text.c_str(), nullptr, 10)));
    }
    return slots;
}

uint64_t sum(const std::vector<size_t>& slots) {
    uint64_t total = 0;
    for (size_t slot : slots) total += probe::counter(slot);
    return total;
}

/**
 * Страница окошка.
 *
 * Числа в ней потом меняются по одному, а не переписывается всё целиком:
 * перерисовывать документ пять раз в секунду поверх игры — заметная работа.
 */
std::string page_html() {
    std::ostringstream page;
    page << "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<style>"
            "*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;user-select:none}"
            "body{margin:0;font:600 11px/1.35 ui-monospace,Menlo,monospace;color:#dfe6ee}"
            ".w{background:rgba(10,12,16,.62);backdrop-filter:blur(10px);border-radius:10px;"
              "border:1px solid rgba(255,255,255,.07);padding:7px 9px;min-width:150px}"
            ".r{display:flex;justify-content:space-between;gap:10px;padding:1px 0}"
            ".k{opacity:.5}"
            ".v{font-variant-numeric:tabular-nums}"
            ".on{color:#6ee7a0}"
            ".off{opacity:.28}"
            ".s{height:1px;background:rgba(255,255,255,.08);margin:5px -9px}"
            "</style><div class=w id=w>";

    page << "<div class=r><span class=k>кадров/с</span><span class=v id=fps>—</span></div>"
            "<div class=r><span class=k>тактов/с</span><span class=v id=tps>—</span></div>"
            "<div class=r><span class=k>в мире</span><span class=v id=up>—</span></div>"
            "<div class=s></div>";

    for (size_t index = 0; index < kSignalCount; ++index) {
        page << "<div class=r id=r" << index << "><span class=k>" << g_signals[index].label
             << "</span><span class=v id=c" << index << ">0</span></div>";
    }

    page << "</div><script>"
            "function u(d){"
              "document.getElementById('fps').textContent=d.f;"
              "document.getElementById('tps').textContent=d.t;"
              "document.getElementById('up').textContent=d.u;"
              "for(var i=0;i<d.s.length;i++){"
                "document.getElementById('r'+i).className='r '+(d.s[i][0]?'on':'off');"
                "document.getElementById('c'+i).textContent=d.s[i][1]}}"
            "</script>";
    return page.str();
}

}  // namespace

bool running() { return g_running; }

bool install(Loader& loader, const std::string& config_path) {
    if (setting(config_path, "hud", "on") == "off") return false;
    if (!probe::running()) {
        MCBE_LOGI("окошко не открыть: разведка выключена, а числа берутся из неё");
        return false;
    }

    for (Signal& signal : g_signals) {
        signal.slots =
            parse_slots(setting(config_path, std::string("hud.") + signal.key, signal.fallback));
    }
    g_frame_slots = parse_slots(setting(config_path, "hud.frame", "35"));
    g_tick_slots = parse_slots(setting(config_path, "hud.tick", "25"));

    const std::string trouble = overlay::trouble();
    if (!trouble.empty()) {
        MCBE_LOGW("окошко не открыть: %s", trouble.c_str());
        return false;
    }

    const std::string failure = overlay::open("nrz.hud", page_html(), 8, 190, 190, 0, false);
    if (!failure.empty()) {
        MCBE_LOGW("окошко не открылось: %s", failure.c_str());
        return false;
    }

    g_started = std::chrono::steady_clock::now();
    g_last = g_started;
    g_running = true;

    // Тапы сквозь него проходят в игру: это показометр, а не пульт.
    MCBE_LOGI("окошко открыто, %zu показателей", kSignalCount);
    (void)loader;
    return true;
}

void refresh() {
    if (!g_running) return;

    const auto now = std::chrono::steady_clock::now();
    const double since = std::chrono::duration<double>(now - g_last).count();
    if (since < 0.15) return;
    g_last = now;

    const double alive = std::chrono::duration<double>(now - g_started).count();

    const uint64_t frames = sum(g_frame_slots);
    const uint64_t ticks = sum(g_tick_slots);
    const long fps = static_cast<long>((frames - g_frame_previous) / since + 0.5);
    const long tps = static_cast<long>((ticks - g_tick_previous) / since + 0.5);
    g_frame_previous = frames;
    g_tick_previous = ticks;

    std::ostringstream data;
    data << "u({f:" << fps << ",t:" << tps << ",u:'" << static_cast<long>(alive) << "с',s:[";

    for (size_t index = 0; index < kSignalCount; ++index) {
        Signal& signal = g_signals[index];
        const uint64_t total = sum(signal.slots);

        // «Прямо сейчас» — это «за последнюю пятую долю секунды хоть раз».
        // Мигание на границе видно куда хуже, чем задержка в десятую долю.
        signal.active = total > signal.previous;
        signal.previous = total;
        signal.total = total;

        if (index != 0) data << ",";
        data << "[" << (signal.active ? 1 : 0) << ",'" << total << "']";
    }
    data << "]})";

    overlay::eval("nrz.hud", data.str());
}

}  // namespace mcbe::hud
