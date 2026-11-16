package com.nunna.embercompanion.android

import android.content.Context
import android.os.Bundle
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import android.speech.tts.Voice
import com.nunna.embercompanion.link.SpeechPrep
import com.nunna.embercompanion.protocol.VoiceInfo
import com.nunna.embercompanion.protocol.VoicePick
import com.nunna.embercompanion.protocol.WavReader
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.io.File
import java.util.UUID
import kotlin.coroutines.resume

/**
 * Amber's voice: offline Android text-to-speech only (a voice that needs the network is never offered or used; this app
 * has no internet permission anyway). [synthesize] renders to a temp WAV in cacheDir, reads it, deletes it, and returns
 * 16 kHz mono PCM for the ADPCM encoder.
 */
class AmberVoice(private val ctx: Context, private val prefs: () -> Prefs) {
    private var tts: TextToSpeech? = null
    private val voiceMap = HashMap<String, Voice>()

    private suspend fun engine(): TextToSpeech? {
        tts?.let { return it }
        return withTimeoutOrNull(10_000) {
            suspendCancellableCoroutine<TextToSpeech?> { c ->
                lateinit var t: TextToSpeech
                t = TextToSpeech(ctx.applicationContext) { status ->
                    if (status == TextToSpeech.SUCCESS) { tts = t; c.resume(t) } else c.resume(null)
                }
            }
        }
    }

    private fun infos(t: TextToSpeech): List<VoiceInfo> {
        val vs = try { t.voices ?: emptySet() } catch (e: Exception) { emptySet() }
        voiceMap.clear()
        return vs.map { v ->
            voiceMap[v.name] = v
            VoiceInfo(v.name, v.locale.toLanguageTag(), v.quality, v.latency, v.isNetworkConnectionRequired,
                notInstalled = v.features?.contains(TextToSpeech.Engine.KEY_FEATURE_NOT_INSTALLED) == true)
        }
    }

    /** Installed offline English voices, best first, with friendly names. */
    suspend fun voices(): List<Pair<VoiceInfo, String>> {
        val t = engine() ?: return emptyList()
        val all = infos(t)
        val names = VoicePick.friendlyNames(all)
        return VoicePick.sorted(all).map { it to (names[it.name] ?: it.name) }
    }

    /** The voice in use: the saved one if still installed and offline, else the best default. */
    suspend fun currentName(): String? {
        val t = engine() ?: return null
        return VoicePick.choose(infos(t), prefs().voiceName)?.name
    }

    private suspend fun prepare(): TextToSpeech? {
        val t = engine() ?: return null
        val v = VoicePick.choose(infos(t), prefs().voiceName) ?: return null   // no offline voice: do not speak
        voiceMap[v.name]?.let { t.voice = it }
        t.setSpeechRate(prefs().speechRate)
        return t
    }

    /** Preview on the phone speaker. */
    suspend fun speakLocal(text: String) {
        val t = prepare() ?: return
        val clean = SpeechPrep.cleanText(text)
        withContext(Dispatchers.Main) { t.speak(clean, TextToSpeech.QUEUE_FLUSH, null, "preview-" + UUID.randomUUID()) }
    }

    fun stopLocal() { tts?.stop() }

    /** Text -> 16 kHz mono PCM, off the main thread. Null when there is no offline voice or synthesis failed. */
    suspend fun synthesize(text: String): ShortArray? = withContext(Dispatchers.IO) {
        val t = prepare() ?: return@withContext null
        val f = File(ctx.cacheDir, "amber_tts_${UUID.randomUUID()}.wav")
        try {
            val id = "syn-" + UUID.randomUUID()
            val ok = withTimeoutOrNull(30_000) {
                suspendCancellableCoroutine<Boolean> { c ->
                    t.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
                        override fun onStart(utteranceId: String?) {}
                        override fun onDone(utteranceId: String?) { if (utteranceId == id && c.isActive) c.resume(true) }
                        @Deprecated("Deprecated in Java") override fun onError(utteranceId: String?) { if (utteranceId == id && c.isActive) c.resume(false) }
                        override fun onError(utteranceId: String?, errorCode: Int) { if (utteranceId == id && c.isActive) c.resume(false) }
                    })
                    if (t.synthesizeToFile(text, Bundle(), f, id) != TextToSpeech.SUCCESS) c.resume(false)
                    c.invokeOnCancellation { t.stop() }
                }
            } ?: false
            if (!ok || !f.exists() || f.length() < 100) return@withContext null
            val wav = WavReader.read(f.readBytes())
            if (wav.samples.isEmpty()) null else SpeechPrep.to16k(wav.samples, wav.sampleRate)
        } catch (e: Exception) {
            null
        } finally {
            f.delete()
        }
    }

    fun shutdown() { tts?.shutdown(); tts = null }
}
