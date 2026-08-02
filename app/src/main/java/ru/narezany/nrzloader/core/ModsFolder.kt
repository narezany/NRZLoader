package ru.narezany.nrzloader.core

import android.os.Environment
import org.json.JSONObject
import java.io.File

/**
 * The folder the loader reads mods from, in the root of shared storage so a
 * plain file manager can reach it.
 */
object ModsFolder {
    const val ROOT_NAME = "NRZLoader"

    val root: File get() = File(Environment.getExternalStorageDirectory(), ROOT_NAME)
    val mods: File get() = File(root, "mods")
    val config: File get() = File(root, "config")
    val log: File get() = File(root, "loader.log")

    fun ensure(): Boolean = mods.mkdirs() || mods.isDirectory

    data class Mod(
        val file: File,
        val kind: Kind,
        val id: String,
        val name: String,
        val version: String = "",
        val description: String = "",
        val author: String = "",
        val icon: File? = null,
    ) {
        enum class Kind { NATIVE, SCRIPT, PACKAGE }

        val sizeBytes: Long
            get() = if (file.isDirectory) file.walkTopDown().filter { it.isFile }.sumOf { it.length() }
            else file.length()
    }

    fun list(): List<Mod> {
        val entries = mods.listFiles().orEmpty()

        return entries.mapNotNull { file ->
            when {
                // A packaged mod is a directory holding its own description.
                file.isDirectory -> readPackage(file)
                file.name.endsWith(".so") -> Mod(file, Mod.Kind.NATIVE, file.name, file.name)
                file.name.endsWith(".js") -> Mod(file, Mod.Kind.SCRIPT, file.name, file.name)
                else -> null
            }
        }.sortedBy { it.name.lowercase() }
    }

    private fun readPackage(directory: File): Mod? {
        val manifest = File(directory, "mod.json")
        if (!manifest.isFile) return null

        val json = runCatching { JSONObject(manifest.readText()) }.getOrNull() ?: return null
        val id = json.optString("id").ifBlank { directory.name }
        val icon = File(directory, "icon.png").takeIf { it.isFile }

        return Mod(
            file = directory,
            kind = Mod.Kind.PACKAGE,
            id = id,
            name = json.optString("name").ifBlank { id },
            version = json.optString("version", ""),
            description = json.optString("description", ""),
            author = json.optString("author", ""),
            icon = icon,
        )
    }

    /** Last few lines of the loader's log, which is where problems show up. */
    fun logTail(lines: Int = 200): List<String> {
        if (!log.isFile) return emptyList()
        return runCatching { log.readLines().takeLast(lines) }.getOrDefault(emptyList())
    }
}
