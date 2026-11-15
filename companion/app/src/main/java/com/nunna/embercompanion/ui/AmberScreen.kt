package com.nunna.embercompanion.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.nunna.embercompanion.android.AppGraph
import kotlinx.coroutines.launch

/** The conversation with Amber (voice from the watch shows up here too) plus a typed fallback. */
@Composable
fun AmberScreen(onBack: () -> Unit) {
    val lines by AppGraph.amber.lines.collectAsState()
    val busy by AppGraph.amber.busy.collectAsState()
    val scope = rememberCoroutineScope()
    var text by remember { mutableStateOf("") }
    Page("Amber", onBack) {
        Text("Hold the watch button in the Amber app to talk, or type below. Your words are turned into text on this phone; only the text goes to Life OS (through Ember Gateway).", style = MaterialTheme.typography.bodySmall)
        if (lines.isEmpty()) Text("No messages yet.")
        lines.forEach { l ->
            Card(
                Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = if (l.fromAmber) MaterialTheme.colorScheme.secondaryContainer else MaterialTheme.colorScheme.surfaceVariant),
            ) {
                Column(Modifier.padding(12.dp)) {
                    Text(if (l.fromAmber) "Amber" else "You", style = MaterialTheme.typography.labelMedium)
                    Text(l.text)
                }
            }
        }
        OutlinedTextField(value = text, onValueChange = { text = it.take(1000) }, label = { Text("Tell Amber something") }, modifier = Modifier.fillMaxWidth())
        Button(enabled = text.isNotBlank() && !busy, onClick = { val t = text; text = ""; scope.launch { AppGraph.amber.submit(t) } }) { Text(if (busy) "Sending..." else "Send") }
    }
}
