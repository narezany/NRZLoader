package ru.narezany.nrzloader.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import ru.narezany.nrzloader.BuildConfig
import ru.narezany.nrzloader.R
import ru.narezany.nrzloader.core.AppLocale
import ru.narezany.nrzloader.core.GameLocator
import ru.narezany.nrzloader.core.ModsFolder
import ru.narezany.nrzloader.core.Diagnostics
import ru.narezany.nrzloader.core.ProbeSettings
import ru.narezany.nrzloader.core.RttiProbe
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun SettingsScreen(onLanguageChanged: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var selected by remember { mutableStateOf(AppLocale.current(context)) }
    var probing by remember { mutableStateOf(false) }
    var probeResult by remember { mutableStateOf<String?>(null) }
    var checks by remember { mutableStateOf<List<Diagnostics.Check>>(emptyList()) }
    var probeClass by remember { mutableStateOf("") }
    val ticks by rememberResumeTicker()

    // Всё, что здесь проверяется, живёт вне приложения, поэтому перечитывается
    // при каждом возврате, а не один раз.
    LaunchedEffect(ticks, probeResult) {
        checks = Diagnostics.run(context)
        probeClass = ProbeSettings.current()
    }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(vertical = 8.dp)) {
                Text(
                    stringResource(R.string.settings_language),
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                )

                AppLocale.supported.forEach { tag ->
                    LanguageRow(
                        label = if (tag == AppLocale.SYSTEM) {
                            stringResource(R.string.settings_language_system)
                        } else {
                            AppLocale.displayName(tag)
                        },
                        selected = tag == selected,
                        onSelect = {
                            if (tag != selected) {
                                selected = tag
                                AppLocale.set(context, tag)
                                // Resource lookup happens when a screen is
                                // created, so the whole activity is rebuilt
                                // instead of only the text being swapped.
                                onLanguageChanged()
                            }
                        },
                    )
                }
            }
        }

        // Разбор игры на предмет уцелевших имён классов. Это ответ на вопрос,
        // можно ли добраться до геймплейных функций, и стоит он одной минуты.
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    stringResource(R.string.probe_title),
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    stringResource(R.string.probe_body),
                    style = MaterialTheme.typography.bodyMedium,
                )

                if (probing) {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                    Text(
                        stringResource(R.string.probe_running),
                        style = MaterialTheme.typography.bodySmall,
                    )
                } else {
                    FilledTonalButton(onClick = {
                        probing = true
                        probeResult = null
                        scope.launch {
                            val text = withContext(Dispatchers.IO) {
                                runCatching {
                                    val game = GameLocator.findAll(context).firstOrNull()
                                        ?: return@runCatching null
                                    val result = RttiProbe.run(game)
                                    val report = RttiProbe.writeReport(result, game)
                                    RttiProbe.writeTable(result)
                                    buildString {
                                        appendLine(
                                            when (result.verdict) {
                                                RttiProbe.Result.Verdict.PRESENT ->
                                                    "RTTI на месте."
                                                RttiProbe.Result.Verdict.PARTIAL ->
                                                    "RTTI похоже вырезан."
                                                RttiProbe.Result.Verdict.ABSENT ->
                                                    "RTTI вырезан."
                                            }
                                        )
                                        appendLine("имён классов: ${result.classNames}")
                                        appendLine(
                                            "из ожидаемых: ${result.found.size} " +
                                                "(${result.found.take(8).joinToString(", ")})"
                                        )
                                        val tables = result.vtables
                                        if (tables != null && tables.found.isNotEmpty()) {
                                            appendLine("таблиц найдено: ${tables.found.size}")
                                        } else if (tables != null) {
                                            appendLine("таблиц нет: ${tables.note}")
                                        }
                                        report?.let { appendLine(it.absolutePath) }
                                    }
                                }.getOrElse { "не вышло: ${it.message}" }
                            }
                            probeResult = text ?: "игра не найдена"
                            probing = false
                        }
                    }) { Text(stringResource(R.string.probe_action)) }
                }

                probeResult?.let {
                    Text(
                        it,
                        style = MaterialTheme.typography.bodySmall.copy(
                            fontFamily = FontFamily.Monospace),
                    )
                }
            }
        }

        // Проверка всей цепочки: где она рвётся, снаружи не видно — файл
        // просто не обновляется, а причин может быть шесть.
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(
                    stringResource(R.string.diag_title),
                    style = MaterialTheme.typography.titleMedium,
                )

                checks.forEach { check ->
                    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text(
                            when {
                                check.ok -> "✓  "
                                check.waiting -> "·  "
                                else -> "✗  "
                            } + check.title,
                            style = MaterialTheme.typography.bodyMedium,
                            color = when {
                                check.ok -> MaterialTheme.colorScheme.onSurface
                                check.waiting -> MaterialTheme.colorScheme.onSurfaceVariant
                                else -> MaterialTheme.colorScheme.error
                            },
                        )
                        Text(
                            "     " + check.detail,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        if (!check.ok && !check.waiting && check.fix.isNotBlank()) {
                            Text(
                                "     → " + check.fix,
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.error,
                            )
                        }
                    }
                }

                OutlinedButton(onClick = { checks = Diagnostics.run(context) }) {
                    Text(stringResource(R.string.diag_again))
                }

                HorizontalDivider()

                Text(
                    stringResource(R.string.diag_probe_class),
                    style = MaterialTheme.typography.titleSmall,
                )
                Text(
                    stringResource(R.string.diag_probe_hint),
                    style = MaterialTheme.typography.bodySmall,
                )

                Row(
                    Modifier.horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    val options = listOf("") + ProbeSettings.candidates()
                    options.forEach { name ->
                        FilterChip(
                            selected = name == probeClass,
                            onClick = {
                                ProbeSettings.set(name)
                                probeClass = name
                                checks = Diagnostics.run(context)
                            },
                            label = {
                                Text(
                                    if (name.isBlank()) stringResource(R.string.diag_probe_off)
                                    else name
                                )
                            },
                        )
                    }
                }
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    stringResource(R.string.settings_about),
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    stringResource(R.string.settings_version, BuildConfig.VERSION_NAME),
                    style = MaterialTheme.typography.bodyMedium,
                )
                HorizontalDivider(Modifier.padding(vertical = 4.dp))
                Text(
                    stringResource(R.string.settings_folder),
                    style = MaterialTheme.typography.bodyMedium,
                )
                Text(
                    ModsFolder.root.absolutePath,
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}

@Composable
private fun LanguageRow(label: String, selected: Boolean, onSelect: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(onClick = onSelect)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        RadioButton(selected = selected, onClick = onSelect)
        Text(label, style = MaterialTheme.typography.bodyLarge)
    }
}
