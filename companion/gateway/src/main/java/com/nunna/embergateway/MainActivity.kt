package com.nunna.embergateway

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.nunna.embershared.RequestKind
import java.text.DateFormat
import java.util.Date

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        GatewayStore.init(this)
        setContent {
            MaterialTheme(colorScheme = if (androidx.compose.foundation.isSystemInDarkTheme()) darkColorScheme(primary = Color(0xFF3A86FF)) else lightColorScheme(primary = Color(0xFF3A86FF))) {
                Surface(Modifier.fillMaxSize()) { GatewayApp() }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GatewayApp() {
    var tab by remember { mutableIntStateOf(0) }
    val pending by GatewayStore.pending.collectAsState()
    Scaffold(topBar = { TopAppBar(title = { Text("Ember Gateway") }) }) { pad ->
        Column(Modifier.padding(pad).padding(horizontal = 16.dp).fillMaxSize()) {
            TabRow(selectedTabIndex = tab) {
                Tab(selected = tab == 0, onClick = { tab = 0 }, text = { Text("To approve" + if (pending.isNotEmpty()) " (${pending.size})" else "") })
                Tab(selected = tab == 1, onClick = { tab = 1 }, text = { Text("Log") })
            }
            Spacer(Modifier.height(12.dp))
            if (tab == 0) ApprovalsTab() else LogTab()
        }
    }
}

@Composable
private fun ApprovalsTab() {
    val ctx = androidx.compose.ui.platform.LocalContext.current
    val pending by GatewayStore.pending.collectAsState()
    val perm = if (Build.VERSION.SDK_INT >= 33) Manifest.permission.NEARBY_WIFI_DEVICES else Manifest.permission.ACCESS_FINE_LOCATION
    var hasPerm by remember { mutableStateOf(ctx.checkSelfPermission(perm) == PackageManager.PERMISSION_GRANTED) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { hasPerm = it }
    Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("This is the only app on your phone that can use the internet. Ember Companion asks it, and you decide.")
        if (!hasPerm) Card { Column(Modifier.padding(12.dp)) {
            Text("To download recordings from your watch, allow Nearby devices (Wi-Fi).")
            Button(onClick = { launcher.launch(perm) }) { Text("Allow") }
        } }
        if (pending.isEmpty()) Text("Nothing is waiting for you.")
        pending.forEach { p ->
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer)) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(when (p.request.kind) { RequestKind.MODEL -> "Download a model"; RequestKind.WATCH_FETCH -> "Download from watch"; RequestKind.HTTP -> "Other request" }, style = MaterialTheme.typography.titleMedium)
                    if (p.request.label.isNotBlank()) Text(p.request.label)
                    Text("Address (exactly):", style = MaterialTheme.typography.labelMedium)
                    Text(p.request.url, fontFamily = FontFamily.Monospace)
                    Text("Method: ${p.request.method}")
                    Text("Size: " + if (p.request.declaredSize > 0) "%.1f MB (as stated by the app)".format(p.request.declaredSize / 1_048_576.0) else "not stated")
                    if (p.request.payload.isNotEmpty()) { Text("Data that would be sent (complete):", style = MaterialTheme.typography.labelMedium); Text(p.request.payload, fontFamily = FontFamily.Monospace) }
                    Text(p.reason, style = MaterialTheme.typography.bodySmall)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = { GatewayStore.decide(p.id, true) }) { Text("Approve") }
                        OutlinedButton(onClick = { GatewayStore.decide(p.id, false) }) { Text("Deny") }
                    }
                }
            }
        }
    }
}

@Composable
private fun LogTab() {
    val audit by GatewayStore.audit.collectAsState()
    val df = remember { DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.MEDIUM) }
    Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("Every request, newest first. The log is encrypted on this phone.")
        if (audit.isEmpty()) Text("No requests yet.")
        audit.forEach { e ->
            Card { Column(Modifier.padding(12.dp)) {
                Text(df.format(Date(e.timeMs)) + "  -  " + e.outcome, style = MaterialTheme.typography.titleSmall)
                Text("${e.method} ${e.host}  (${e.kind})")
                Text(e.url, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
                Text("%.2f MB".format(e.bytes / 1_048_576.0) + if (e.note.isNotBlank()) "  -  ${e.note}" else "", style = MaterialTheme.typography.bodySmall)
            } }
        }
    }
}
