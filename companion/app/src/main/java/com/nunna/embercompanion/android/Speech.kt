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
import kotlinx.coroutines.suspendCancellableCoroutine
import java.util.Locale
import java.util.UUID
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/** Offline speech-to-text with sherpa-onnx (Whisper tiny). Not touched by JVM tests: it loads native libraries. */
class SherpaStt(private val modelFor: () -> ModelStore) {
    private val models get() = modelFor()
    private var rec: OfflineRecognizer? = null
    private var recLang: String? = null
    private var recModel: String? = null

    /** Loads the model now (used by the bench to time it). */
    fun warmUp(lang: String = "en") { recognizer(lang) }

    @Synchronized fun unload() { rec?.release(); rec = null; recLang = null; recModel = null }

    val ready get() = models.installed()

    @Synchronized
    private fun recognizer(lang: String): OfflineRecognizer {
        if (!models.installed()) throw IllegalStateException(Errors.STT_MODEL)
        if (rec == null || recLang != lang || recModel != models.spec.id) {
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
            recModel = models.spec.id
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

    /** Milliseconds from asking to speak until the engine reports the first audio started (bench). */
    suspend fun firstAudioLatencyMs(text: String, lang: String): Long {
        var started = 0L
        val t0 = System.nanoTime()
        speak(text, lang) { if (started == 0L) started = System.nanoTime() }
        return (started - t0) / 1_000_000
    }

    /** True when the default voice for [lang] needs the network (then it is not an offline test). */
    suspend fun voiceNeedsNetwork(lang: String): Boolean? {
        val t = engine(); t.setLanguage(Locale.forLanguageTag(lang))
        return t.voice?.isNetworkConnectionRequired
    }

    suspend fun speak(text: String, lang: String, onStart: (() -> Unit)? = null) {
        val t = engine()
        val r = t.setLanguage(Locale.forLanguageTag(lang))
        if (r == TextToSpeech.LANG_MISSING_DATA || r == TextToSpeech.LANG_NOT_SUPPORTED)
            throw IllegalStateException("No voice for ${Languages.name(lang)} is installed. Add one in Android Settings > Text-to-speech.")
        suspendCancellableCoroutine { c ->
            val id = UUID.randomUUID().toString()
            t.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
                override fun onStart(utteranceId: String?) { if (utteranceId == id) onStart?.invoke() }
                override fun onDone(utteranceId: String?) { if (utteranceId == id && c.isActive) c.resume(Unit) }
                @Deprecated("Deprecated in Java") override fun onError(utteranceId: String?) { if (utteranceId == id && c.isActive) c.resume(Unit) }
            })
            t.speak(text, TextToSpeech.QUEUE_FLUSH, null, id)
            c.invokeOnCancellation { t.stop() }
        }
    }

    fun shutdown() { tts?.shutdown(); tts = null }
}
