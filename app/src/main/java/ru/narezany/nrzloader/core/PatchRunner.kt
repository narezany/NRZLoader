package ru.narezany.nrzloader.core

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import ru.narezany.nrzloader.patch.ApkPatcher
import java.io.File

/**
 * Runs the patch off the main thread and reports progress back.
 *
 * The heavy part is copying several hundred megabytes, so the caller gets
 * steady updates rather than a frozen screen.
 */
object PatchRunner {
    private const val LOADER_ASSET = "libnrzloader.so"
    private const val BOOTSTRAP_ASSET = "nrz_bootstrap.dex"
    private const val KEYSTORE_ASSET = "nrzloader.p12"
    private const val KEYSTORE_PASSWORD = "nrzloader"

        /** Where the details of a failed run are kept. */
    val errorLog: File get() = File(ModsFolder.root, "patch-error.log")

    private fun recordFailure(error: Throwable) {
        runCatching {
            ModsFolder.root.mkdirs()
            errorLog.writeText(buildString {
                appendLine("NRZLoader patch failure")
                appendLine(java.util.Date().toString())
                appendLine()
                appendLine(error.stackTraceToString())
            })
        }
    }

    sealed interface Outcome {
        data class Success(val apk: File, val dexName: String) : Outcome
        data class Failure(val message: String) : Outcome
    }

    suspend fun run(
        context: Context,
        sourceApk: String,
        appLabel: String,
        onProgress: (String, Int) -> Unit,
    ): Outcome = withContext(Dispatchers.IO) {
        try {
            val options = ApkPatcher.Options().apply {
                loaderLibrary = context.assets.open(LOADER_ASSET).use { it.readBytes() }
                bootstrapDex = context.assets.open(BOOTSTRAP_ASSET).use { it.readBytes() }
                applicationLabel = appLabel
                loaderVersion = LoaderVersion.VALUE
                requestAllFiles = true
            }
            context.assets.open(KEYSTORE_ASSET).use {
                ApkPatcher.loadSigningKey(it, KEYSTORE_PASSWORD, options)
            }

            // Written to the app's own external directory: large, and reachable
            // by the installer through the file provider.
            val destination = File(context.getExternalFilesDir(null), "patched")
            destination.mkdirs()
            val output = File(destination, "minecraft-nrzloader.apk")
            if (output.exists()) output.delete()

            val result = ApkPatcher.patch(File(sourceApk), output, options) { message, percent ->
                onProgress(message, percent)
            }

            ModsFolder.ensure()
            Outcome.Success(result.output, result.dexName)
        } catch (error: Throwable) {
            // Written out in full: a message on screen is easy to lose and hard
            // to retype, and the stack is what actually says where it broke.
            recordFailure(error)
            Outcome.Failure(error.message ?: error.toString())
        }
    }
}
