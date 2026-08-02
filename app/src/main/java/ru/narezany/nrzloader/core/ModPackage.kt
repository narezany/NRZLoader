package ru.narezany.nrzloader.core

import android.content.Context
import android.graphics.BitmapFactory
import android.net.Uri
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.util.zip.ZipInputStream

/**
 * A packaged mod: a zip renamed to .nrzmod.
 *
 * A single loose script says nothing about itself. A package carries a name,
 * a description and an icon alongside its code, which is what makes a list of
 * installed mods readable.
 *
 *   mod.json    required: id, name; optional: version, description, author
 *   icon.png    optional
 *   *.js, *.so  the mod itself
 */
object ModPackage {
    const val EXTENSION = "nrzmod"
    private const val MANIFEST = "mod.json"
    private const val ICON = "icon.png"

    /** Anything larger is refused rather than unpacked into the user's storage. */
    private const val MAX_ENTRY_BYTES = 64L * 1024 * 1024
    private const val MAX_TOTAL_BYTES = 256L * 1024 * 1024

    data class Info(
        val id: String,
        val name: String,
        val version: String,
        val description: String,
        val author: String,
        val iconBytes: ByteArray?,
        val payloadNames: List<String>,
    ) {
        val hasIcon: Boolean get() = iconBytes != null

        // Data classes with an array member need these written out.
        override fun equals(other: Any?): Boolean = this === other || (other is Info && other.id == id)
        override fun hashCode(): Int = id.hashCode()
    }

    class InvalidPackage(message: String) : Exception(message)

    fun read(context: Context, uri: Uri): Info =
        context.contentResolver.openInputStream(uri)?.use { readFrom(it) }
            ?: throw InvalidPackage("cannot open the file")

    /** Reads the description without unpacking anything to disk. */
    fun readFrom(stream: InputStream): Info {
        var manifest: JSONObject? = null
        var icon: ByteArray? = null
        val payload = mutableListOf<String>()

        ZipInputStream(stream).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                if (entry.isDirectory) continue
                val name = entry.name.substringAfterLast('/')

                when {
                    entry.name == MANIFEST || name == MANIFEST ->
                        manifest = JSONObject(zip.readBounded().decodeToString())
                    name == ICON -> icon = zip.readBounded()
                    name.endsWith(".js") || name.endsWith(".so") -> payload.add(name)
                }
            }
        }

        val json = manifest ?: throw InvalidPackage("no $MANIFEST inside")
        val id = json.optString("id").ifBlank { throw InvalidPackage("$MANIFEST has no id") }
        if (!id.matches(Regex("[A-Za-z0-9._-]{1,64}"))) {
            throw InvalidPackage("id may only contain letters, digits, dot, dash and underscore")
        }
        if (payload.isEmpty()) throw InvalidPackage("no .js or .so inside")

        return Info(
            id = id,
            name = json.optString("name").ifBlank { id },
            version = json.optString("version", "1.0"),
            description = json.optString("description", ""),
            author = json.optString("author", ""),
            iconBytes = icon,
            payloadNames = payload,
        )
    }

    fun decodeIcon(info: Info) = info.iconBytes?.let {
        runCatching { BitmapFactory.decodeByteArray(it, 0, it.size) }.getOrNull()
    }

    /**
     * Unpacks into its own directory under the mods folder, replacing whatever
     * was there under the same id.
     */
    fun install(context: Context, uri: Uri, info: Info): File {
        ModsFolder.ensure()
        val target = File(ModsFolder.mods, info.id)
        if (target.exists()) target.deleteRecursively()
        if (!target.mkdirs()) throw InvalidPackage("cannot create ${target.absolutePath}")

        var total = 0L
        context.contentResolver.openInputStream(uri)?.use { source ->
            ZipInputStream(source).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    if (entry.isDirectory) continue

                    // Flattened on purpose: a name like ../../etc must not be
                    // able to write outside the mod's own directory.
                    val name = entry.name.substringAfterLast('/')
                    if (name.isEmpty() || name == "." || name == "..") continue

                    val bytes = zip.readBounded()
                    total += bytes.size
                    if (total > MAX_TOTAL_BYTES) throw InvalidPackage("package is too large")

                    File(target, name).writeBytes(bytes)
                }
            }
        } ?: throw InvalidPackage("cannot open the file")

        return target
    }

    fun uninstall(id: String): Boolean {
        val target = File(ModsFolder.mods, id)
        return target.exists() && target.deleteRecursively()
    }

    private fun InputStream.readBounded(): ByteArray {
        val buffer = java.io.ByteArrayOutputStream()
        val chunk = ByteArray(65536)
        var total = 0L
        while (true) {
            val read = read(chunk)
            if (read <= 0) break
            total += read
            if (total > MAX_ENTRY_BYTES) throw InvalidPackage("a file inside the package is too large")
            buffer.write(chunk, 0, read)
        }
        return buffer.toByteArray()
    }
}
