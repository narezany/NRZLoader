package ru.narezany.nrzloader.core

import org.json.JSONObject
import java.io.File

/**
 * The values a mod's own settings hold.
 *
 * One file per mod, in the config folder the mod already reads from, so a mod
 * picks its settings up with the file call it has anyway:
 * `nrz.readJson("config/<id>.json")`. The launcher only writes what the mod's
 * manifest declared; it never invents keys of its own.
 */
object ModSettings {
    private fun fileFor(modId: String): File = File(ModsFolder.config, "$modId.json")

    fun read(modId: String): Map<String, String> {
        val file = fileFor(modId)
        if (!file.isFile) return emptyMap()

        val json = runCatching { JSONObject(file.readText()) }.getOrNull() ?: return emptyMap()
        return json.keys().asSequence().associateWith { json.opt(it)?.toString().orEmpty() }
    }

    /** The value in force: what the user chose, or what the mod defaults to. */
    fun value(modId: String, setting: ModsFolder.Setting): String =
        read(modId)[setting.key] ?: setting.default

    fun write(modId: String, values: Map<String, String>, declared: List<ModsFolder.Setting>) {
        val json = JSONObject()

        for (setting in declared) {
            val raw = values[setting.key] ?: setting.default

            // Written with the type the mod asked for, so its javascript gets
            // a boolean or a number rather than a string that looks like one.
            when (setting.kind) {
                ModsFolder.Setting.Kind.TOGGLE -> json.put(setting.key, raw.toBoolean())
                ModsFolder.Setting.Kind.SLIDER ->
                    json.put(setting.key, raw.toFloatOrNull()?.toDouble() ?: 0.0)
                else -> json.put(setting.key, raw)
            }
        }

        runCatching {
            val file = fileFor(modId)
            file.parentFile?.mkdirs()
            file.writeText(json.toString(2))
        }
    }
}
