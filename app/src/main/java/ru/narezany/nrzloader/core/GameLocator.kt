package ru.narezany.nrzloader.core

import android.content.Context
import android.content.pm.PackageManager
import java.io.File

/**
 * Finds the copy of the game already on the device.
 *
 * Nothing is downloaded: the patcher works from the package the user installed
 * themselves, which is also why a missing install is reported rather than
 * worked around.
 */
object GameLocator {
    private val CANDIDATE_PACKAGES = listOf(
        "com.mojang.minecraftpe",
        "com.mojang.minecraftpe.preview",
    )

    data class Installed(
        val packageName: String,
        val label: String,
        val versionName: String,
        val apkPaths: List<String>,
        val patched: Boolean,
    ) {
        /** The package carrying the 64-bit native library, which is the one to patch. */
        val primaryApk: String? get() = apkPaths.firstOrNull { hasArm64Library(it) } ?: apkPaths.firstOrNull()

        val totalSizeBytes: Long get() = apkPaths.sumOf { runCatching { File(it).length() }.getOrDefault(0L) }
    }

    fun findAll(context: Context): List<Installed> {
        val packages = context.packageManager
        return CANDIDATE_PACKAGES.mapNotNull { name ->
            runCatching {
                val info = packages.getPackageInfo(name, 0)
                val application = info.applicationInfo ?: return@runCatching null

                val paths = buildList {
                    application.sourceDir?.let { add(it) }
                    application.splitSourceDirs?.let { addAll(it) }
                }
                if (paths.isEmpty()) return@runCatching null

                Installed(
                    packageName = name,
                    label = packages.getApplicationLabel(application).toString(),
                    versionName = info.versionName ?: "unknown",
                    apkPaths = paths,
                    // A patched install carries the loader; spotting it keeps the
                    // app from offering to patch something already patched.
                    patched = paths.any { hasLoader(it) },
                )
            }.getOrNull()
        }
    }

    /** True when the package already contains the loader library. */
    private fun hasLoader(apkPath: String): Boolean = containsEntry(apkPath, "lib/arm64-v8a/libnrzloader.so")

    private fun hasArm64Library(apkPath: String): Boolean =
        containsEntry(apkPath, "lib/arm64-v8a/libminecraftpe.so")

    private fun containsEntry(apkPath: String, entry: String): Boolean = runCatching {
        java.util.zip.ZipFile(apkPath).use { it.getEntry(entry) != null }
    }.getOrDefault(false)

    fun isInstalled(context: Context, packageName: String): Boolean = runCatching {
        context.packageManager.getPackageInfo(packageName, 0)
        true
    }.getOrDefault(false)

    fun launchIntent(context: Context, packageName: String) =
        context.packageManager.getLaunchIntentForPackage(packageName)

    @Suppress("unused")
    private fun unusedFlag() = PackageManager.GET_META_DATA
}
