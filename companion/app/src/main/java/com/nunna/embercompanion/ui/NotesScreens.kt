package com.nunna.embercompanion.ui

import android.content.Intent
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.nunna.embercompanion.android.*
import com.nunna.embercompanion.link.LinkPhase
import com.nunna.embercompanion.protocol.Message
import kotlinx.coroutines.launch
import java.text.DateFormat
import java.util.Date

@Composable
fun ModelCard() {
    val scope = rememberCoroutineScope()
    val st by AppGraph.models.state.collectAsState()
    InfoCard("Speech model: ${SpeechModel.NAME}") {
        Text("Needed once to turn speech into text. It stays on your phone; nothing is sent anywhere after the download.")
        when (val s = st) {
            is DownloadState.Done -> {
                Text("Ready (uses about ${SpeechModel.DISK_MB} MB).")
                OutlinedButton(onClick = { AppGraph.models.delete() }) { Text("Delete model") }
            }
            is DownloadState.Running -> { LinearProgressIndicator(progress = { s.percent / 100f }, Modifier.fillMaxWidth()); Text("Downloading ${s.percent}%") }
            is DownloadState.Failed -> { ErrorText(s.message); Button(onClick = { scope.launch { AppGraph.models.download() } }) { Text("Try again") } }
            DownloadState.Idle -> Button(onClick = { scope.launch { AppGraph.models.download() } }) { Text("Download (${SpeechModel.DOWNLOAD_MB} MB, from GitHub)") }
        }
    }
}

@Composable
fun NotesScreen(onBack: () -> Unit, onOpen: (String) -> Unit) {
    val phase by AppGraph.link.phase.collectAsState()
    val recs by AppGraph.recs.collectAsState()
    val jobs by AppGraph.recJobs.collectAsState()
    val notes by AppGraph.notes.notes.collectAsState()
    val connected = phase == LinkPhase.CONNECTED
    val df = remember { DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT) }
    LaunchedEffect(connected) { if (connected) AppGraph.link.send(Message.RecsReq) }

    Page("Meeting notes", onBack) {
        ModelCard()
        InfoCard("Recordings on the watch") {
            if (!connected) Text("Connect your watch to see its recordings.")
            else if (recs.isEmpty()) Text("No recordings yet.")
            recs.forEach { r ->
                val j = jobs[r.n]
                val saved = notes.any { it.id == r.n }
                Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(r.n, style = MaterialTheme.typography.titleSmall)
                    Text("${r.s / 1024} KB, ${r.marks.size} bookmark${if (r.marks.size == 1) "" else "s"}", style = MaterialTheme.typography.bodySmall)
                    if (j != null && !j.done && j.error == null) {
                        Text(j.stage + if (j.percent > 0) " ${j.percent}%" else "")
                        LinearProgressIndicator(progress = { j.percent / 100f }, Modifier.fillMaxWidth())
                    }
                    ErrorText(j?.error)
                    val busy = j != null && !j.done && j.error == null
                    Button(enabled = connected && !busy, onClick = { AppGraph.processRecording(r) }) {
                        Text(if (saved) "Make the note again" else "Download and make notes")
                    }
                }
                HorizontalDivider()
            }
            OutlinedButton(enabled = connected, onClick = { AppGraph.link.send(Message.RecsReq) }) { Text("Refresh list") }
        }
        InfoCard("Saved notes") {
            if (notes.isEmpty()) Text("Nothing saved yet.")
            notes.forEach { n ->
                Column(Modifier.fillMaxWidth().clickable { onOpen(n.id) }.padding(vertical = 4.dp)) {
                    Text(n.title, style = MaterialTheme.typography.titleSmall)
                    Text(df.format(Date(n.createdMs)) + "  -  " + n.engine, style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    }
}

@Composable
fun NoteScreen(id: String, onBack: () -> Unit) {
    val ctx = LocalContext.current
    val notes by AppGraph.notes.notes.collectAsState()
    val note = notes.firstOrNull { it.id == id }
    val phase by AppGraph.link.phase.collectAsState()
    val list = rememberLazyListState()
    val scope = rememberCoroutineScope()
    var confirmDelete by remember { mutableStateOf(false) }
    if (note == null) { Page("Note", onBack) { Text("This note is gone.") }; return }

    Page(note.title, onBack, scroll = false) {
        LazyColumn(Modifier.weight(1f), state = list, verticalArrangement = Arrangement.spacedBy(8.dp)) {
            item {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Summary (${note.engine})", style = MaterialTheme.typography.titleMedium)
                    Text(note.summary.ifBlank { "No summary." })
                    Text("Action items", style = MaterialTheme.typography.titleMedium)
                    if (note.actions.isEmpty()) Text("None found.")
                    note.actions.forEach { Text("- $it") }
                    if (note.marks.isNotEmpty()) {
                        Text("Bookmarks (tap to jump)", style = MaterialTheme.typography.titleMedium)
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            note.marks.forEach { m ->
                                AssistChip(onClick = { scope.launch { list.animateScrollToItem(1 + note.segmentForMark(m)) } }, label = { Text(clock(m)) })
                            }
                        }
                    }
                    Text("Transcript", style = MaterialTheme.typography.titleMedium)
                }
            }
            itemsIndexed(note.segments) { _, s ->
                Row(verticalAlignment = Alignment.Top) {
                    Text(clock(s.startMs), style = MaterialTheme.typography.labelSmall, modifier = Modifier.width(48.dp))
                    Text(s.text)
                }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = {
                val i = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_SUBJECT, note.title).putExtra(Intent.EXTRA_TEXT, note.shareText())
                ctx.startActivity(Intent.createChooser(i, "Share note").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            }) { Text("Share") }
            Button(enabled = phase == LinkPhase.CONNECTED, onClick = { AppGraph.sendNote(note) }) { Text(if (note.sentToWatch) "Send again" else "Send to watch") }
            OutlinedButton(onClick = { confirmDelete = true }) { Text("Delete") }
        }
    }
    if (confirmDelete) AlertDialog(
        onDismissRequest = { confirmDelete = false },
        title = { Text("Delete this note?") },
        text = { Text("The recording file on the phone is kept; only the note is removed.") },
        confirmButton = { TextButton(onClick = { AppGraph.notes.delete(note.id); confirmDelete = false; onBack() }) { Text("Delete") } },
        dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text("Cancel") } },
    )
}

private fun clock(ms: Long): String {
    val s = ms / 1000
    return "%d:%02d".format(s / 60, s % 60)
}
