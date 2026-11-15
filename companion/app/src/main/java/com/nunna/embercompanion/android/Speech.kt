package com.nunna.embercompanion.android

import android.content.Context
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import com.k2fsa.sherpa.onnx.FeatureConfig
import com.k2fsa.sherpa.onnx.OfflineModelConfig
import com.k2fsa.sherpa.onnx.OfflineRecognizer
import com.k2fsa.sherpa.onnx.OfflineRecognizerConfig
import com.k2fsa.sherpa.onnx.OfflineWhisperModelConfig
import com.nunna.embercompanion.protocol.Chunker
import com.nunna.embercompanion.protocol.TranscriptSegment
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.apache.commons.compress.compressors.bzip2.BZip2CompressorInputStream
import java.io.File
import java.io.FilterInputStream
import java.io.InputStream
import java.net.URL
import java.util.Locale
import java.util.UUID
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/** Where the offline speech model comes from. Official k2-fsa/sherpa-onnx GitHub release, downloaded once on request. */
object SpeechModel {
    const val NAME = "Whisper tiny (many languages)"
    const val URL_TAR = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-whisper-tiny.tar.bz2"
    const val DOWNLOAD_MB = 116
    const val DISK_MB = 104
}

sealed interface DownloadState {
    data object Idle : DownloadState
    data class Running(val percent: Int) : DownloadState
    data class Failed(val message: String) : DownloadState
    data object Done : DownloadState
}

/** Downloads the speech model and unpacks only the files we need (int8 encoder/decoder + tokens). */
class ModelStore(ctx: Context) {
    private val root = File(ctx.filesDir, "models/whisper-tiny")
    private val _state = MutableStateFlow<DownloadState>(if (installed()) DownloadState.Done else DownloadState.Idle)
    val state: StateFlow<DownloadState> = _state

    fun installed() = encoder().exists() && decoder().exists() && tokens().exists()
    fun encoder() = File(root, "tiny-encoder.int8.onnx")
    fun decoder() = File(root, "tiny-decoder.int8.onnx")
    fun tokens() = File(root, "tiny-tokens.txt")

    fun delete() { root.deleteRecursively(); _state.value = DownloadState.Idle }

    private class Counting(i: InputStream, val onBytes: (Long) -> Unit) : FilterInputStream(i) {
        var n = 0L
        override fun read(): Int = super.read().also { if (it >= 0) { n++; onBytes(n) } }
        override fun read(b: ByteArray, off: Int, len: Int): Int = super.read(b, off, len).also { if (it > 0) { n += it; onBytes(n) } }
    }

    suspend fun download() = withContext(Dispatchers.IO) {
        if (_state.value is DownloadState.Running) return@withContext
        _state.value = DownloadState.Running(0)
        val tmp = File(root.parentFile, "tmp-" + UUID.randomUUID())
        try {
            tmp.mkdirs()
            val conn = URL(SpeechModel.URL_TAR).openConnection()
            conn.connectTimeout = 20_000; conn.readTimeout = 30_000
            val total = conn.contentLengthLong.takeIf { it > 0 } ?: (SpeechModel.DOWNLOAD_MB * 1_048_576L)
            var lastPct = -1
            val counting = Counting(conn.getInputStream().buffered(1 shl 16)) { n ->
                val p = (n * 100 / total).toInt().coerceIn(0, 99)
                if (p != lastPct) { lastPct = p; _state.value = DownloadState.Running(p) }
            }
            TarArchiveInputStream(BZip2CompressorInputStream(counting)).use { tar ->
                while (true) {
                    val e = tar.nextEntry ?: break
                    if (e.isDirectory) continue
                    val name = e.name.substringAfterLast('/')
                    if (!(name.contains("int8") || name.endsWith("tokens.txt"))) continue
                    File(tmp, name).outputStream().use { o -> tar.copyTo(o) }
                }
            }
            val ok = File(tmp, "tiny-encoder.int8.onnx").exists() && File(tmp, "tiny-decoder.int8.onnx").exists() && File(tmp, "tiny-tokens.txt").exists()
            if (!ok) throw IllegalStateException("model files missing in archive")
            root.deleteRecursively()
            root.parentFile?.mkdirs()
            if (!tmp.renameTo(root)) throw IllegalStateException("could not move model into place")
            _state.value = DownloadState.Done
        } catch (e: Exception) {
            tmp.deleteRecursively()
            _state.value = DownloadState.Failed("ST-03: The speech model did not download (${e.javaClass.simpleName}). Check your internet and tap Download again.")
        }
    }
}

/** Offline speech-to-text with sherpa-onnx (Whisper tiny). Not touched by JVM tests: it loads native libraries. */
class SherpaStt(private val models: ModelStore) {
    private var rec: OfflineRecognizer? = null
    private var recLang: String? = null

    val ready get() = models.installed()

    @Synchronized
    private fun recognizer(lang: String): OfflineRecognizer {
        if (!models.installed()) throw IllegalStateException(Errors.STT_MODEL)
        if (rec == null || recLang != lang) {
            rec?.release()
            val cfg = OfflineRecognizerConfig(
                featConfig = FeatureConfig(sampleRate = 16000, featureDim = 80),
                modelConfig = OfflineModelConfig(
                    whisper = OfflineWhisperModelConfig(
                        encoder = models.encoder().absolutePath, decoder = models.decoder().absolutePath,
                        language = lang, task = "transcribe",
                    ),
                    tokens = models.tokens().absolutePath, numThreads = 3, provider = "cpu", modelType = "whisper",
                ),
                decodingMethod = "greedy_search",
            )
            rec = OfflineRecognizer(config = cfg)
            recLang = lang
        }
        return rec!!
    }

    /** One short utterance (up to ~30 s) of 16 kHz mono float audio. */
    @Synchronized
    fun transcribe(samples: FloatArray, lang: String): String {
        val r = recognizer(lang)
        val s = r.createStream()
        try {
            s.acceptWaveform(samples, 16000)
            r.decode(s)
            return r.getResult(s).text.trim()
        } finally {
            s.release()
        }
    }

    /** A long recording: cut into <=25 s pieces at quiet spots; each piece becomes one transcript segment with its start time. */
    fun transcribeLong(audio16k: FloatArray, lang: String, onProgress: (Int) -> Unit): List<TranscriptSegment> {
        val chunks = Chunker.split(audio16k)
        val out = ArrayList<TranscriptSegment>()
        for ((i, c) in chunks.withIndex()) {
            val text = transcribe(c.samples, lang)
            if (text.isNotBlank()) out += TranscriptSegment(c.startMs(), text)
            onProgress(((i + 1) * 100) / chunks.size)
        }
        return out
    }
}

/** Android text-to-speech, one utterance at a time. */
class Tts(private val ctx: Context) {
    private var tts: TextToSpeech? = null

    private suspend fun engine(): TextToSpeech {
        tts?.let { return it }
        return suspendCancellableCoroutine { c ->
            lateinit var t: TextToSpeech
            t = TextToSpeech(ctx.applicationContext) { status ->
                if (status == TextToSpeech.SUCCESS) { tts = t; c.resume(t) }
                else c.resumeWithException(IllegalStateException("The phone voice engine did not start. Check Android Settings > Text-to-speech."))
            }
        }
    }

    suspend fun speak(text: String, lang: String) {
        val t = engine()
        val r = t.setLanguage(Locale.forLanguageTag(lang))
        if (r == TextToSpeech.LANG_MISSING_DATA || r == TextToSpeech.LANG_NOT_SUPPORTED)
            throw IllegalStateException("No voice for ${Languages.name(lang)} is installed. Add one in Android Settings > Text-to-speech.")
        suspendCancellableCoroutine { c ->
            val id = UUID.randomUUID().toString()
            t.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
                override fun onStart(utteranceId: String?) {}
                override fun onDone(utteranceId: String?) { if (utteranceId == id && c.isActive) c.resume(Unit) }
                @Deprecated("Deprecated in Java") override fun onError(utteranceId: String?) { if (utteranceId == id && c.isActive) c.resume(Unit) }
            })
            t.speak(text, TextToSpeech.QUEUE_FLUSH, null, id)
            c.invokeOnCancellation { t.stop() }
        }
    }

    fun shutdown() { tts?.shutdown(); tts = null }
}
