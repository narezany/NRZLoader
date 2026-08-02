package ru.narezany.nrzloader.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import ru.narezany.nrzloader.core.AppLocale
import ru.narezany.nrzloader.core.LoaderFile
import java.io.File

class MainActivity : ComponentActivity() {
    /** Applies the chosen language before any text is looked up. */
    override fun attachBaseContext(base: android.content.Context) {
        super.attachBaseContext(AppLocale.wrap(base))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        askForNotifications()

        // Свежий загрузчик кладётся рядом с модами при каждом запуске: игра
        // предпочтёт его тому, что вшит внутрь неё, и обновление лаунчера
        // обойдётся без пересборки игры.
        if (MainActivity.hasStorageAccess()) LoaderFile.refresh(this)
        setContent {
            NrzTheme {
                NrzApp(
                    onGrantStorage = ::openStorageSettings,
                    onGrantGameStorage = ::openGameStorageSettings,
                    onGrantOverlay = ::openOverlaySettings,
                    onUninstall = ::uninstallPackage,
                    onInstall = ::installPackage,
                    onLanguageChanged = ::recreate,
                )
            }
        }
    }

    /**
     * The progress notification is what makes leaving the app safe, so the
     * permission is asked for up front rather than when the work starts.
     */
    private fun askForNotifications() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        val granted = ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
        if (granted == PackageManager.PERMISSION_GRANTED) return

        // The plain request avoids registering a result contract at a point in
        // the lifecycle where launching one is not yet allowed.
        ActivityCompat.requestPermissions(
            this, arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATIONS)
    }

    /**
     * The patched package lives in this app's own directory, so the installer
     * reaches it through a shared uri rather than a raw path.
     */
    private fun installPackage(apk: File) {
        val uri: Uri = FileProvider.getUriForFile(this, "$packageName.files", apk)
        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        startActivity(intent)
    }

    /**
     * The rebuilt package carries a different signature, so the original has to
     * go first or the installer only reports a damaged package.
     */
    private fun uninstallPackage(packageName: String) {
        val intent = Intent(Intent.ACTION_DELETE).apply {
            data = Uri.parse("package:$packageName")
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        startActivity(intent)
    }

    private fun openStorageSettings() = openAllFilesSettings(packageName)

    /**
     * The game asks for file access on its own only long after startup, so the
     * launcher offers to open the same screen for it right away.
     */
    private fun openGameStorageSettings(gamePackage: String) = openAllFilesSettings(gamePackage)

    /**
     * Mods draw their own windows through the system rather than through the
     * game's interface, and that needs a permission only the user can give.
     */
    private fun openOverlaySettings(gamePackage: String) {
        val intent = Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION).apply {
            data = Uri.parse("package:$gamePackage")
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        runCatching { startActivity(intent) }.onFailure {
            startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION))
        }
    }

    private fun openAllFilesSettings(target: String) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
            data = Uri.parse("package:$target")
        }
        runCatching { startActivity(intent) }.onFailure {
            startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
        }
    }

    companion object {
        private const val REQUEST_NOTIFICATIONS = 1001

        fun hasStorageAccess(): Boolean =
            Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()
    }
}
