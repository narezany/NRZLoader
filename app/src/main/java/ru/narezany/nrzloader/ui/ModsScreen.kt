package ru.narezany.nrzloader.ui

import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Code
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Inventory2
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.OpenInNew
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import ru.narezany.nrzloader.R
import ru.narezany.nrzloader.core.DisabledMods
import ru.narezany.nrzloader.core.LoaderVersion
import ru.narezany.nrzloader.core.ModsFolder

@Composable
fun ModsScreen() {
    var mods by remember { mutableStateOf<List<ModsFolder.Mod>>(emptyList()) }
    var disabled by remember { mutableStateOf<Set<String>>(emptySet()) }
    var refreshes by remember { mutableStateOf(0) }
    var settingsFor by remember { mutableStateOf<ModsFolder.Mod?>(null) }
    val ticks by rememberResumeTicker()

    // A mod arrives by tapping a file outside this app, so the folder is read
    // again on every return rather than once.
    LaunchedEffect(refreshes, ticks) {
        ModsFolder.ensure()
        mods = ModsFolder.list()
        disabled = DisabledMods.ids()
    }

    settingsFor?.let { mod ->
        ModSettingsDialog(mod, onDismiss = { settingsFor = null })
    }

    LazyColumn(
        Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(
                        stringResource(R.string.mods_folder_title),
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Text(ModsFolder.mods.absolutePath, style = MaterialTheme.typography.bodySmall)
                    Text(
                        stringResource(R.string.mods_folder_hint),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
        }

        if (mods.isEmpty()) {
            item {
                Text(
                    stringResource(R.string.mods_empty),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        items(mods, key = { it.file.absolutePath }) { mod ->
            ModCard(
                mod = mod,
                enabled = mod.id !in disabled,
                onToggle = { on ->
                    DisabledMods.set(mod.id, on)
                    disabled = DisabledMods.ids()
                },
                onSettings = { settingsFor = mod },
                onRemove = {
                    if (mod.file.isDirectory) mod.file.deleteRecursively() else mod.file.delete()
                    refreshes++
                },
            )
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ModCard(
    mod: ModsFolder.Mod,
    enabled: Boolean,
    onToggle: (Boolean) -> Unit,
    onSettings: () -> Unit,
    onRemove: () -> Unit,
) {
    val context = LocalContext.current

    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            // A mod switched off stays on the list but stops looking active.
            containerColor = if (enabled) MaterialTheme.colorScheme.surfaceVariant
            else MaterialTheme.colorScheme.surface,
        ),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(
                Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                // A packaged mod brings its own icon; a loose file gets one
                // that says what kind of file it is.
                val icon = mod.icon?.let {
                    runCatching { BitmapFactory.decodeFile(it.absolutePath) }.getOrNull()
                }
                if (icon != null) {
                    Image(
                        bitmap = icon.asImageBitmap(),
                        contentDescription = null,
                        modifier = Modifier.size(40.dp),
                    )
                } else {
                    Icon(
                        when (mod.kind) {
                            ModsFolder.Mod.Kind.NATIVE -> Icons.Filled.Memory
                            ModsFolder.Mod.Kind.SCRIPT -> Icons.Filled.Code
                            ModsFolder.Mod.Kind.PACKAGE -> Icons.Filled.Inventory2
                        },
                        contentDescription = null,
                    )
                }

                Column(Modifier.weight(1f)) {
                    Text(mod.name, style = MaterialTheme.typography.titleSmall)
                    Text(
                        buildString {
                            append(
                                when (mod.kind) {
                                    ModsFolder.Mod.Kind.NATIVE ->
                                        stringResource(R.string.mod_kind_native)
                                    ModsFolder.Mod.Kind.SCRIPT ->
                                        stringResource(R.string.mod_kind_script)
                                    ModsFolder.Mod.Kind.PACKAGE ->
                                        stringResource(R.string.mod_kind_package)
                                }
                            )
                            if (mod.version.isNotBlank()) {
                                append(" ")
                                append(mod.version)
                            }
                            append(" · ")
                            append(formatSize(mod.sizeBytes))
                        },
                        style = MaterialTheme.typography.bodySmall,
                    )
                }

                // Switching a mod off leaves it exactly where it is, so it is
                // safe to try and safe to undo.
                Switch(checked = enabled, onCheckedChange = onToggle)
            }

            if (!mod.compatible) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Icon(
                        Icons.Filled.Warning,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.error,
                    )
                    Text(
                        stringResource(
                            R.string.mod_needs_loader, mod.requirement, LoaderVersion.VALUE),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }

            if (mod.description.isNotBlank()) {
                Text(mod.description, style = MaterialTheme.typography.bodySmall)
            }
            if (mod.author.isNotBlank()) {
                Text(
                    stringResource(R.string.mod_by, mod.author),
                    style = MaterialTheme.typography.labelSmall,
                )
            }

            // The mod's own buttons: where it lives, where to ask about it,
            // and its settings when it declared any.
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (mod.settings.isNotEmpty()) {
                    AssistChip(
                        onClick = onSettings,
                        label = { Text(stringResource(R.string.mod_settings)) },
                        leadingIcon = { Icon(Icons.Filled.Tune, contentDescription = null) },
                    )
                }

                mod.links.forEach { link ->
                    AssistChip(
                        onClick = {
                            runCatching {
                                context.startActivity(
                                    Intent(Intent.ACTION_VIEW, Uri.parse(link.url))
                                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                                )
                            }
                        },
                        label = { Text(link.label) },
                        leadingIcon = { Icon(Icons.Filled.OpenInNew, contentDescription = null) },
                    )
                }

                TextButton(onClick = onRemove) {
                    Icon(Icons.Filled.Delete, contentDescription = null)
                    Spacer(Modifier.width(4.dp))
                    Text(stringResource(R.string.mod_remove))
                }
            }
        }
    }
}
