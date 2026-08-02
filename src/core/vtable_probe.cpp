#include "vtable_probe.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "loader.h"
#include "log.h"
#include "module.h"

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

void worker() {
    // Первый отчёт быстро, чтобы было видно, что счётчики живые; дальше реже.
    std::this_thread::sleep_for(std::chrono::seconds(10));
    while (g_running.load()) {
        write_report();
        std::this_thread::sleep_for(std::chrono::seconds(15));
    }
}

}  // namespace

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

    out << "сработало мест: " << hits.size() << "\n\n";
    for (const auto& hit : hits) {
        out << "  слот " << hit.second << " — вызовов " << hit.first << "\n";
    }

    if (hits.empty()) {
        out << "  пока ни одного. Зайдите в мир и подвигайтесь.\n";
    }

    std::ofstream file(g_report_path, std::ios::trunc);
    if (file) file << out.str();
}

#if defined(__aarch64__)

bool install(Loader& loader, const std::string& config_path) {
    const std::string wanted = setting(config_path, "probe.class");
    if (wanted.empty()) return false;

    const std::string tables = loader.data_directory() + "/config/vtables.conf";

    uintptr_t link_address = 0;
    size_t slots = 0;
    if (!lookup_vtable(tables, wanted, link_address, slots)) {
        MCBE_LOGW("нет записи vtable.%s в %s", wanted.c_str(), tables.c_str());
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

    // Папку создаём заранее: поток пишет туда каждые пятнадцать секунд, и
    // разбираться с этим на каждом заходе незачем.
    ::mkdir((loader.data_directory() + "/reports").c_str(), 0775);

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
