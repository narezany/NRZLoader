package ru.narezany.nrzloader.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import ru.narezany.nrzloader.R
import ru.narezany.nrzloader.core.GameLocator
import ru.narezany.nrzloader.core.LoaderFile
import ru.narezany.nrzloader.core.LoaderVersion
import ru.narezany.nrzloader.core.ModsFolder
import ru.narezany.nrzloader.core.PatchRunner
import ru.narezany.nrzloader.core.PatchService
import ru.narezany.nrzloader.core.PatchState
import java.io.File
import java.util.Locale

@Composable
fun HomeScreen(
    onGrantStorage: () -> Unit,
    onGrantGameStorage: (String) -> Unit,
    onGrantOverlay: (String) -> Unit,
    onUninstall: (String) -> Unit,
    onInstall: (File) -> Unit,
) {
    val context = LocalContext.current
    val stage by PatchState.state.collectAsState()

    var installs by remember { mutableStateOf<List<GameLocator.Installed>>(emptyList()) }
    var storageGranted by remember { mutableStateOf(MainActivity.hasStorageAccess()) }
    val ticks by rememberResumeTicker()

    // Everything here describes the device rather than this app, and the user
    // changes it from outside: installing, removing, granting access. So it is
    // read again every time the screen comes back.
    LaunchedEffect(stage, ticks) {
        installs = GameLocator.findAll(context)
        storageGranted = MainActivity.hasStorageAccess()

        // Once the rebuilt package is in place the build result is history;
        // leaving it on screen next to "already installed" reads as nonsense.
        if (installs.any { it.patched } && stage is PatchState.Stage.Done) PatchState.reset()
    }

    Column(
        Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            stringResource(R.string.loader_version, LoaderVersion.VALUE),
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        if (!storageGranted) {
            StatusCard(
                title = stringResource(R.string.storage_title),
                body = stringResource(R.string.storage_body),
                warning = true,
            ) {
                Button(onClick = onGrantStorage) { Text(stringResource(R.string.storage_action)) }
            }
        }

        if (installs.isEmpty()) {
            StatusCard(
                title = stringResource(R.string.game_not_found_title),
                body = stringResource(R.string.game_not_found_body),
                warning = true,
            )
        } else {
            installs.forEach { GameCard(it) }
        }

        val original = installs.firstOrNull { !it.patched }
        val patched = installs.firstOrNull { it.patched }

        when (val current = stage) {
            is PatchState.Stage.Running -> ProgressCard(current)

            is PatchState.Stage.Done -> StatusCard(
                title = stringResource(R.string.done_title),
                body = stringResource(R.string.done_body),
            ) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    // Ordered the way it has to happen: the installer refuses
                    // the rebuilt package while the original is still there.
                    if (original != null) {
                        Button(
                            onClick = { onUninstall(original.packageName) },
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Icon(Icons.Filled.Delete, contentDescription = null)
                            Spacer(Modifier.width(8.dp))
                            Text(stringResource(R.string.uninstall_action))
                        }
                        Text(
                            stringResource(R.string.uninstall_first_hint),
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }

                    FilledTonalButton(
                        onClick = { onInstall(File(current.apkPath)) },
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text(stringResource(R.string.install_action)) }
                }
            }

            is PatchState.Stage.Failed -> StatusCard(
                title = stringResource(R.string.failed_title),
                body = current.message + "\n\n"
                    + stringResource(R.string.failed_details, PatchRunner.errorLog.absolutePath),
                warning = true,
            ) {
                OutlinedButton(onClick = { PatchState.reset() }) {
                    Text(stringResource(R.string.ok_action))
                }
            }

            PatchState.Stage.Idle -> Unit
        }

        val source = original?.primaryApk
        if (stage !is PatchState.Stage.Running && stage !is PatchState.Stage.Done && source != null) {
            Button(
                onClick = { PatchService.start(context, source, "NRZLoader") },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Icon(Icons.Filled.PlayArrow, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.build_action))
            }
        }

        patched?.let { installed ->
            StatusCard(
                title = stringResource(R.string.game_storage_title),
                body = stringResource(R.string.game_storage_body),
            ) {
                FilledTonalButton(onClick = { onGrantGameStorage(installed.packageName) }) {
                    Text(stringResource(R.string.game_storage_action))
                }
            }

            if (installed.loaderVersion.isNotBlank()
                && LoaderVersion.compare(installed.loaderVersion, LoaderVersion.VALUE) < 0) {
                StatusCard(
                    title = stringResource(R.string.game_outdated_title),
                    body = stringResource(
                        R.string.game_outdated_body, installed.loaderVersion, LoaderVersion.VALUE),
                    warning = true,
                )
            }

            StatusCard(
                title = stringResource(R.string.overlay_title),
                body = stringResource(R.string.overlay_body),
            ) {
                FilledTonalButton(onClick = { onGrantOverlay(installed.packageName) }) {
                    Text(stringResource(R.string.overlay_action))
                }
            }

            StatusCard(
                title = stringResource(R.string.loader_hot_title),
                body = stringResource(R.string.loader_hot_body),
            )

            StatusCard(
                title = stringResource(R.string.loader_ready_title),
                body = stringResource(R.string.loader_ready_body, ModsFolder.mods.absolutePath),
            ) {
                OutlinedButton(onClick = {
                    GameLocator.launchIntent(context, installed.packageName)
                        ?.let(context::startActivity)
                }) { Text(stringResource(R.string.launch_action)) }
            }
        }
    }
}

@Composable
private fun ProgressCard(running: PatchState.Stage.Running) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(running.message, style = MaterialTheme.typography.bodyMedium)
            LinearProgressIndicator(
                progress = { running.percent / 100f },
                modifier = Modifier.fillMaxWidth(),
            )
            Text(
                stringResource(R.string.progress_hint, running.percent),
                style = MaterialTheme.typography.labelSmall,
            )
        }
    }
}

@Composable
private fun GameCard(install: GameLocator.Installed) {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (install.patched) MaterialTheme.colorScheme.primaryContainer
            else MaterialTheme.colorScheme.surfaceVariant,
        ),
    ) {
        Row(
            Modifier
                .padding(16.dp)
                .fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Icon(
                if (install.patched) Icons.Filled.CheckCircle else Icons.Filled.Folder,
                contentDescription = null,
            )
            Column {
                Text(install.label, style = MaterialTheme.typography.titleMedium)
                Text(
                    buildString {
                        append(install.versionName)
                        append(" · ")
                        append(formatSize(install.totalSizeBytes))
                        if (install.apkPaths.size > 1) {
                            append(" · ")
                            append(stringResource(R.string.game_parts, install.apkPaths.size))
                        }
                    },
                    style = MaterialTheme.typography.bodySmall,
                )
                if (install.patched) {
                    Text(
                        if (install.loaderVersion.isBlank()) {
                            stringResource(R.string.game_patched_badge)
                        } else {
                            stringResource(R.string.game_patched_with, install.loaderVersion)
                        },
                        style = MaterialTheme.typography.labelMedium,
                    )
                }
            }
        }
    }
}

@Composable
private fun StatusCard(
    title: String,
    body: String,
    warning: Boolean = false,
    action: @Composable (() -> Unit)? = null,
) {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (warning) MaterialTheme.colorScheme.errorContainer
            else MaterialTheme.colorScheme.secondaryContainer,
        ),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                if (warning) Icon(Icons.Filled.Warning, contentDescription = null)
                Text(title, style = MaterialTheme.typography.titleMedium)
            }
            Text(body, style = MaterialTheme.typography.bodyMedium)
            action?.invoke()
        }
    }
}

internal fun formatSize(bytes: Long): String = when {
    bytes >= 1L shl 30 -> String.format(Locale.US, "%.1f GB", bytes / (1L shl 30).toDouble())
    bytes >= 1L shl 20 -> String.format(Locale.US, "%.0f MB", bytes / (1L shl 20).toDouble())
    bytes >= 1L shl 10 -> String.format(Locale.US, "%.0f KB", bytes / (1L shl 10).toDouble())
    else -> "$bytes B"
}
