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
    private val startTab = mutableIntStateOf(0)

    override fun onNewIntent(intent: android.content.Intent) {
        super.onNewIntent(intent)
        startTab.intValue = intent.getIntExtra("tab", 0).coerceIn(0, 2)
        takeHandoff(intent)
    }

    // A Life OS link handed over from the browser: kept in memory only until the owner taps Save or Ignore.
    private fun takeHandoff(intent: android.content.Intent?) {
        val data = intent?.data?.toString() ?: return
        if (intent.action != android.content.Intent.ACTION_VIEW) return
        pendingHandoff.value = com.nunna.embershared.LifeOsPolicy.parseHandoff(data)
        handoffInvalid.value = pendingHandoff.value == null
        startTab.intValue = 2
        intent.data = null                          // don't re-read it after a rotation
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        GatewayStore.init(this)
        startTab.intValue = intent?.getIntExtra("tab", 0)?.coerceIn(0, 2) ?: 0
        takeHandoff(intent)
        setContent {
            MaterialTheme(colorScheme = if (androidx.compose.foundation.isSystemInDarkTheme()) darkColorScheme(primary = Color(0xFF3A86FF)) else lightColorScheme(primary = Color(0xFF3A86FF))) {
                Surface(Modifier.fillMaxSize()) { GatewayApp(startTab.intValue) }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GatewayApp(startTab: Int) {
    var tab by remember(startTab) { mutableIntStateOf(startTab) }
    val pending by GatewayStore.pending.collectAsState()
    Scaffold(topBar = { TopAppBar(title = { Text("Ember Gateway") }) }) { pad ->
        Column(Modifier.padding(pad).padding(horizontal = 16.dp).fillMaxSize()) {
            TabRow(selectedTabIndex = tab) {
                Tab(selected = tab == 0, onClick = { tab = 0 }, text = { Text("To approve" + if (pending.isNotEmpty()) " (${pending.size})" else "") })
                Tab(selected = tab == 1, onClick = { tab = 1 }, text = { Text("Log") })
                Tab(selected = tab == 2, onClick = { tab = 2 }, text = { Text("Life OS") })
            }
            Spacer(Modifier.height(12.dp))
            when (tab) { 0 -> ApprovalsTab(); 1 -> LogTab(); else -> LifeOsTab() }
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
                    Text(when (p.request.kind) { RequestKind.MODEL -> "Download a model"; RequestKind.WATCH_FETCH -> "Download from watch"; RequestKind.HTTP -> "Other request"; RequestKind.LIFEOS -> "Life OS" }, style = MaterialTheme.typography.titleMedium)
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
        val ctx = androidx.compose.ui.platform.LocalContext.current
        var crashText by remember { mutableStateOf(GatewayStore.crash.read()) }
        if (crashText.isNotBlank()) Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Ember Gateway had a problem. Tap Copy and paste it to Claude (nothing is sent).", style = MaterialTheme.typography.titleSmall)
                Text(crashText.takeLast(1200), style = MaterialTheme.typography.bodySmall, maxLines = 14)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = {
                        val cm = ctx.getSystemService(android.content.Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager
                        cm.setPrimaryClip(android.content.ClipData.newPlainText("Ember Gateway crash log", crashText))
                    }) { Text("Copy") }
                    OutlinedButton(onClick = { GatewayStore.crash.clear(); crashText = "" }) { Text("Clear") }
                }
            }
        }
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

private val pendingHandoff = kotlinx.coroutines.flow.MutableStateFlow<com.nunna.embershared.LifeOsConfig?>(null)
private val handoffInvalid = kotlinx.coroutines.flow.MutableStateFlow(false)

@Composable
private fun LifeOsTab() {
    val cfg by GatewayStore.lifeOs.collectAsState()
    var url by remember { mutableStateOf("") }
    var token by remember { mutableStateOf("") }
    var msg by remember { mutableStateOf<String?>(null) }
    val handoff by pendingHandoff.collectAsState()
    val invalid by handoffInvalid.collectAsState()
    Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        if (invalid) Card { Text("A link tried to set up Life OS but it was not valid, so it was ignored.", Modifier.padding(12.dp)) }
        handoff?.let { h ->
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer)) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Connect to this Life OS?", style = MaterialTheme.typography.titleMedium)
                    Text(h.host + (h.base.substringAfter(h.host, "")), fontFamily = FontFamily.Monospace)
                    if (cfg != null && cfg!!.base != h.base) Text("This REPLACES the current link (${cfg!!.host}).", color = MaterialTheme.colorScheme.error)
                    Text("Only save it if you just tapped \"Send to Ember Gateway\" in your own Life OS.", style = MaterialTheme.typography.bodySmall)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = { msg = if (GatewayStore.setLifeOs(h.base, h.token)) "Saved (encrypted)." else "Could not save."; pendingHandoff.value = null }) { Text("Save") }
                        OutlinedButton(onClick = { pendingHandoff.value = null; msg = "Ignored." }) { Text("Ignore") }
                    }
                }
            }
        }
        Text("Life OS link: lets the watch check-ins sync with Life OS on your laptop. Only your own tailnet address is accepted (https, ending in .ts.net, port allowed).")
        Text(if (cfg != null) "Configured: ${cfg!!.host}" else "Not configured.", style = MaterialTheme.typography.titleMedium)
        OutlinedTextField(value = url, onValueChange = { url = it }, label = { Text("Base URL, e.g. https://host.tailnet.ts.net:8410") }, singleLine = true, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(
            value = token, onValueChange = { token = it }, label = { Text("Device token (lw_...)") }, singleLine = true, modifier = Modifier.fillMaxWidth(),
            visualTransformation = androidx.compose.ui.text.input.PasswordVisualTransformation(),
        )
        msg?.let { Text(it) }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = {
                if (com.nunna.embershared.LifeOsPolicy.normalizeBase(url) == null) msg = "That address is not allowed: it must be https, end in .ts.net (a port is fine), and have no path, query or user name."
                else if (!com.nunna.embershared.LifeOsPolicy.validToken(token.trim())) msg = "The token must look like lw_ followed by 32 lowercase hex characters."
                else if (GatewayStore.setLifeOs(url, token)) { msg = "Saved (encrypted)."; url = ""; token = "" }
                else msg = "Could not save."
            }) { Text("Save") }
            if (cfg != null) OutlinedButton(onClick = { GatewayStore.clearLifeOs(); msg = "Removed." }) { Text("Remove") }
        }
        Text("The token is stored encrypted and is never shown again or written to the log. Only the five /api/watch/ calls (today, checkins, amber, inbox, inbox/ack) on this one host and port are allowed; redirects are refused.", style = MaterialTheme.typography.bodySmall)
    }
}
