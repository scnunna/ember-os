package com.nunna.embercompanion.android

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.Ringtone
import android.media.RingtoneManager
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.view.KeyEvent
import androidx.health.connect.client.HealthConnectClient
import androidx.health.connect.client.permission.HealthPermission
import androidx.health.connect.client.records.StepsRecord
import androidx.health.connect.client.records.metadata.Device
import androidx.health.connect.client.records.metadata.Metadata
import com.google.android.gms.tasks.Task
import com.google.mediapipe.tasks.genai.llminference.LlmInference
import com.google.mlkit.common.model.DownloadConditions
import com.google.mlkit.common.model.RemoteModelManager
import com.google.mlkit.nl.translate.TranslateLanguage
import com.google.mlkit.nl.translate.TranslateRemoteModel
import com.google.mlkit.nl.translate.Translation
import com.google.mlkit.nl.translate.TranslatorOptions
import com.nunna.embercompanion.protocol.ExtractiveSummarizer
import com.nunna.embercompanion.protocol.LlmFormat
import com.nunna.embercompanion.protocol.StepsRecordSpec
import com.nunna.embercompanion.protocol.Summarizer
import com.nunna.embercompanion.protocol.Summary
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.time.Instant
import java.time.ZoneOffset
import java.util.Locale
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

// ------------------------------------------------------------------ Health Connect

enum class HcStatus { READY, NOT_INSTALLED, UPDATE_NEEDED }

object HealthSteps {
    val permissions = setOf(HealthPermission.getWritePermission(StepsRecord::class))

    fun status(ctx: Context): HcStatus = when (HealthConnectClient.getSdkStatus(ctx)) {
        HealthConnectClient.SDK_AVAILABLE -> HcStatus.READY
        HealthConnectClient.SDK_UNAVAILABLE_PROVIDER_UPDATE_REQUIRED -> HcStatus.UPDATE_NEEDED
        else -> HcStatus.NOT_INSTALLED
    }

    suspend fun hasPermission(ctx: Context) =
        HealthConnectClient.getOrCreate(ctx).permissionController.getGrantedPermissions().containsAll(permissions)

    /** Writes the day records. The per-date clientRecordId makes a repeat update the same entry instead of adding one. */
    suspend fun write(ctx: Context, specs: List<StepsRecordSpec>) {
        if (specs.isEmpty()) return
        val records = specs.map { s ->
            StepsRecord(
                count = s.count,
                startTime = s.start, startZoneOffset = ZoneOffset.ofTotalSeconds(s.zoneOffsetSeconds),
                endTime = s.end, endZoneOffset = ZoneOffset.ofTotalSeconds(s.zoneOffsetSeconds),
                metadata = Metadata.autoRecorded(Device(type = Device.TYPE_WATCH), clientRecordId = s.clientRecordId, clientRecordVersion = s.version),
            )
        }
        HealthConnectClient.getOrCreate(ctx).insertRecords(records)
    }
}

// ------------------------------------------------------------------ Translation (ML Kit, on device)

object Languages {
    /** Languages ML Kit can translate offline, as codes. */
    val codes: List<String> by lazy { TranslateLanguage.getAllLanguages().sortedBy { name(it) } }

    fun name(code: String): String =
        Locale.forLanguageTag(code).getDisplayLanguage(Locale.ENGLISH).replaceFirstChar { it.uppercase() }.ifBlank { code }
}

suspend fun <T> Task<T>.await(): T = suspendCancellableCoroutine { c ->
    addOnSuccessListener { c.resume(it) }
    addOnFailureListener { c.resumeWithException(it) }
}

class TranslateEngine {
    private val manager by lazy { RemoteModelManager.getInstance() }

    suspend fun downloaded(): Set<String> =
        manager.getDownloadedModels(TranslateRemoteModel::class.java).await().map { it.language }.toSet()

    /** Explicit download of one language pack (about 30 MB each). */
    suspend fun download(code: String) {
        manager.download(TranslateRemoteModel.Builder(code).build(), DownloadConditions.Builder().build()).await()
    }

    suspend fun delete(code: String) {
        manager.deleteDownloadedModel(TranslateRemoteModel.Builder(code).build()).await()
    }

    /** Translates using only packs that are already on the phone. Fails (so the app can show TR-06) if a pack is missing. */
    suspend fun translate(text: String, from: String, to: String): String {
        if (from == to) return text
        val have = downloaded()
        val needed = setOf(from, to)
        if (!have.containsAll(needed)) throw IllegalStateException(Errors.TR_PACK)
        val t = Translation.getClient(TranslatorOptions.Builder().setSourceLanguage(from).setTargetLanguage(to).build())
        try {
            return t.translate(text).await()
        } finally {
            t.close()
        }
    }
}

// ------------------------------------------------------------------ Optional LLM summary (MediaPipe)

/** Uses an LLM file the user imported (.task / .litertlm). Falls back to the simple summarizer if it cannot run. */
class LlmSummarizer(private val ctx: Context, private val modelPath: String, private val fallback: Summarizer = ExtractiveSummarizer()) : Summarizer {
    override val name = "AI model (" + File(modelPath).name + ")"
    /** Set when the last run fell back to the simple summarizer. */
    @Volatile var lastError: String? = null

    override suspend fun summarize(transcript: String): Summary = withContext(Dispatchers.Default) {
        lastError = null
        try {
            val opts = LlmInference.LlmInferenceOptions.builder().setModelPath(modelPath).setMaxTokens(1536).build()
            val llm = LlmInference.createFromOptions(ctx, opts)
            try {
                val reply = llm.generateResponse(LlmFormat.prompt(transcript))
                LlmFormat.parse(reply, name) ?: throw IllegalStateException("unreadable reply")
            } finally {
                llm.close()
            }
        } catch (e: Throwable) {
            lastError = Errors.LLM_FAIL
            fallback.summarize(transcript)
        }
    }
}

// ------------------------------------------------------------------ Watch Wi-Fi file download

class WifiFetcher(private val ctx: Context) {
    /**
     * Joins the watch hotspot (only for this app), downloads http://ip/f/name into [dest], then lets go of the network.
     * The download goes through the joined network object, which does the same job as binding the whole process to it.
     */
    suspend fun download(ssid: String, pass: String, ip: String, name: String, dest: File, onProgress: (Int) -> Unit): File {
        val cm = ctx.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val spec = WifiNetworkSpecifier.Builder().setSsid(ssid).setWpa2Passphrase(pass).build()
        val req = NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).setNetworkSpecifier(spec).build()
        lateinit var cb: ConnectivityManager.NetworkCallback
        try {
            val net: Network = suspendCancellableCoroutine { c ->
                cb = object : ConnectivityManager.NetworkCallback() {
                    override fun onAvailable(network: Network) { if (c.isActive) c.resume(network) }
                    override fun onUnavailable() { if (c.isActive) c.resumeWithException(IllegalStateException(Errors.WIFI_JOIN)) }
                }
                cm.requestNetwork(req, cb, 30_000)
                c.invokeOnCancellation { runCatching { cm.unregisterNetworkCallback(cb) } }
            }
            return withContext(Dispatchers.IO) {
                dest.parentFile?.mkdirs()
                val tmp = File(dest.path + ".part")
                val conn = net.openConnection(URL("http://$ip/f/$name")) as HttpURLConnection
                conn.connectTimeout = 10_000; conn.readTimeout = 20_000
                if (conn.responseCode != 200) throw IllegalStateException(Errors.WIFI_GET)
                val total = conn.contentLengthLong
                conn.inputStream.use { i ->
                    tmp.outputStream().use { o ->
                        val buf = ByteArray(32 * 1024)
                        var n = 0L
                        while (true) {
                            val r = i.read(buf)
                            if (r < 0) break
                            o.write(buf, 0, r); n += r
                            if (total > 0) onProgress((n * 100 / total).toInt())
                        }
                    }
                }
                if (!tmp.renameTo(dest)) throw IllegalStateException(Errors.WIFI_GET)
                dest
            }
        } finally {
            runCatching { cm.unregisterNetworkCallback(cb) }
        }
    }
}

// ------------------------------------------------------------------ Find phone + music keys

class Ringer(private val ctx: Context) {
    private var ringtone: Ringtone? = null
    private var oldVolume = -1

    val ringing get() = ringtone != null

    fun start() {
        if (ringtone != null) return
        val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        oldVolume = am.getStreamVolume(AudioManager.STREAM_ALARM)
        am.setStreamVolume(AudioManager.STREAM_ALARM, am.getStreamMaxVolume(AudioManager.STREAM_ALARM), 0)
        val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM) ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE)
        ringtone = RingtoneManager.getRingtone(ctx, uri)?.apply {
            audioAttributes = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ALARM).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build()
            isLooping = true
            play()
        }
    }

    fun stop() {
        ringtone?.stop(); ringtone = null
        if (oldVolume >= 0) {
            (ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager).setStreamVolume(AudioManager.STREAM_ALARM, oldVolume, 0)
            oldVolume = -1
        }
    }
}

object Media {
    /** Handles the watch Remote app: toggle / next / prev / vol_up / vol_down. */
    fun command(ctx: Context, cmd: String) {
        val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        fun key(code: Int) {
            am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, code))
            am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, code))
        }
        when (cmd) {
            "toggle" -> key(KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE)
            "next" -> key(KeyEvent.KEYCODE_MEDIA_NEXT)
            "prev" -> key(KeyEvent.KEYCODE_MEDIA_PREVIOUS)
            "vol_up" -> am.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_RAISE, AudioManager.FLAG_SHOW_UI)
            "vol_down" -> am.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_LOWER, AudioManager.FLAG_SHOW_UI)
        }
    }
}
