package ru.narezany.nrzloader.ui

import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.size
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import ru.narezany.nrzloader.R
import ru.narezany.nrzloader.core.AppLocale
import ru.narezany.nrzloader.core.ModPackage
import ru.narezany.nrzloader.core.ModsFolder
import java.io.File

/**
 * Opens when a .nrzmod file is tapped anywhere on the device.
 *
 * The file is described before anything is written: the point of packaging a
 * mod is that the user can see what they are about to install.
 */
class ModInstallActivity : ComponentActivity() {
    /** The same language the rest of the launcher is shown in. */
    override fun attachBaseContext(base: android.content.Context) {
        super.attachBaseContext(AppLocale.wrap(base))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val uri = intent?.data
        if (uri == null) {
            finish()
            return
        }

        val info = runCatching { ModPackage.read(this, uri) }.getOrElse { error ->
            Toast.makeText(
                this,
                getString(R.string.mod_install_failed, error.message ?: error.toString()),
                Toast.LENGTH_LONG,
            ).show()
            finish()
            return
        }

        setContent {
            NrzTheme {
                InstallDialog(
                    info = info,
                    alreadyInstalled = File(ModsFolder.mods, info.id).exists(),
                    onConfirm = { install(uri, info) },
                    onDismiss = { finish() },
                )
            }
        }
    }

    private fun install(uri: Uri, info: ModPackage.Info) {
        val message = runCatching {
            ModPackage.install(this, uri, info)
            getString(R.string.mod_installed)
        }.getOrElse { error ->
            getString(R.string.mod_install_failed, error.message ?: error.toString())
        }

        Toast.makeText(this, message, Toast.LENGTH_LONG).show()
        finish()
    }
}

@Composable
private fun InstallDialog(
    info: ModPackage.Info,
    alreadyInstalled: Boolean,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
) {
    val icon = ModPackage.decodeIcon(info)

    AlertDialog(
        onDismissRequest = onDismiss,
        icon = icon?.let {
            {
                Image(
                    bitmap = it.asImageBitmap(),
                    contentDescription = null,
                    modifier = Modifier.size(48.dp),
                )
            }
        },
        title = { Text(stringResource(R.string.mod_install_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text(info.name, style = MaterialTheme.typography.titleMedium)
                    if (info.version.isNotBlank()) {
                        Text(info.version, style = MaterialTheme.typography.labelMedium)
                    }
                }
                if (info.author.isNotBlank()) {
                    Text(
                        stringResource(R.string.mod_by, info.author),
                        style = MaterialTheme.typography.labelMedium,
                    )
                }
                if (info.description.isNotBlank()) {
                    Text(info.description, style = MaterialTheme.typography.bodyMedium)
                }
                if (alreadyInstalled) {
                    Text(
                        stringResource(R.string.mod_install_replace),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onConfirm) { Text(stringResource(R.string.mod_install_confirm)) }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.mod_install_cancel)) }
        },
    )
}
