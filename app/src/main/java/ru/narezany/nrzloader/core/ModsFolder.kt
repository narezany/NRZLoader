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

    /** A button on a mod's card that opens something in a browser. */
    data class Link(val label: String, val url: String)

    /**
     * One thing a mod lets the user change.
     *
     * The launcher draws the control and writes the value; the mod reads it
     * back with nrz.readJson("config/<id>.json"). Nothing in it is interpreted
     * by the loader, which is what lets a mod add settings without the
     * launcher having to learn about them first.
     */
    data class Setting(
        val key: String,
        val label: String,
        val kind: Kind,
        val default: String,
        val options: List<String> = emptyList(),
        val min: Float = 0f,
        val max: Float = 1f,
    ) {
        enum class Kind { TOGGLE, SLIDER, CHOICE, TEXT }
    }

    data class Mod(
        val file: File,
        val kind: Kind,
        val id: String,
        val name: String,
        val version: String = "",
        val description: String = "",
        val author: String = "",
        val icon: File? = null,
        val minLoader: String = "",
        val maxLoader: String = "",
        val links: List<Link> = emptyList(),
        val settings: List<Setting> = emptyList(),
    ) {
        enum class Kind { NATIVE, SCRIPT, PACKAGE }

        val sizeBytes: Long
            get() = if (file.isDirectory) file.walkTopDown().filter { it.isFile }.sumOf { it.length() }
            else file.length()

        /** Whether the loader on this device is one the mod was written for. */
        val compatible: Boolean get() = LoaderVersion.satisfies(minLoader, maxLoader)

        /** What the mod asks for, shown when it is not what is installed. */
        val requirement: String
            get() = when {
                minLoader.isNotBlank() && maxLoader.isNotBlank() -> "$minLoader – $maxLoader"
                minLoader.isNotBlank() -> "$minLoader+"
                maxLoader.isNotBlank() -> "≤ $maxLoader"
                else -> ""
            }
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
            minLoader = json.optString("minLoader", ""),
            maxLoader = json.optString("maxLoader", ""),
            links = readLinks(json),
            settings = readSettings(json),
        )
    }

    /**
     * Buttons the mod wants on its card: its page, its chat, its issue
     * tracker. Only http links are kept — a card in a launcher is no place to
     * fire arbitrary intents from a file someone downloaded.
     */
    private fun readLinks(json: JSONObject): List<Link> {
        val array = json.optJSONArray("links") ?: return emptyList()

        return (0 until array.length()).mapNotNull { index ->
            val entry = array.optJSONObject(index) ?: return@mapNotNull null
            val url = entry.optString("url").trim()
            val label = entry.optString("label").ifBlank { url }

            if (!url.startsWith("https://") && !url.startsWith("http://")) return@mapNotNull null
            Link(label, url)
        }
    }

    private fun readSettings(json: JSONObject): List<Setting> {
        val array = json.optJSONArray("settings") ?: return emptyList()

        return (0 until array.length()).mapNotNull { index ->
            val entry = array.optJSONObject(index) ?: return@mapNotNull null
            val key = entry.optString("key").trim()
            if (key.isEmpty()) return@mapNotNull null

            val kind = when (entry.optString("type").lowercase()) {
                "toggle", "bool", "boolean" -> Setting.Kind.TOGGLE
                "slider", "number", "float" -> Setting.Kind.SLIDER
                "choice", "select", "enum" -> Setting.Kind.CHOICE
                else -> Setting.Kind.TEXT
            }

            val options = entry.optJSONArray("options")?.let { list ->
                (0 until list.length()).map { list.optString(it) }
            }.orEmpty()

            Setting(
                key = key,
                label = entry.optString("label").ifBlank { key },
                kind = kind,
                default = entry.opt("default")?.toString().orEmpty(),
                options = options,
                min = entry.optDouble("min", 0.0).toFloat(),
                max = entry.optDouble("max", 1.0).toFloat(),
            )
        }
    }

    /** Last few lines of the loader's log, which is where problems show up. */
    fun logTail(lines: Int = 200): List<String> {
        if (!log.isFile) return emptyList()
        return runCatching { log.readLines().takeLast(lines) }.getOrDefault(emptyList())
    }
}
