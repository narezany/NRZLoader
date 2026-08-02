#!/usr/bin/env python3
"""Ищет в клиенте следы имён классов, которых нет в таблице символов.

Зачем. Розничная сборка вырезает имена функций — это мы уже видели. Но C++
хранит имена классов ещё в одном месте: в RTTI. Если у класса есть виртуальные
методы и сборка не собрана с -fno-rtti, рядом с таблицей виртуальных функций
лежит структура typeinfo, а в ней — строка с искажённым именем класса. Строки
в .rodata никто не вырезает.

Если эти строки на месте, открывается вот что: от строки идём к typeinfo, от
typeinfo к таблице виртуальных функций, а какой метод в ней под каким номером —
говорит выделенный сервер Mojang, у которого символы есть. То есть половина
геймплейных функций находится механически, без дизассемблера.

Если строк нет — этот путь закрыт, и лучше узнать это за минуту, чем за месяц.

    python3 tools/probe_rtti.py base.apk split_config.arm64_v8a.apk
    python3 tools/probe_rtti.py libminecraftpe.so
"""

import re
import struct
import sys
import zipfile

# Классы, ради которых всё и затевается. Если найдётся хотя бы часть —
# остальное ищется тем же способом.
WANTED = [
    "Actor", "Mob", "Player", "ServerPlayer", "Level", "ServerLevel",
    "Dimension", "BlockSource", "BlockLegacy", "Block", "ItemStack",
    "Item", "ItemStackBase", "GameMode", "ChunkSource", "LevelChunk",
    "Container", "ContainerModel", "MinecraftPackets", "Packet",
    "ActorDefinitionIdentifier", "HitResult", "Vec3", "BlockPos",
]

# Строки-литералы, которые в игре точно есть и переживают любую пересборку.
# Даже когда RTTI вырезан, они остаются якорями: найдя такую строку, можно
# найти обращающийся к ней код.
ANCHORS = [
    b"minecraft:player",
    b"minecraft:stone",
    b"minecraft:overworld",
    b"minecraft:creeper",
    b"selected_item",
    b"MinecraftEventing",
    b"scoreboard",
]


def read_library(paths):
    """Возвращает содержимое libminecraftpe.so под arm64 и путь, откуда оно."""
    for path in paths:
        if path.endswith(".so"):
            with open(path, "rb") as handle:
                return handle.read(), path

        with zipfile.ZipFile(path) as archive:
            names = [n for n in archive.namelist() if n.endswith("libminecraftpe.so")]
            # Сплит под нужную архитектуру может лежать в любом из файлов.
            names.sort(key=lambda n: "arm64" not in n)
            if names:
                return archive.read(names[0]), "%s!%s" % (path, names[0])

    raise SystemExit("libminecraftpe.so не найдена ни в одном из указанных файлов")


def sections(image):
    """Разбор заголовков секций ELF64: (имя, тип, смещение, размер, link, entsize)."""
    if image[:4] != b"\x7fELF":
        raise SystemExit("это не ELF")

    table, = struct.unpack_from("<Q", image, 0x28)
    entry_size, count, names_index = struct.unpack_from("<HHH", image, 0x3A)

    raw = [struct.unpack_from("<IIQQQQIIQQ", image, table + i * entry_size)
           for i in range(count)]

    name_table = raw[names_index]
    strings = image[name_table[4]:name_table[4] + name_table[5]]

    result = []
    for entry in raw:
        end = strings.find(b"\0", entry[0])
        result.append((strings[entry[0]:end].decode("utf8", "replace"),
                       entry[1], entry[4], entry[5], entry[6], entry[9]))
    return result


def dynamic_symbols(image, parsed):
    """Имена из таблиц символов — то, что доступно и без всяких ухищрений."""
    found = set()
    for _name, kind, offset, size, link, entry_size in parsed:
        if kind not in (2, 11) or entry_size == 0:
            continue

        table = parsed[link]
        strings = image[table[2]:table[2] + table[3]]
        for index in range(size // entry_size):
            name_offset, = struct.unpack_from("<I", image, offset + index * entry_size)
            if not name_offset:
                continue
            end = strings.find(b"\0", name_offset)
            found.add(strings[name_offset:end].decode("utf8", "replace"))
    return found


# Искажённое имя класса в typeinfo: длина, потом само имя. Вложенные имена
# завёрнуты в N...E, поэтому проверяются отдельно.
SIMPLE = re.compile(rb"(?<![A-Za-z0-9_$])(\d{1,3})([A-Za-z_][A-Za-z0-9_]{1,80})\x00")
NESTED = re.compile(rb"N((?:\d{1,3}[A-Za-z_][A-Za-z0-9_]{1,60})+)E\x00")


def typeinfo_names(blob):
    """Строки, выглядящие как имена классов из RTTI, и их смещения в блоке."""
    names = {}

    for match in SIMPLE.finditer(blob):
        declared = int(match.group(1))
        name = match.group(2)
        # Длина должна совпасть с объявленной — иначе это просто текст.
        if declared == len(name):
            names.setdefault(name.decode("utf8", "replace"), match.start())

    for match in NESTED.finditer(blob):
        inner = match.group(1)
        parts = []
        cursor = 0
        while cursor < len(inner):
            digits = re.match(rb"\d{1,3}", inner[cursor:])
            if not digits:
                break
            length = int(digits.group(0))
            cursor += digits.end()
            parts.append(inner[cursor:cursor + length].decode("utf8", "replace"))
            cursor += length
        if parts and cursor == len(inner):
            names.setdefault("::".join(parts), match.start())

    return names


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    image, source = read_library(sys.argv[1:])
    print("библиотека: %s" % source)
    print("размер:     %.1f МБ\n" % (len(image) / (1024 * 1024)))

    parsed = sections(image)
    symbols = dynamic_symbols(image, parsed)
    print("имён в таблицах символов: %d" % len(symbols))

    # Сами таблицы виртуальных функций и typeinfo иногда остаются в символах
    # даже когда обычные функции вырезаны.
    for prefix, what in (("_ZTV", "таблиц виртуальных функций"),
                         ("_ZTI", "структур typeinfo"),
                         ("_ZTS", "имён typeinfo")):
        hits = sorted(n for n in symbols if n.startswith(prefix))
        print("  %-28s %d" % (what + ":", len(hits)))
        for name in hits[:3]:
            print("      %s" % name[:110])

    # Дальше — то, ради чего всё: строки имён классов в данных.
    print("\n== имена классов в данных ==")
    interesting = [s for s in parsed
                   if s[0] in (".rodata", ".data.rel.ro", ".data.rel.ro.local", ".rdata")]
    if not interesting:
        interesting = [s for s in parsed if s[1] == 1]  # SHT_PROGBITS

    found = {}
    for name, _kind, offset, size, _link, _entry in interesting:
        blob = image[offset:offset + size]
        for label, where in typeinfo_names(blob).items():
            found.setdefault(label, (name, offset + where))

    print("похожих на имена классов строк: %d" % len(found))

    print("\n== то, что нужно для геймплейных модов ==")
    hit = 0
    for wanted in WANTED:
        matches = [k for k in found if k == wanted or k.endswith("::" + wanted)]
        if matches:
            section, at = found[matches[0]]
            print("  %-28s есть   %s +0x%x" % (wanted, section, at))
            hit += 1
        else:
            print("  %-28s нет" % wanted)

    print("\n== строки-якоря ==")
    for anchor in ANCHORS:
        count = image.count(anchor)
        print("  %-28s %d" % (anchor.decode(), count))

    # Три независимых признака. Считать по одному нельзя: в игре скрытая
    # видимость символов, поэтому _ZTS из таблицы вырезан, а строка в данных
    # остаётся; а список выше — это догадки о названиях, и промах в нём
    # означает лишь то, что класс зовётся иначе.
    typeinfo_symbols = len([n for n in symbols if n.startswith(("_ZTS", "_ZTI", "_ZTV"))])

    print("\n== вывод ==")
    print("  имён классов в данных:      %d" % len(found))
    print("  из ожидаемых нашлось:       %d из %d" % (hit, len(WANTED)))
    print("  typeinfo в таблице символов: %d" % typeinfo_symbols)
    print()

    if hit >= 5 or len(found) > 2000 or typeinfo_symbols > 100:
        print("  RTTI на месте.")
        print("  От имени класса есть дорога к таблице виртуальных функций, а что")
        print("  в ней под каким номером — скажет выделенный сервер Mojang, где")
        print("  символы никто не вырезал. Это и есть способ добраться до")
        print("  геймплейных функций без дизассемблера.")
    elif found:
        print("  RTTI похоже вырезан: имён нашлось слишком мало, чтобы это")
        print("  выглядело как таблица классов настоящей игры.")
        print("  Остаются строки-якоря: искать код по обращениям к ним.")
    else:
        print("  RTTI вырезан. Имена классов в клиенте не сохранились.")
        print("  Остаются строки-якоря: искать код по обращениям к ним.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
