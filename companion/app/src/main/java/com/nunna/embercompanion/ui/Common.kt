package com.nunna.embercompanion.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

enum class Screen { HOME, NOTIFS, STEPS, APPS, NOTES, NOTE, TRANSLATE, PRACTICE, SETTINGS }

/** A page with a title bar and a back arrow. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun Page(title: String, onBack: (() -> Unit)?, scroll: Boolean = true, content: @Composable ColumnScope.() -> Unit) {
    Scaffold(topBar = {
        TopAppBar(
            title = { Text(title) },
            navigationIcon = {
                if (onBack != null) IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
            },
        )
    }) { pad ->
        val m = Modifier.padding(pad).padding(horizontal = 16.dp).fillMaxSize()
        Column(if (scroll) m.verticalScroll(rememberScrollState()) else m, verticalArrangement = Arrangement.spacedBy(12.dp), content = content)
    }
}

@Composable
fun ErrorText(msg: String?) {
    if (msg != null) Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)) {
        Text(msg, Modifier.padding(12.dp), color = MaterialTheme.colorScheme.onErrorContainer)
    }
}

@Composable
fun InfoCard(title: String, content: @Composable ColumnScope.() -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            content()
        }
    }
}
