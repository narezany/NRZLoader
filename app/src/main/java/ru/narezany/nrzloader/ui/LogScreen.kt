package ru.narezany.nrzloader.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Card
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import ru.narezany.nrzloader.R
import ru.narezany.nrzloader.core.ModsFolder

/**
 * The loader writes its log to a plain file, so it can be read here without a
 * computer or developer tools.
 */
@Composable
fun LogScreen() {
    var lines by remember { mutableStateOf<List<String>>(emptyList()) }
    var refreshes by remember { mutableStateOf(0) }
    val ticks by rememberResumeTicker()

    // The game writes this while the launcher is in the background, so coming
    // back should show what it wrote.
    LaunchedEffect(refreshes, ticks) {
        lines = ModsFolder.logTail()
    }

    Column(
        Modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        FilledTonalButton(onClick = { refreshes++ }) {
            Text(stringResource(R.string.log_refresh))
        }

        if (lines.isEmpty()) {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(
                        stringResource(R.string.log_empty_title),
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Text(
                        stringResource(R.string.log_empty_body, ModsFolder.log.absolutePath),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
        }

        LazyColumn(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            items(lines) { line ->
                Text(
                    line,
                    style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                )
            }
        }
    }
}
