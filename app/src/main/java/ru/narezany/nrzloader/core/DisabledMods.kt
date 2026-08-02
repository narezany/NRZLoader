package ru.narezany.nrzloader.core

import java.io.File

/**
 * The mods the user switched off.
 *
 * Kept as a list of ids in a plain file rather than by moving or renaming
 * anything: switching a mod off should not touch the mod, so switching it
 * back on cannot fail. The loader reads the same file when the game starts.
 */
object DisabledMods {
    private val file: File get() = File(ModsFolder.config, "disabled.txt")

    fun ids(): Set<String> {
        if (!file.isFile) return emptySet()

        return runCatching {
            file.readLines()
                .map { it.substringBefore('#').trim() }
                .filter { it.isNotEmpty() }
                .toSet()
        }.getOrDefault(emptySet())
    }

    fun contains(id: String): Boolean = id in ids()

    fun set(id: String, enabled: Boolean) {
        val current = ids().toMutableSet()
        if (enabled) current.remove(id) else current.add(id)
        write(current)
    }

    private fun write(ids: Set<String>) {
        runCatching {
            file.parentFile?.mkdirs()
            file.writeText(
                buildString {
                    appendLine("# Mods switched off in the launcher. One id per line.")
                    appendLine("# Delete a line to switch that mod back on.")
                    ids.sorted().forEach { appendLine(it) }
                }
            )
        }
    }
}
