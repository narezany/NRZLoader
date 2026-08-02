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
        val classNames: Int,
        val found: List<String>,
        val missing: List<String>,
        val anchors: Map<String, Int>,
        val samples: List<String>,
    ) {
        /**
         * Признаков два, и одного мало: список ожидаемых имён — всё-таки
         * догадки о том, как классы называются, а общее их число ничего не
         * говорит о том, те ли это имена.
         */
        val verdict: Verdict
            get() = when {
                found.size >= 5 || classNames > 2000 -> Verdict.PRESENT
                classNames > 0 -> Verdict.PARTIAL
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

            val tally = zip.getInputStream(entry).use {
                TypeNameScan.scan(it, WANTED.toSet(), ANCHORS)
            }

            return Result(
                libraryBytes = entry.size,
                classNames = tally.names,
                found = WANTED.filter { it in tally.found },
                missing = WANTED.filterNot { it in tally.found },
                anchors = tally.anchors,
                samples = tally.samples.toList(),
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
}
