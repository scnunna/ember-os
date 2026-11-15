package com.nunna.embercompanion.android

import android.content.Context
import com.nunna.embercompanion.link.TrLanguages
import com.nunna.embercompanion.protocol.Message
import com.nunna.embercompanion.protocol.NoteRecord
import com.nunna.embercompanion.protocol.PracticeCalc
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** Short code + one plain sentence on what to do. Every error the user sees comes from here. */
object Errors {
    const val BT_OFF = "BT-01: Bluetooth is off. Turn it on in Android settings, then try again."
    const val BT_PERMISSION = "BT-02: This app needs the Nearby devices permission. Tap Allow when asked."
    const val BT_NO_WATCH = "BT-03: No watch found yet. Wake the watch (it should be called Watch-XXXX) and tap Pair again."
    const val LINK_LOST = "LK-01: The watch connection dropped. It will reconnect by itself; keep the watch close."
    const val NOTIF_ACCESS = "NT-01: Notification access is off. Tap the button to open the setting and allow Ember Companion."
    const val HC_MISSING = "HC-01: Health Connect is not available on this phone. Install or update it from the Play Store."
    const val HC_PERMISSION = "HC-02: Steps cannot be saved until you allow it. Tap Allow Health Connect."
    const val HC_WRITE = "HC-03: Health Connect would not take the steps. They are kept here and will be sent next time."
    const val WIFI_WATCH = "WF-01: The watch did not start its Wi-Fi. Make sure it has an SD card or storage, then try again."
    const val WIFI_JOIN = "WF-02: Could not join the watch Wi-Fi. Stay next to the watch and try again."
    const val WIFI_GET = "WF-03: The file did not download. Try again, staying close to the watch."
    const val STT_MODEL = "ST-01: The speech model is not downloaded yet. Open Settings in this app and tap Download."
    const val STT_FAIL = "ST-02: Could not turn the recording into text. Try again, or download the speech model again."
    const val LLM_FAIL = "LM-01: The AI summary model could not run, so the simple summary was used instead."
    const val TR_PACK = "TR-06: Translation language packs are missing. Open Travel translate and tap Download."
    const val MIC = "MC-01: The microphone permission is needed. Tap Allow when asked."
    const val WATCH_ERR = "WA-01: The watch reported a problem"
    const val APP_BAD = "AP-01: That file does not look like a watch app. Pick a .lua file with a start function."
}

class Prefs(ctx: Context) {
    private val sp = ctx.getSharedPreferences("ember", Context.MODE_PRIVATE)

    var watchAddress: String?
        get() = sp.getString("addr", null)
        set(v) { sp.edit().putString("addr", v).apply() }
    var watchName: String?
        get() = sp.getString("wname", null)
        set(v) { sp.edit().putString("wname", v).apply() }

    private val _langs = MutableStateFlow(TrLanguages(sp.getString("me", "en")!!, sp.getString("them", "fr")!!))
    val languages: StateFlow<TrLanguages> = _langs
    fun setLanguages(me: String, them: String) {
        sp.edit().putString("me", me).putString("them", them).apply()
        _langs.value = TrLanguages(me, them)
    }

    val notifCustomized get() = sp.getBoolean("notif_custom", false)
    var notifApps: Set<String>
        get() = sp.getStringSet("notif_apps", emptySet()) ?: emptySet()
        set(v) { sp.edit().putStringSet("notif_apps", v).putBoolean("notif_custom", true).apply() }

    fun setNotifCustom(on: Boolean) { sp.edit().putBoolean("notif_custom", on).apply() }

    var llmModelPath: String?
        get() = sp.getString("llm", null)
        set(v) { sp.edit().putString("llm", v).apply() }

    var lastRecs: String
        get() = sp.getString("recs", "") ?: ""
        set(v) { sp.edit().putString("recs", v).apply() }
}

/** Steps per day as last reported by the watch (kept so the chart works offline). */
class StepsStore(ctx: Context) {
    private val sp = ctx.getSharedPreferences("steps", Context.MODE_PRIVATE)
    private val _days = MutableStateFlow(read())
    val days: StateFlow<List<Message.StepDay>> = _days

    fun update(new: List<Message.StepDay>) {
        val map = (_days.value + new).associateBy { it.date }.toSortedMap(compareByDescending { it })
        _days.value = map.values.take(30)
        sp.edit().putString("d", JSONArray().also { a -> _days.value.forEach { a.put(JSONObject().put("d", it.date).put("s", it.steps)) } }.toString()).apply()
    }

    private fun read(): List<Message.StepDay> = runCatching {
        val a = JSONArray(sp.getString("d", "[]"))
        (0 until a.length()).map { Message.StepDay(a.getJSONObject(it).getInt("d"), a.getJSONObject(it).getInt("s")) }
    }.getOrDefault(emptyList())
}

class PracticeStore(ctx: Context) {
    private val sp = ctx.getSharedPreferences("practice", Context.MODE_PRIVATE)
    private val _sessions = MutableStateFlow(read())
    val sessions: StateFlow<List<Message.PracticeSession>> = _sessions

    fun merge(new: List<Message.PracticeSession>) {
        _sessions.value = PracticeCalc.merge(_sessions.value, new).takeLast(500)
        sp.edit().putString("s", JSONArray().also { a ->
            _sessions.value.forEach { a.put(JSONObject().put("d", it.date).put("st", it.start).put("m", it.min).put("t", it.tool).put("b", it.bpmMax).put("c", it.changes)) }
        }.toString()).apply()
    }

    private fun read(): List<Message.PracticeSession> = runCatching {
        val a = JSONArray(sp.getString("s", "[]"))
        (0 until a.length()).map {
            val o = a.getJSONObject(it)
            Message.PracticeSession(o.getInt("d"), o.getString("st"), o.getInt("m"), o.getString("t"), o.getInt("b"), o.getInt("c"))
        }
    }.getOrDefault(emptyList())
}

/** Meeting notes as one JSON file each in the app's private folder. Nothing leaves the phone. */
class NotesStore(private val ctx: Context) {
    private val dir = File(ctx.filesDir, "notes").apply { mkdirs() }
    private val json = Json { ignoreUnknownKeys = true }
    private val _notes = MutableStateFlow(load())
    val notes: StateFlow<List<NoteRecord>> = _notes

    private fun fileFor(id: String) = File(dir, id.replace(Regex("[^A-Za-z0-9._-]"), "_") + ".json")

    fun save(n: NoteRecord) {
        fileFor(n.id).writeText(json.encodeToString(n))
        _notes.value = load()
    }

    fun get(id: String) = _notes.value.firstOrNull { it.id == id }

    fun delete(id: String) { fileFor(id).delete(); _notes.value = load() }

    private fun load(): List<NoteRecord> =
        (dir.listFiles { f -> f.extension == "json" } ?: emptyArray())
            .mapNotNull { runCatching { json.decodeFromString<NoteRecord>(it.readText()) }.getOrNull() }
            .sortedByDescending { it.createdMs }
}
