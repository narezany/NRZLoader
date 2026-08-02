package ru.narezany.nrzloader.core

import java.io.File
import java.util.zip.ZipFile

/**
 * Ищет в игре следы имён классов, которых нет в таблице символов.
 *
 * Розничная сборка вырезает имена функций — это давно видно по логу. Но C++
 * хранит имена классов ещё в одном месте: у класса с виртуальными методами
 * рядом с таблицей этих методов лежит структура typeinfo, а в ней строка с
 * искажённым именем класса. Строки из данных никто не вырезает.
 *
 * Если строки на месте, от имени класса есть дорога к таблице виртуальных
 * методов, а какой метод в ней под каким номером — говорит выделенный сервер
 * Mojang, где символы никто не трогал. Тогда геймплейные функции находятся
 * механически.
 *
 * Проверка живёт здесь, а не в скрипте для компьютера, по простой причине:
 * начиная с Android 11 посторонняя программа вообще не видит, где лежит чужой
 * пакет, а лаунчер видит — он для этого и написан.
 */
object RttiProbe {
    /** Классы, ради которых всё и затевается. */
    private val WANTED = listOf(
        "Actor", "Mob", "Player", "ServerPlayer", "Level", "ServerLevel",
        "Dimension", "BlockSource", "BlockLegacy", "Block", "ItemStack",
        "Item", "ItemStackBase", "GameMode", "ChunkSource", "LevelChunk",
        "Container", "Packet", "HitResult", "Vec3", "BlockPos",
    )

    /**
     * Строки, которые в игре точно есть и переживают любую пересборку. Даже
     * когда RTTI вырезан, они остаются якорями: найдя такую строку, можно
     * найти обращающийся к ней код.
     */
    private val ANCHORS = listOf(
        "minecraft:player", "minecraft:stone", "minecraft:overworld",
        "selected_item", "scoreboard",
    )

    private const val LIBRARY = "lib/arm64-v8a/libminecraftpe.so"

    data class Result(
        val libraryBytes: Long,
        val sectionsScanned: List<String>,
        /** Почему секции разобрать не вышло, если не вышло. */
        val sectionTrouble: String = "",
        val classNames: Int,
        val found: List<String>,
        val missing: List<String>,
        val anchors: Map<String, Int>,
        val samples: List<String>,
        /** Найденные таблицы виртуальных методов, если до них дошло. */
        val vtables: VtableFinder.Report? = null,
    ) {
        /**
         * Признаков два, и одного мало: список ожидаемых имён — всё-таки
         * догадки о том, как классы называются, а общее их число ничего не
         * говорит о том, те ли это имена.
         */
        /**
         * Считается по найденным именам, а не по их числу.
         *
         * Общее число легко раздувается мусором — совпадение вроде «qQ» ничего
         * не значит, — а вот ServerPlayer или ItemStackBase случайно не
         * выпадают.
         */
        val verdict: Verdict
            get() = when {
                found.size >= 5 -> Verdict.PRESENT
                found.isNotEmpty() -> Verdict.PARTIAL
                else -> Verdict.ABSENT
            }

        enum class Verdict { PRESENT, PARTIAL, ABSENT }
    }

    /** Разбирает игру, найденную лаунчером. Дело на минуту, не для main. */
    fun run(install: GameLocator.Installed): Result {
        val apk = install.apkPaths.firstOrNull { hasLibrary(it) }
            ?: throw IllegalStateException("в пакете нет $LIBRARY")

        ZipFile(apk).use { zip ->
            val entry = zip.getEntry(LIBRARY)
                ?: throw IllegalStateException("в пакете нет $LIBRARY")

            val open = { zip.getInputStream(entry) }

            // Имена ищутся только в данных. Две трети библиотеки — машинный
            // код, и узор «цифра, пара букв, ноль» встречается в нём тысячи
            // раз просто так; без этого ограничения счётчик врёт.
            //
            // Если разобрать секции не вышло, это должно быть написано в
            // отчёте: молчаливый откат на чтение всего файла один раз уже
            // выдал мусор за результат.
            var trouble = ""
            val sections = try {
                ElfLayout.dataSections(ElfLayout.read(open))
            } catch (error: Throwable) {
                trouble = error.message ?: error.toString()
                emptyList()
            }

            val tally = open().use { stream ->
                if (sections.isEmpty()) {
                    TypeNameScan.scan(stream, WANTED.toSet(), ANCHORS)
                } else {
                    TypeNameScan.scanRegions(
                        stream,
                        sections.map { it.offset to it.size },
                        WANTED.toSet(),
                        ANCHORS,
                    )
                }
            }

            // Второй заход: от найденных имён к самим таблицам. Это заодно
            // и проверка первого — на случайное совпадение строки никто не
            // ссылается, а на настоящее имя ссылается typeinfo.
            val vtables = runCatching {
                VtableFinder.run(open, WANTED.toSet())
            }.getOrNull()

            return Result(
                libraryBytes = entry.size,
                sectionsScanned = sections.map { it.name },
                sectionTrouble = trouble,
                classNames = tally.names,
                found = WANTED.filter { it in tally.found },
                missing = WANTED.filterNot { it in tally.found },
                anchors = tally.anchors,
                samples = tally.samples.toList(),
                vtables = vtables,
            )
        }
    }

    private fun hasLibrary(path: String): Boolean =
        runCatching { ZipFile(path).use { it.getEntry(LIBRARY) != null } }.getOrDefault(false)

    /** Отчёт в файл, чтобы его можно было переслать, а не переписывать. */
    fun writeReport(result: Result, install: GameLocator.Installed): File? = runCatching {
        val file = File(File(ModsFolder.root, "reports"), "rtti.txt")
        file.parentFile?.mkdirs()

        file.writeText(
            buildString {
                appendLine("NRZLoader ${LoaderVersion.VALUE} — проверка RTTI")
                appendLine("игра: ${install.packageName} ${install.versionName}")
                appendLine("библиотека: ${result.libraryBytes / (1024 * 1024)} МБ")
                appendLine(
                    "просмотрено: " +
                        result.sectionsScanned.joinToString(", ").ifBlank { "весь файл" }
                )
                if (result.sectionTrouble.isNotBlank()) {
                    appendLine("секции разобрать не вышло: ${result.sectionTrouble}")
                }
                appendLine()
                appendLine("имён классов в данных: ${result.classNames}")
                appendLine("из ожидаемых нашлось: ${result.found.size} из ${WANTED.size}")
                appendLine()
                appendLine("есть:  ${result.found.joinToString(", ").ifBlank { "—" }}")
                appendLine("нет:   ${result.missing.joinToString(", ").ifBlank { "—" }}")
                appendLine()
                appendLine("строки-якоря:")
                result.anchors.forEach { (text, times) -> appendLine("  $text: $times") }
                appendLine()
                appendLine("примеры найденных имён:")
                result.samples.forEach { appendLine("  $it") }
                appendLine()

                val tables = result.vtables
                appendLine("таблицы виртуальных методов:")
                when {
                    tables == null -> appendLine("  разобрать не вышло")
                    tables.found.isEmpty() -> {
                        appendLine("  не найдено — ${tables.note}")
                        tables.relocations?.let {
                            appendLine("  список перемещений: ${it.kind}, подставлено ${it.applied}")
                        }
                        appendLine("  указателей: ${tables.pointersRead}, " +
                            "ненулевых ${tables.nonZeroPointers}")
                    }
                    else -> {
                        tables.relocations?.let {
                            appendLine("  список перемещений: ${it.kind}, подставлено ${it.applied}")
                        }
                        appendLine("  указателей: ${tables.pointersRead}, " +
                            "ненулевых ${tables.nonZeroPointers}")
                        appendLine()

                        // Настоящая таблица у класса одна: та, за которой
                        // больше всего методов. Остальные — вторые таблицы
                        // при множественном наследовании.
                        val vtables = tables.found
                            .filter { it.kind == VtableFinder.Kind.VTABLE }
                            .groupBy { it.name }

                        vtables.entries.sortedBy { it.key }.forEach { (name, all) ->
                            val main = all.maxByOrNull { it.methodSlots } ?: return@forEach
                            appendLine(
                                "  %-14s 0x%08x, методов %d%s".format(
                                    name, main.vtableAddress, main.methodSlots,
                                    if (all.size > 1) " (ещё ${all.size - 1} вторичных)" else ""
                                )
                            )
                        }

                        // Ссылка на typeinfo без методов за ней — это класс,
                        // унаследованный от найденного. Так вычитывается всё
                        // семейство.
                        val children = tables.found
                            .filter { it.kind == VtableFinder.Kind.DERIVED }
                            .groupBy({ it.name }, { it.derived })
                            .mapValues { it.value.distinct().sorted() }

                        if (children.isNotEmpty()) {
                            appendLine()
                            appendLine("наследники:")
                            children.entries.sortedByDescending { it.value.size }
                                .forEach { (base, kids) ->
                                    appendLine("  $base — ${kids.size}")
                                    kids.take(24).forEach { appendLine("      $it") }
                                    if (kids.size > 24) {
                                        appendLine("      … и ещё ${kids.size - 24}")
                                    }
                                }
                        }
                    }
                }
                appendLine()

                appendLine(
                    when (result.verdict) {
                        Result.Verdict.PRESENT ->
                            "RTTI на месте. От имени класса есть дорога к таблице виртуальных\n" +
                                "методов, а номер метода в ней скажет выделенный сервер Mojang."
                        Result.Verdict.PARTIAL ->
                            "RTTI похоже вырезан: имён слишком мало для настоящей таблицы классов."
                        Result.Verdict.ABSENT ->
                            "RTTI вырезан. Остаются строки-якоря."
                    }
                )
            }
        )
        file
    }.getOrNull()

    /**
     * Адреса в виде, который читает сам загрузчик.
     *
     * Отчёт выше — для человека, а это для дела: строки вида
     * `vtable.Actor = 0x13891da0 138` кладутся в config, и мод может
     * перехватить метод по номеру, не зная ни одного имени функции.
     */
    fun writeTable(result: Result): File? = runCatching {
        val tables = result.vtables ?: return@runCatching null
        if (tables.found.isEmpty()) return@runCatching null

        val file = File(File(ModsFolder.root, "reports"), "vtables.txt")
        file.parentFile?.mkdirs()

        // Рядом — копия туда, где её читает сам загрузчик при запуске игры.
        val forLoader = File(ModsFolder.config, "vtables.conf")
        forLoader.parentFile?.mkdirs()

        file.writeText(
            buildString {
                appendLine("# NRZLoader ${LoaderVersion.VALUE} — таблицы виртуальных методов")
                appendLine("# класс = адрес таблицы, число методов")
                appendLine("# Адреса от начала библиотеки; загрузчик прибавит, куда её положили.")
                appendLine()

                tables.found
                    .filter { it.kind == VtableFinder.Kind.VTABLE }
                    .groupBy { it.name }
                    .entries.sortedBy { it.key }
                    .forEach { (name, all) ->
                        val main = all.maxByOrNull { it.methodSlots } ?: return@forEach
                        appendLine("vtable.%s = 0x%x %d".format(name, main.vtableAddress,
                            main.methodSlots))
                    }

                appendLine()
                appendLine("# наследники, по одному в строке")
                tables.found
                    .filter { it.kind == VtableFinder.Kind.DERIVED }
                    .groupBy({ it.name }, { it.derived })
                    .forEach { (base, kids) ->
                        kids.distinct().sorted().forEach { appendLine("child.$base = $it") }
                    }
            }
        )
        runCatching { file.copyTo(forLoader, overwrite = true) }
        file
    }.getOrNull()
}
