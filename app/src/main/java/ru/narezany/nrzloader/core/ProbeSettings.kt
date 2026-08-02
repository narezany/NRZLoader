package ru.narezany.nrzloader.core

import java.io.File

/**
 * Какой класс разведывать — без правки файла руками.
 *
 * Настройка живёт в том же bindings.conf, что читает загрузчик; лаунчер её
 * лишь переписывает. Держать её отдельно было бы удобнее в коде и хуже в
 * жизни: тогда открывший файл человек видел бы одно, а игра делала другое.
 */
object ProbeSettings {
    private const val KEY = "probe.class"

    private val file: File get() = File(ModsFolder.config, "bindings.conf")

    fun current(): String = runCatching {
        file.readLines()
            .map { it.substringBefore('#') }
            .firstOrNull { it.substringBefore('=').trim() == KEY }
            ?.substringAfter('=')?.trim()
            .orEmpty()
    }.getOrDefault("")

    /** Пустое имя выключает разведку, не стирая остального. */
    fun set(name: String) {
        runCatching {
            file.parentFile?.mkdirs()

            val lines = if (file.isFile) file.readLines().toMutableList() else mutableListOf()

            // Строка могла быть закомментирована — такую надо не обойти, а
            // заменить, иначе в файле окажутся две и правда будет за нижней.
            val index = lines.indexOfFirst {
                val body = it.trimStart().removePrefix("#").substringBefore('#')
                body.substringBefore('=').trim() == KEY
            }
            val line = if (name.isBlank()) "#$KEY = LocalPlayer" else "$KEY = $name"

            if (index >= 0) lines[index] = line else lines += line
            file.writeText(lines.joinToString("\n") + "\n")
        }
    }

    /** Классы из списка таблиц, самые крупные первыми: у них больше методов. */
    fun candidates(limit: Int = 40): List<String> = runCatching {
        File(ModsFolder.config, "vtables.conf").readLines()
            .filter { it.startsWith("vtable.") }
            .mapNotNull { line ->
                val name = line.substringAfter("vtable.").substringBefore('=').trim()
                val slots = line.substringAfterLast(' ').trim().toIntOrNull() ?: return@mapNotNull null
                name to slots
            }
            .sortedByDescending { it.second }
            .take(limit)
            .map { it.first }
    }.getOrDefault(emptyList())
}
