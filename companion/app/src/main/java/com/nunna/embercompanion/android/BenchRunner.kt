package com.nunna.embercompanion.android

import android.app.ActivityManager
import android.content.Context
import android.os.Build
import android.os.Debug
import com.nunna.embercompanion.protocol.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * "What can my phone run?": real timings on this phone, median of 3 runs, failures reported as failures.
 * Nothing here is estimated or copied from a spec sheet.
 */
class BenchRunner(private val ctx: Context) {
    companion object {
        const val RUNS = 3
        /** Fixed prompt: a long-ish text and a request for a ~150 word answer, so prefill and decode are both measured. */
        val LLM_PROMPT = "Read this text and then write a summary of about 150 words.\n\n" + List(14) {
            "On Tuesday the team met to review the new watch firmware. The battery lasted two days in the last test, the display was bright outdoors, and the step counter was within five percent of the reference. Sam will send the updated battery report by Friday."
        }.joinToString(" ")
    }

    fun deviceInfo(): BenchRow {
        val am = ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val mi = ActivityManager.MemoryInfo().also { am.getMemoryInfo(it) }
        val soc = if (Build.VERSION.SDK_INT >= 31) "${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}" else Build.HARDWARE
        return BenchRow(
            "Phone",
            "%s %s, SoC %s, %d CPU cores, RAM %.1f GB (%.1f GB free now), Android %s (API %d)".format(
                Build.MANUFACTURER, Build.MODEL, soc, Runtime.getRuntime().availableProcessors(),
                mi.totalMem / 1_073_741_824.0, mi.availMem / 1_073_741_824.0, Build.VERSION.RELEASE, Build.VERSION.SDK_INT,
            ),
        )
    }

    private fun pssMb(): Double = Debug.MemoryInfo().also { Debug.getMemoryInfo(it) }.totalPss / 1024.0

    private fun clip(): FloatArray {
        val bytes = ctx.assets.open("bench_clip_10s.wav").use { it.readBytes() }
        val w = WavReader.read(bytes)
        return Resampler.toFloat(w.samples, w.sampleRate, 16000)
    }

    /** One row per speech model that is installed. Models that are not installed are reported as such, not skipped silently. */
    suspend fun stt(store: ModelStore): BenchRow = withContext(Dispatchers.Default) {
        val name = "Speech to text: ${store.spec.name}"
        if (!store.installed()) return@withContext BenchRow(name, "", "not downloaded (Settings > Speech models)")
        try {
            val audio = clip()
            val seconds = audio.size / 16000.0
            val engine = SherpaStt { store }
            val before = pssMb()
            val t0 = System.nanoTime()
            engine.warmUp("en")
            val load = (System.nanoTime() - t0) / 1e9
            var text = ""
            val times = (1..RUNS).map {
                val t = System.nanoTime()
                text = engine.transcribe(audio, "en")
                (System.nanoTime() - t) / 1e9
            }
            val ram = pssMb() - before
            engine.unload()
            if (text.isBlank()) return@withContext BenchRow(name, "", "ran but returned no text")
            val med = BenchMath.median(times)
            BenchRow(name, "load %.1f s, %.1f s of audio in %.2f s (real-time factor %.2f, %s), RAM about +%.0f MB. Heard: \"%s\"".format(
                load, seconds, med, BenchMath.rtf(med, seconds), if (med < seconds) "faster than real time" else "SLOWER than real time", ram, text.take(60)))
        } catch (e: Throwable) {
            BenchRow(name, "", e.javaClass.simpleName + ": " + (e.message ?: "").take(120))
        }
    }

    /** One row per backend. A backend that cannot start (for example no usable GPU) is a failure row, with the reason. */
    suspend fun llm(llm: LlmRuntime, modelPresent: Boolean, backend: LlmBackend): BenchRow {
        val name = "On-device AI (Gemma 4 E2B) on ${backend.name}"
        if (!modelPresent) return BenchRow(name, "", "model not downloaded (Settings > AI model, 2.4 GB)")
        return try {
            val runs = (1..RUNS).map { llm.benchRun(backend, LLM_PROMPT) }
            BenchRow(name, LlmSummaryStats.describe(backend.name, LlmSummaryStats.median(runs)))
        } catch (e: Throwable) {
            BenchRow(name, "", e.javaClass.simpleName + ": " + (e.message ?: "").take(160))
        }
    }

    suspend fun tts(tts: Tts): BenchRow {
        val name = "Spoken translation (Android text-to-speech)"
        return try {
            val net = tts.voiceNeedsNetwork("en")
            val times = (1..RUNS).map { tts.firstAudioLatencyMs("This is a speed test of the voice.", "en") }
            val med = BenchMath.median(times.map { it.toDouble() })
            BenchRow(name, "first audio after %.0f ms (median of %d)%s".format(med, RUNS, if (net == true) ". WARNING: this voice needs the network, so this is not an offline number" else if (net == false) ", offline voice" else ""))
        } catch (e: Throwable) {
            BenchRow(name, "", e.javaClass.simpleName + ": " + (e.message ?: "").take(120))
        }
    }
}
