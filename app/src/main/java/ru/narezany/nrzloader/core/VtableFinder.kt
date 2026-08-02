package ru.narezany.nrzloader.core

import java.io.InputStream

/**
 * От имени класса к таблице его виртуальных методов.
 *
 * Цепочка в C++ такая. Имя класса лежит строкой в данных. На эту строку
 * ссылается структура typeinfo: второе поле в ней — указатель на имя. А на
 * саму typeinfo ссылается таблица виртуальных методов: в ней typeinfo стоит
 * вторым полем, а дальше подряд идут адреса методов.
 *
 * Значит, найдя строку, можно пойти по ссылкам обратно и получить адрес
 * таблицы. Какой метод в ней под каким номером — скажет выделенный сервер
 * Mojang, где имена методов никто не вырезал. Это и есть способ добраться до
 * кода игры, не разбирая двести мегабайт машинных команд вручную.
 *
 * Оговорка, которая может всё остановить: в библиотеке для Android указатели
 * внутри данных иногда лежат в файле нулями, а настоящие адреса подставляются
 * при загрузке по отдельной таблице. Тогда искать по файлу нечего, и это
 * видно по статистике, которую отчёт приводит рядом с результатом.
 */
object VtableFinder {
    /** Больше этого в память не берём: телефону такое не выдержать. */
    private const val MAX_BUFFERED = 96L * 1024 * 1024

    /**
     * На typeinfo ссылаются из двух разных мест, и это разные находки.
     *
     * Из таблицы виртуальных методов — тогда сразу за ссылкой идут адреса
     * самих методов. И из typeinfo класса-наследника, у которого поле
     * «базовый класс» указывает сюда же, — тогда за ссылкой методов нет,
     * зато рядом лежит имя наследника.
     */
    enum class Kind { VTABLE, DERIVED, UNKNOWN }

    data class Found(
        val name: String,
        val nameAddress: Long,
        val typeInfoAddress: Long,
        val vtableAddress: Long,
        /** Сколько подряд идущих значений похожи на адреса кода. */
        val methodSlots: Int,
        val kind: Kind = Kind.UNKNOWN,
        /** Имя наследника, когда находка — его typeinfo. */
        val derived: String = "",
    )

    data class Report(
        val namesLocated: Map<String, Long>,
        val found: List<Found>,
        val pointersRead: Long,
        val nonZeroPointers: Long,
        val note: String,
        /** Какой список перемещений нашёлся и сколько значений подставлено. */
        val relocations: Relocations.Outcome? = null,
    ) {
        /**
         * Указатели в файле есть, а не подставляются при загрузке.
         *
         * Если данные почти сплошь нули, идти по ссылкам бесполезно: адреса
         * появятся только в памяти запущенной игры.
         */
        val pointersAreInTheFile: Boolean
            get() = pointersRead > 0 && nonZeroPointers * 4 > pointersRead
    }

    fun run(open: () -> InputStream, wanted: Set<String>): Report {
        val sections = ElfLayout.read(open)

        val rodata = sections.filter { it.name == ".rodata" && it.size > 0 }
        val pointerSections = sections
            .filter { it.name in setOf(".data.rel.ro", ".data.rel.ro.local", ".data") }
            .filter { it.size > 0 }
            .sortedBy { it.offset }

        if (rodata.isEmpty() || pointerSections.isEmpty()) {
            return Report(emptyMap(), emptyList(), 0, 0, "в библиотеке нет нужных секций")
        }

        // Шаг первый: где лежит каждое имя. Запоминаются все, а не только
        // искомые: по ним потом читаются имена классов-наследников.
        val allNames = HashMap<Long, String>()
        val addresses = locateNames(open, rodata, wanted, allNames)
        if (addresses.isEmpty()) {
            return Report(emptyMap(), emptyList(), 0, 0, "ни одного имени не нашлось")
        }

        val total = pointerSections.sumOf { it.size }
        if (total > MAX_BUFFERED) {
            return Report(addresses, emptyList(), 0, 0,
                "данных ${total / (1024 * 1024)} МБ — больше, чем можно взять в память")
        }

        val blocks = readSections(open, pointerSections)

        // В файле указателей почти нет: компоновщик заменил их нулями, а
        // настоящие значения лежат отдельным списком. Без него дальше идти
        // не по чему.
        val relocations = Relocations.apply(open, sections, blocks)

        // Шаг второй: кто ссылается на имя. Это второе поле typeinfo, значит
        // сама структура начинается на восемь байт раньше.
        //
        // Берутся все имена, а не только искомые. Голых Actor в игре не
        // бывает: каждая сущность — Mob, ItemActor, Boat, и у каждого класса
        // своя копия таблицы. Подменять надо ту, что у создаваемого класса,
        // поэтому нужны все.
        val byAddress = HashMap<Long, String>(allNames.size)
        for ((at, name) in allNames) {
            if (name.length < 3) continue
            // Класс из пространства имён зовётся коротко, если о нём и
            // спрашивали коротко: mce::ItemStack и ItemStack — одно и то же.
            val short = name.substringAfterLast("::")
            byAddress[at] = if (short in wanted) short else name
        }
        val typeInfos = HashMap<Long, String>()
        var pointersRead = 0L
        var nonZero = 0L

        forEachPointer(blocks) { at, value ->
            pointersRead++
            if (value != 0L) nonZero++
            val name = byAddress[value]
            if (name != null) typeInfos[at - 8] = name
        }

        if (typeInfos.isEmpty()) {
            val note = if (nonZero * 4 > pointersRead) {
                "на имена никто не ссылается: возможно, у этих классов нет " +
                    "виртуальных методов"
            } else {
                "данные почти сплошь нули: адреса подставляются при загрузке, " +
                    "по файлу их не пройти"
            }
            return Report(addresses, emptyList(), pointersRead, nonZero, note, relocations)
        }

        // Шаг третий: кто ссылается на typeinfo. Из таблицы методов — тогда
        // сразу за ссылкой идут методы. Из typeinfo наследника — тогда
        // методов нет, а на восемь байт раньше лежит указатель на его имя.
        val found = ArrayList<Found>()
        forEachPointer(blocks) { at, value ->
            val name = typeInfos[value] ?: return@forEachPointer

            val vtable = at + 8
            val slots = countMethods(blocks, vtable, sections)

            val derived = if (slots == 0) {
                val nameSlot = pointerAt(blocks, at - 8)
                allNames[nameSlot].orEmpty()
            } else {
                ""
            }

            // Ссылок на typeinfo много: у Packet их за две сотни. В список
            // идут только настоящие таблицы и потомки тех классов, о которых
            // спрашивали, — иначе он вырастет на десятки тысяч строк.
            if (slots == 0 && name !in wanted) return@forEachPointer

            found.add(
                Found(
                    name = name,
                    nameAddress = addresses[name] ?: 0L,
                    typeInfoAddress = value,
                    vtableAddress = vtable,
                    methodSlots = slots,
                    kind = when {
                        slots > 0 -> Kind.VTABLE
                        derived.isNotEmpty() -> Kind.DERIVED
                        else -> Kind.UNKNOWN
                    },
                    derived = derived,
                )
            )
        }

        return Report(
            namesLocated = addresses,
            found = found.sortedBy { it.name },
            pointersRead = pointersRead,
            nonZeroPointers = nonZero,
            note = "",
            relocations = relocations,
        )
    }

    // ------------------------------------------------------------------
    // Шаги по отдельности
    // ------------------------------------------------------------------

    /**
     * Виртуальный адрес каждого искомого имени.
     *
     * .rodata у игры на десятки мегабайт, поэтому читается окнами, а не
     * целиком; адрес считается по тому, сколько байт секции уже прошло.
     */
    private fun locateNames(
        open: () -> InputStream,
        rodata: List<ElfLayout.Section>,
        wanted: Set<String>,
        allNames: MutableMap<Long, String>,
    ): Map<String, Long> {
        val addresses = HashMap<String, Long>()
        val window = 1 shl 20
        val overlap = TypeNameScan.OVERLAP

        open().use { stream ->
            var position = 0L
            for (section in rodata.sortedBy { it.offset }) {
                stream.skipFully(section.offset - position)
                position = section.offset

                val buffer = ByteArray(window + overlap)
                var carried = 0
                var consumed = 0L   // сколько байт секции уже позади

                while (consumed < section.size) {
                    val want = minOf(window.toLong(), section.size - consumed).toInt()
                    val read = stream.read(buffer, carried, want)
                    if (read <= 0) break

                    val filled = carried + read
                    val atEnd = consumed + read >= section.size
                    val limit = if (atEnd || filled <= overlap) filled else filled - overlap

                    for (at in 0 until limit) {
                        val name = TypeNameScan.nameAt(buffer, at, filled)
                            ?: TypeNameScan.nestedAt(buffer, at, filled)
                            ?: continue

                        val inSectionHere = consumed - carried + at
                        if (allNames.size < 200_000) {
                            allNames[section.address + inSectionHere] = name
                        }

                        // typeinfo ссылается на начало записи, то есть на
                        // длину, а не на первую букву имени.
                        val short = name.substringAfterLast("::")
                        val key = when {
                            name in wanted -> name
                            short in wanted -> short
                            else -> null
                        } ?: continue

                        val inSection = consumed - carried + at
                        addresses[key] = section.address + inSection
                    }

                    consumed += read
                    carried = filled - limit
                    if (carried > 0) System.arraycopy(buffer, limit, buffer, 0, carried)
                }
                position = section.offset + section.size
            }
        }
        return addresses
    }

    private fun readSections(
        open: () -> InputStream,
        sections: List<ElfLayout.Section>,
    ): List<Relocations.Target> {
        val blocks = ArrayList<Relocations.Target>()
        open().use { stream ->
            var position = 0L
            for (section in sections) {
                stream.skipFully(section.offset - position)
                val body = ByteArray(section.size.toInt())
                stream.readFully(body, 0, body.size)
                position = section.offset + body.size
                blocks.add(Relocations.Target(section.address, body))
            }
        }
        return blocks
    }

    /** Каждое выровненное восьмибайтовое значение и адрес, где оно лежит. */
    private inline fun forEachPointer(
        blocks: List<Relocations.Target>,
        action: (Long, Long) -> Unit,
    ) {
        for (block in blocks) {
            var at = 0
            while (at + 8 <= block.bytes.size) {
                action(block.address + at, readLong(block.bytes, at))
                at += 8
            }
        }
    }

    /**
     * Сколько значений подряд после таблицы похожи на адреса кода.
     *
     * Ноль означает, что найденное — не таблица методов, а что-то другое, на
     * что случайно указывает то же значение.
     */
    private fun countMethods(
        blocks: List<Relocations.Target>,
        vtable: Long,
        sections: List<ElfLayout.Section>,
    ): Int {
        val text = sections.firstOrNull { it.name == ".text" } ?: return 0
        val from = text.address
        val to = text.address + text.size

        val block = blocks.firstOrNull {
            vtable >= it.address && vtable < it.address + it.bytes.size
        } ?: return 0

        var at = (vtable - block.address).toInt()
        var slots = 0
        while (at + 8 <= block.bytes.size && slots < 512) {
            val value = readLong(block.bytes, at)
            if (value < from || value >= to) break
            slots++
            at += 8
        }
        return slots
    }

    /** Значение по адресу, если он попадает в прочитанные куски. */
    private fun pointerAt(blocks: List<Relocations.Target>, address: Long): Long {
        val block = blocks.firstOrNull {
            address >= it.address && address + 8 <= it.address + it.bytes.size
        } ?: return 0
        return readLong(block.bytes, (address - block.address).toInt())
    }

    private fun readLong(data: ByteArray, at: Int): Long {
        var value = 0L
        for (index in 7 downTo 0) {
            value = (value shl 8) or (data[at + index].toLong() and 0xFF)
        }
        return value
    }
}
