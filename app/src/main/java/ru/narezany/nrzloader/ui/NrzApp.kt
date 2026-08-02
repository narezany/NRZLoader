package ru.narezany.nrzloader.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Article
import androidx.compose.material.icons.filled.Extension
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import ru.narezany.nrzloader.R
import java.io.File

private enum class Tab(val title: Int, val icon: ImageVector) {
    HOME(R.string.tab_loader, Icons.Filled.Home),
    MODS(R.string.tab_mods, Icons.Filled.Extension),
    LOG(R.string.tab_log, Icons.Filled.Article),
    SETTINGS(R.string.tab_settings, Icons.Filled.Settings),
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun NrzApp(
    onGrantStorage: () -> Unit,
    onGrantGameStorage: (String) -> Unit,
    onGrantOverlay: (String) -> Unit,
    onUninstall: (String) -> Unit,
    onInstall: (File) -> Unit,
    onLanguageChanged: () -> Unit,
) {
    var tab by remember { mutableStateOf(Tab.HOME) }

    Scaffold(
        topBar = { TopAppBar(title = { Text(stringResource(tab.title)) }) },
        bottomBar = {
            NavigationBar {
                Tab.entries.forEach { entry ->
                    NavigationBarItem(
                        selected = tab == entry,
                        onClick = { tab = entry },
                        icon = { Icon(entry.icon, contentDescription = null) },
                        label = { Text(stringResource(entry.title)) },
                    )
                }
            }
        },
    ) { padding ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(padding)
        ) {
            when (tab) {
                Tab.HOME -> HomeScreen(
                    onGrantStorage = onGrantStorage,
                    onGrantGameStorage = onGrantGameStorage,
                    onGrantOverlay = onGrantOverlay,
                    onUninstall = onUninstall,
                    onInstall = onInstall,
                )
                Tab.MODS -> ModsScreen()
                Tab.LOG -> LogScreen()
                Tab.SETTINGS -> SettingsScreen(onLanguageChanged = onLanguageChanged)
            }
        }
    }
}
