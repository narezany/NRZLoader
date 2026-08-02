#include "vtable_probe.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "loader.h"
#include "log.h"
#include "module.h"
#include "hud.h"
#include "overlay.h"

extern "C" {
/**
 * Счётчик и настоящий адрес — парой, потому что подставка вычисляет адрес
 * этой пары один раз и берёт из неё и то, и другое.
 */
struct NrzProbeSlot {
    uint64_t count;
    void* original;
};

NrzProbeSlot nrz_probe_slots[256] = {};

#if defined(__aarch64__)
extern const unsigned char nrz_probe_stubs[];
#endif
}

namespace mcbe::probe {
namespace {

constexpr size_t kMaxSlots = 256;
constexpr size_t kStubSize = 32;

std::atomic<bool> g_running{false};
std::string g_class;
size_t g_slots = 0;
void** g_vtable = nullptr;
std::string g_report_path;
std::string g_timeline_path;
uint64_t g_previous[kMaxSlots] = {};
uint64_t g_timeline_previous[kMaxSlots] = {};
std::chrono::steady_clock::time_point g_started;
std::chrono::steady_clock::time_point g_last_report;
bool g_want_panel = false;
std::string g_config_path;
int g_windows = 0;

/**
 * Такт игры — тот, чья скорость не гуляет.
 *
 * Первая попытка искала основу так: чьё число делит остальные нацело. На
 * живой игре это выбрало кадры, а не такты, — кадров больше, и делится на них
 * тоже многое. Разница же в другом: такт игра держит ровно двадцать раз в
 * секунду, чего бы ей это ни стоило, а кадры проседают на каждом чихе.
 *
 * Поэтому основа ищется по постоянству: за одинаковые промежутки времени у
 * такта одно и то же число, у кадров — каждый раз разное.
 */
struct Rate {
    uint64_t last = 0;
    uint64_t smallest = ~0ull;
    uint64_t largest = 0;
    int windows = 0;
    int active = 0;

    void observe(uint64_t total) {
        const uint64_t delta = total - last;
        last = total;
        if (delta == 0) return;

        smallest = std::min(smallest, delta);
        largest = std::max(largest, delta);
        ++windows;
        ++active;
    }

    /** Насколько скорость гуляет, в долях. У такта почти ноль. */
    double wobble() const {
        if (windows < 3 || largest == 0) return 1.0;
        return static_cast<double>(largest - smallest) / largest;
    }

    /**
     * Такт идёт непрерывно, поэтому попадает в каждое окно.
     *
     * Без этой проверки за такт принималось что попало: метод, срабатывающий
     * пачками ровно по пять, тоже держит скорость ровно — но только в тех
     * окнах, где он вообще был.
     */
    bool constant(int total_windows) const {
        return total_windows >= 5 && active * 10 >= total_windows * 8;
    }
};

Rate g_rates[kMaxSlots];

uint64_t find_base(const std::vector<std::pair<uint64_t, size_t>>& hits) {
    uint64_t best = 0;
    double best_wobble = 0.08;  // больше восьми процентов — это уже не такт

    for (const auto& hit : hits) {
        if (hit.first < 100) continue;

        if (!g_rates[hit.second].constant(g_windows)) continue;

        const double wobble = g_rates[hit.second].wobble();
        if (wobble > best_wobble) continue;

        // Из ровных берётся самый редкий: такт один, а кратные ему идут
        // вдвое и втрое чаще и так же ровно.
        if (best == 0 || hit.first < best) {
            best = hit.first;
            best_wobble = wobble;
        }
    }
    return best;
}

std::string trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

/** Читает одну настройку из файла вида `ключ = значение`. */
std::string setting(const std::string& path, const std::string& key) {
    std::ifstream file(path);
    if (!file) return std::string();

    std::string line;
    while (std::getline(file, line)) {
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        if (trim(line.substr(0, equals)) == key) return trim(line.substr(equals + 1));
    }
    return std::string();
}

/**
 * Адрес таблицы и число методов из файла, который написал лаунчер.
 *
 * Строки в нём такие: `vtable.Actor = 0x13891da0 138`. Адрес отсчитан от
 * начала библиотеки, поэтому к нему ещё нужно прибавить то место, куда её
 * положила система.
 */
bool lookup_vtable(const std::string& path, const std::string& name, uintptr_t& address,
                   size_t& slots) {
    const std::string value = setting(path, "vtable." + name);
    if (value.empty()) return false;

    unsigned long long parsed_address = 0;
    unsigned long parsed_slots = 0;
    if (sscanf(value.c_str(), "%llx %lu", &parsed_address, &parsed_slots) < 1) return false;

    address = static_cast<uintptr_t>(parsed_address);
    slots = parsed_slots;
    return address != 0;
}

/** Таблица лежит в области, которую компоновщик закрыл от записи. */
bool make_writable(void* address, size_t length) {
    const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~(page - 1);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(address) + length + page - 1) & ~(page - 1);

    return mprotect(reinterpret_cast<void*>(start), end - start,
                    PROT_READ | PROT_WRITE) == 0;
}

/**
 * Дописывает, что произошло за последние секунды.
 *
 * Без этого выяснить, какой метод за что отвечает, можно было только так:
 * сделать одно действие, переписать файл, сделать другое, переписать снова.
 * Занятие изматывающее и требующее аккуратности, которой при игре не бывает.
 *
 * Здесь же остаётся просто играть. Лента пишется сама, и по ней потом видно,
 * что в такую-то секунду сработали такие-то методы, — а что человек в ту
 * секунду делал, он помнит и так.
 */
void append_timeline(double seconds);
std::string panel_html();

}  // namespace

namespace {

void append_timeline(double seconds) {
    if (g_timeline_path.empty()) return;
    ++g_windows;

    std::ostringstream line;
    bool anything = false;

    for (size_t slot = 0; slot < g_slots && slot < kMaxSlots; ++slot) {
        const uint64_t total = nrz_probe_slots[slot].count;
        const uint64_t delta = total - g_timeline_previous[slot];
        g_timeline_previous[slot] = total;
        g_rates[slot].observe(total);
        if (delta == 0) continue;

        if (anything) line << ", ";
        line << slot << "+" << delta;
        anything = true;
    }
    if (!anything) return;

    std::ofstream file(g_timeline_path, std::ios::app);
    if (!file) return;
    file << static_cast<long>(seconds) << " с: " << line.str() << "\n";
}

void worker() {
    const auto start = std::chrono::steady_clock::now();

    // Панель открывается отсюда, а не на пути запуска игры: окно создаётся
    // средствами Android, и просить их об этом, пока приложение само не
    // достроено, — верный способ подвесить загрузку мира. К пятнадцатой
    // секунде игра давно на ногах.
    if (g_want_panel) {
        std::this_thread::sleep_for(std::chrono::seconds(15));

        const std::string trouble = overlay::trouble();
        if (trouble.empty()) {
            overlay::open("nrz.probe", panel_html(), 8, 8, 150, 0, true);
            MCBE_LOGI("панель отметок открыта");
        } else {
            MCBE_LOGW("панель отметок не открыть: %s", trouble.c_str());
        }
    }

    hud::install(Loader::instance(), g_config_path);

    // Отчёт пишется реже ленты: он для чтения целиком, а лента для того,
    // чтобы поймать момент.
    int step = 0;
    while (g_running.load()) {
        // Окошко обновляется впятеро чаще ленты: пять секунд задержки
        // превращают показометр в бесполезную табличку.
        for (int tick = 0; tick < 25 && g_running.load(); ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            hud::refresh();
        }
        if (!g_running.load()) break;

        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        append_timeline(seconds);
        if (++step % 3 == 0) write_report();
    }
}

/** Кнопки, которыми человек размечает ленту, не отрываясь от игры. */
constexpr const char* kActions[] = {
    "ломаю блок", "поставил блок", "бью моба", "получил урон",
    "открыл сундук", "крафчу", "прыгаю", "плыву",
    "ем", "сменил предмет", "открыл инвентарь", "просто стою",
};

std::string panel_html() {
    std::ostringstream page;
    page << "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<style>"
            "*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;"
              "user-select:none}"
            "body{margin:0;font:600 10px/1 -apple-system,Roboto,sans-serif;color:#e8ecf1}"
            ".w{background:rgba(12,14,18,.66);backdrop-filter:blur(12px);border-radius:12px;"
              "border:1px solid rgba(255,255,255,.08)}"
            ".h{display:flex;align-items:center;gap:6px;padding:7px 9px}"
            ".d{width:6px;height:6px;border-radius:50%;background:#5fd28d}"
            ".n{flex:1;opacity:.5;letter-spacing:.08em}"
            ".g{display:none;grid-template-columns:1fr 1fr;gap:3px;padding:0 6px 6px}"
            ".o .g{display:grid}"
            "b{border:0;border-radius:8px;padding:7px 3px;background:rgba(255,255,255,.07);"
              "color:inherit;font:inherit}"
            "b:active{background:#5fd28d;color:#0b120e}"
            "</style>"
            "<div class=w id=w><div class=h onclick=\"t()\">"
              "<span class=d></span><span class=n id=n>ОТМЕТКИ</span><span id=a>+</span>"
            "</div><div class=g>";

    for (const char* action : kActions) {
        page << "<b onclick=\"m(event,'" << action << "')\">" << action << "</b>";
    }

    // Свёрнутая по умолчанию: развёрнутая занимала пол-экрана, и по ней
    // попадали случайно, целясь в игру.
    page << "</div></div><script>"
            "var o=0,w=document.getElementById('w');"
            "function t(){o=!o;w.className=o?'w o':'w';"
              "document.getElementById('a').textContent=o?'\u2212':'+';"
              "document.getElementById('n').textContent=o?'\u041e\u0422\u041c\u0415\u0422\u041a\u0418':''}"
            "function m(e,x){e.stopPropagation();nrzhost.send('nrz:mark '+x);"
              "var d=document.querySelector('.d');d.style.background='#ffd166';"
              "setTimeout(function(){d.style.background='#5fd28d'},600)}"
            "t();t();"
            "</script>";
    return page.str();
}

}  // namespace

void mark(const std::string& label) {
    if (g_timeline_path.empty()) return;

    // Сначала дописывается то, что накопилось до отметки: иначе действие и
    // предшествующее ему безделье слиплись бы в одно окно.
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_started).count();
    append_timeline(seconds);

    std::ofstream file(g_timeline_path, std::ios::app);
    if (file) file << "--- " << static_cast<long>(seconds) << " с: " << label << " ---\n";

    MCBE_LOGI("отметка: %s", label.c_str());
}

uint64_t counter(size_t slot) {
    return slot < kMaxSlots ? nrz_probe_slots[slot].count : 0;
}

bool running() { return g_running.load(); }

void write_report() {
    if (g_vtable == nullptr || g_report_path.empty()) return;

    std::ostringstream out;
    out << "# NRZLoader — какие методы " << g_class << " вызывались\n"
        << "# Имён у методов нет: их вырезали из игры. Зато видно, когда какой\n"
        << "# срабатывает, и по этому его можно узнать. Двадцать вызовов в\n"
        << "# секунду — это тик. Один при ударе — обработка урона.\n"
        << "#\n"
        << "# всего мест в таблице: " << g_slots << "\n\n";

    std::vector<std::pair<uint64_t, size_t>> hits;
    for (size_t slot = 0; slot < g_slots && slot < kMaxSlots; ++slot) {
        const uint64_t count = nrz_probe_slots[slot].count;
        if (count != 0) hits.emplace_back(count, slot);
    }
    std::sort(hits.rbegin(), hits.rend());

    const auto now = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(now - g_started).count();
    const double window =
        std::chrono::duration<double>(now - g_last_report).count();
    g_last_report = now;

    const uint64_t base = find_base(hits);

    out << "сработало мест: " << hits.size() << "\n"
        << "игра идёт: " << static_cast<long>(seconds) << " с\n";
    if (base != 0) {
        out << "тактов было: " << base << "\n";
    }
    out << "\n";

    char line[160] = {};
    for (const auto& hit : hits) {
        const size_t slot = hit.second;
        const uint64_t total = hit.first;
        const uint64_t delta = total - g_previous[slot];
        g_previous[slot] = total;

        // Ровная скорость значит привязку к такту, гуляющая — к кадрам:
        // такт игра держит любой ценой, а кадры проседают.
        std::string per_tick = "—";
        if (base != 0) {
            const double ratio = static_cast<double>(total) / base;
            const bool steady =
                g_rates[slot].constant(g_windows) && g_rates[slot].wobble() <= 0.08;
            char text[40] = {};

            if (steady && std::fabs(ratio - std::round(ratio)) < 0.02 && ratio >= 0.98) {
                snprintf(text, sizeof(text), "%.0f за такт", std::round(ratio));
            } else if (steady) {
                snprintf(text, sizeof(text), "%.2f за такт", ratio);
            } else {
                snprintf(text, sizeof(text), "%.1f за такт, скачет", ratio);
            }
            per_tick = text;
        }

        snprintf(line, sizeof(line), "  слот %-4zu %10llu  %-14s  за последние %2.0f с: %llu\n",
                 slot, static_cast<unsigned long long>(total), per_tick.c_str(), window,
                 static_cast<unsigned long long>(delta));
        out << line;
    }

    if (hits.empty()) {
        out << "  пока ни одного. Зайдите в мир и подвигайтесь.\n";
    } else {
        out << "\n«за такт» без пометки — привязан к такту игры, он ровно 20 в\n"
            << "секунду. «скачет» — привязан к кадрам, а их число гуляет.\n"
            << "Остальное ищите в slots-timeline.txt: там видно, что срабатывало\n"
            << "в ту секунду, когда вы что-то делали.\n";
    }

    std::ofstream file(g_report_path, std::ios::trunc);
    if (file) file << out.str();
}

#if defined(__aarch64__)

bool install(Loader& loader, const std::string& config_path) {
    const std::string wanted = setting(config_path, "probe.class");
    if (wanted.empty()) {
        // Сказать об этом стоит: иначе непонятно, выключено оно или сломалось.
        MCBE_LOGI("разведка методов выключена; чтобы включить, добавьте в %s строку "
                  "probe.class = LocalPlayer",
                  config_path.c_str());
        return false;
    }
    MCBE_LOGI("разведка методов: класс %s", wanted.c_str());

    const std::string tables = loader.data_directory() + "/config/vtables.conf";

    uintptr_t link_address = 0;
    size_t slots = 0;
    if (!lookup_vtable(tables, wanted, link_address, slots)) {
        MCBE_LOGW("нет записи vtable.%s в %s — нажмите «Проверить» в лаунчере, "
                  "он этот файл и пишет",
                  wanted.c_str(), tables.c_str());
        return false;
    }

    const LoadedModule game = LoadedModule::find("libminecraftpe.so");
    if (!game.valid) {
        MCBE_LOGE("библиотека игры не найдена, счётчики не поставить");
        return false;
    }

    if (slots == 0 || slots > kMaxSlots) slots = kMaxSlots;

    auto** vtable = reinterpret_cast<void**>(game.load_bias + link_address);
    if (!make_writable(vtable, slots * sizeof(void*))) {
        MCBE_LOGE("таблица %s закрыта от записи", wanted.c_str());
        return false;
    }

    for (size_t slot = 0; slot < slots; ++slot) {
        nrz_probe_slots[slot].original = vtable[slot];
        nrz_probe_slots[slot].count = 0;
        vtable[slot] = const_cast<unsigned char*>(nrz_probe_stubs) + slot * kStubSize;
    }

    g_class = wanted;
    g_slots = slots;
    g_vtable = vtable;
    g_report_path = loader.data_directory() + "/reports/slots.txt";
    g_timeline_path = loader.data_directory() + "/reports/slots-timeline.txt";

    // Папку создаём заранее: поток пишет туда каждые пятнадцать секунд, и
    // разбираться с этим на каждом заходе незачем.
    ::mkdir((loader.data_directory() + "/reports").c_str(), 0775);

    g_started = std::chrono::steady_clock::now();
    g_last_report = g_started;

    // Лента начинается заново с каждым запуском игры: прошлая к нынешним
    // счётчикам отношения не имеет.
    {
        std::ofstream fresh(g_timeline_path, std::ios::trunc);
        if (fresh) {
            fresh << "# Что срабатывало и когда, у класса " << wanted << ".\n"
                  << "# Строка на каждые пять секунд: номер метода и сколько раз\n"
                  << "# он сработал за эти пять секунд.\n"
                  << "#\n"
                  << "# Просто играйте. Что вы делали в такую-то секунду, вы помните,\n"
                  << "# а что при этом вызывалось — написано здесь.\n\n";
        }
    }

    // Отчёт пишется сразу, ещё пустой: так видно, что счётчики встали, и не
    // приходится гадать, ждать ли дальше.
    write_report();

    g_want_panel = setting(config_path, "probe.markers") != "off";
    g_config_path = config_path;

    g_running.store(true);
    std::thread(worker).detach();

    MCBE_LOGI("счётчики стоят на %s: %zu мест, таблица %p", wanted.c_str(), slots,
              static_cast<void*>(vtable));
    MCBE_LOGI("отчёт будет в %s", g_report_path.c_str());
    return true;
}

#else

bool install(Loader&, const std::string&) { return false; }

#endif

}  // namespace mcbe::probe
