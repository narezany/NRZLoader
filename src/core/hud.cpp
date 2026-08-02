#include "hud.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "loader.h"
#include "mcbe/mod_api.h"
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

/**
 * Только то, что подтвердилось на записи однозначно.
 *
 * Первый состав был шире и оттого хуже. «Прыгает» стоял на методах, которые
 * срабатывают раз в пятнадцать-сорок секунд сами по себе, — это сохранение
 * мира, а не прыжок. «Использует» и «крутит камерой» горели почти всегда, а
 * показатель, который горит всегда, не показывает ничего.
 *
 * Осталось пять, каждый из которых на присланной записи встречался только при
 * своём действии и больше нигде.
 */
Signal g_signals[] = {
    {"break", "breaking", "111", true, {}},
    {"place", "placing", "103,112", true, {}},
    {"hit", "attacking", "144", true, {}},
    {"eat", "eating", "68,69", true, {}},
    {"swim", "swimming", "37", true, {}},
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

    // Отладочный экран Minecraft — это просто белые строки с чёрной тенью
    // поверх игры. Ни рамок, ни подложки: всё, что их изображает, выглядит
    // приклеенным поверх, а не частью игры.
    page << "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<style>"
            "*{margin:0;padding:0;-webkit-tap-highlight-color:transparent;user-select:none}"
            "body{font:400 13px/1.42 ui-monospace,'Roboto Mono',monospace;color:#fff;"
              "text-shadow:1px 1px 0 rgba(0,0,0,.85);white-space:nowrap;padding:2px 4px}"
            ".o{color:#7cf29a}"
            ".f{opacity:.42}"
            "</style><div id=b></div>"
            "<script>"
            "var N=[";

    for (size_t index = 0; index < kSignalCount; ++index) {
        if (index != 0) page << ",";
        page << "'" << g_signals[index].label << "'";
    }

    // Строка собирается целиком в javascript: перебирать элементы по одному
    // пять раз в секунду дороже, чем сложить строку и присвоить один раз.
    page << "];"
            "function u(d){"
              "var h='NRZLoader '+d.v+'<br>'+d.f+' fps, '+d.t+' tps, '+d.u+'<br><br>';"
              "for(var i=0;i<d.s.length;i++){"
                "var on=d.s[i][0];"
                "h+='<span class='+(on?'o':'f')+'>['+(on?'x':' ')+'] '+N[i]+"
                   "' '+d.s[i][1]+'</span><br>'}"
              "document.getElementById('b').innerHTML=h}"
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

    // Ширина не задаётся: строки короткие, а окно по содержимому не режет
    // хвосты — прежнее, заданное числом, обрезало их на любом экране.
    const std::string failure = overlay::open("nrz.hud", page_html(), 6, 44, 0, 0, false);
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
    data << "u({v:'" << MCBE_LOADER_VERSION << "',f:" << fps << ",t:" << tps << ",u:'"
         << static_cast<long>(alive) << "s',s:[";

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
