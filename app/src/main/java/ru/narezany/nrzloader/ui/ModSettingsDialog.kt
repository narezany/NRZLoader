package ru.narezany.nrzloader.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import ru.narezany.nrzloader.R
import ru.narezany.nrzloader.core.ModSettings
import ru.narezany.nrzloader.core.ModsFolder
import java.util.Locale

/**
 * The settings a mod declared, drawn from its manifest.
 *
 * The launcher knows four kinds of control and nothing about what any of them
 * mean. That is the point: a mod adds a setting by describing it, not by
 * waiting for the launcher to grow support for it.
 */
@Composable
fun ModSettingsDialog(mod: ModsFolder.Mod, onDismiss: () -> Unit) {
    val values = remember(mod.id) {
        mutableStateMapOf<String, String>().apply {
            mod.settings.forEach { put(it.key, ModSettings.value(mod.id, it)) }
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(mod.name) },
        text = {
            Column(
                Modifier
                    .heightIn(max = 420.dp)
                    .verticalScroll(rememberScrollState())
                    .selectableGroup(),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                mod.settings.forEach { setting ->
                    SettingRow(
                        setting = setting,
                        value = values[setting.key].orEmpty(),
                        onChange = { values[setting.key] = it },
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                ModSettings.write(mod.id, values.toMap(), mod.settings)
                onDismiss()
            }) { Text(stringResource(R.string.mod_settings_save)) }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.mod_install_cancel)) }
        },
    )
}

@Composable
private fun SettingRow(
    setting: ModsFolder.Setting,
    value: String,
    onChange: (String) -> Unit,
) {
    when (setting.kind) {
        ModsFolder.Setting.Kind.TOGGLE -> Row(
            Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(setting.label, style = MaterialTheme.typography.bodyLarge)
            Switch(
                checked = value.toBoolean(),
                onCheckedChange = { onChange(it.toString()) },
            )
        }

        ModsFolder.Setting.Kind.SLIDER -> Column {
            val current = value.toFloatOrNull() ?: setting.min
            Text(
                "${setting.label}: ${String.format(Locale.US, "%.2f", current)}",
                style = MaterialTheme.typography.bodyLarge,
            )
            Slider(
                value = current.coerceIn(setting.min, setting.max),
                onValueChange = { onChange(it.toString()) },
                valueRange = setting.min..setting.max,
            )
        }

        ModsFolder.Setting.Kind.CHOICE -> Column(
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Text(setting.label, style = MaterialTheme.typography.bodyLarge)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                setting.options.forEach { option ->
                    FilterChip(
                        selected = option == value,
                        onClick = { onChange(option) },
                        label = { Text(option) },
                    )
                }
            }
        }

        ModsFolder.Setting.Kind.TEXT -> OutlinedTextField(
            value = value,
            onValueChange = onChange,
            label = { Text(setting.label) },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
    }
}
