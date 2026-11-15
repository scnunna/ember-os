package com.nunna.embercompanion.protocol

/** Small pure helpers for the "What can my phone run?" screen, so the maths is tested. */
object BenchMath {
    /** Median of the values (the middle one, or the mean of the two middle ones). */
    fun median(v: List<Double>): Double {
        require(v.isNotEmpty())
        val s = v.sorted()
        return if (s.size % 2 == 1) s[s.size / 2] else (s[s.size / 2 - 1] + s[s.size / 2]) / 2
    }

    /** Real-time factor: seconds of work per second of audio. Under 1.0 is faster than real time. */
    fun rtf(processSeconds: Double, audioSeconds: Double) = processSeconds / audioSeconds

    fun tokensPerSecond(tokens: Int, seconds: Double) = if (seconds <= 0) 0.0 else tokens / seconds
}

/** One row of the results table. A failure is a failure: [error] set and no numbers. */
data class BenchRow(val item: String, val details: String, val error: String? = null) {
    val ok get() = error == null
}

object BenchReport {
    fun format(rows: List<BenchRow>): String = buildString {
        appendLine("Ember bench results")
        for (r in rows) {
            if (r.ok) appendLine("- ${r.item}: ${r.details}") else appendLine("- ${r.item}: FAILED - ${r.error}")
        }
    }.trimEnd()
}

/** One timed run of an LLM on one backend. */
data class LlmRun(val loadSec: Double, val ttftSec: Double, val prefillTps: Double, val decodeTps: Double, val prefillTokens: Int, val decodeTokens: Int, val ramMb: Double)

object LlmSummaryStats {
    /** Median of each number across runs (3 runs normally). */
    fun median(runs: List<LlmRun>): LlmRun = LlmRun(
        BenchMath.median(runs.map { it.loadSec }), BenchMath.median(runs.map { it.ttftSec }),
        BenchMath.median(runs.map { it.prefillTps }), BenchMath.median(runs.map { it.decodeTps }),
        runs.first().prefillTokens, runs.first().decodeTokens, BenchMath.median(runs.map { it.ramMb }),
    )

    fun describe(backend: String, m: LlmRun) =
        "%s: load %.1f s, first token %.2f s, prefill %.0f tokens/s, decode %.1f tokens/s (%d prompt / %d output tokens), RAM about %.0f MB"
            .format(backend, m.loadSec, m.ttftSec, m.prefillTps, m.decodeTps, m.prefillTokens, m.decodeTokens, m.ramMb)
}
