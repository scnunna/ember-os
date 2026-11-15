package com.nunna.embercompanion.protocol

import kotlinx.serialization.Serializable
import java.time.LocalDate

/** Splits long audio into pieces of at most [maxSec] seconds (Whisper handles 30 s at a time), cutting at the quietest spot near the end of a piece. */
object Chunker {
    class Chunk(val startSample: Int, val samples: FloatArray) {
        fun startMs(rate: Int = 16000) = startSample * 1000L / rate
    }

    fun split(audio: FloatArray, rate: Int = 16000, maxSec: Int = 25, searchSec: Int = 4): List<Chunk> {
        val max = maxSec * rate
        val out = ArrayList<Chunk>()
        var pos = 0
        while (pos < audio.size) {
            var end = minOf(pos + max, audio.size)
            if (end < audio.size) {
                // look for the quietest 100 ms window in the last [searchSec] seconds of this piece
                val win = rate / 10
                var best = end
                var bestE = Double.MAX_VALUE
                var s = end - searchSec * rate
                if (s < pos + win) s = pos + win
                while (s + win <= end) {
                    var e = 0.0
                    for (i in s until s + win step 8) e += audio[i].toDouble() * audio[i]
                    if (e < bestE) { bestE = e; best = s + win / 2 }
                    s += win / 2
                }
                end = best
            }
            if (end <= pos) end = minOf(pos + max, audio.size)
            out += Chunk(pos, audio.copyOfRange(pos, end))
            pos = end
        }
        return out
    }
}

/** One stored meeting note. Times are in ms from the start of the recording. */
@Serializable
data class TranscriptSegment(val startMs: Long, val text: String)

@Serializable
data class NoteRecord(
    val id: String,
    val title: String,
    val createdMs: Long,
    val segments: List<TranscriptSegment>,
    val marks: List<Long>,
    val summary: String,
    val actions: List<String>,
    val engine: String,
    val sentToWatch: Boolean = false,
) {
    val transcript: String get() = segments.joinToString(" ") { it.text }.trim()

    /** Index of the segment that contains the bookmark [markMs] (the last segment starting at or before it). */
    fun segmentForMark(markMs: Long): Int {
        var idx = 0
        for ((i, s) in segments.withIndex()) if (s.startMs <= markMs) idx = i
        return idx
    }

    fun toWatchMessage(): Message.Note = Message.Note(
        id = id, title = title.take(40), summary = summary.take(600),
        actions = actions.take(6).map { it.take(80) },
    )

    fun shareText(): String = buildString {
        appendLine(title); appendLine()
        if (summary.isNotBlank()) { appendLine("Summary"); appendLine(summary); appendLine() }
        if (actions.isNotEmpty()) { appendLine("Action items"); actions.forEach { appendLine("- $it") }; appendLine() }
        appendLine("Transcript"); append(transcript)
    }
}

/** Parses the text an LLM returns for our summary prompt. */
object LlmFormat {
    fun prompt(transcript: String) = """
You summarise meeting transcripts. Reply in exactly this format and nothing else:
TITLE: <at most 6 words>
SUMMARY: <2 to 3 short sentences>
ACTIONS:
- <one action item per line, include who and when if said>
(write "- none" if there are no action items)

Transcript:
${transcript.take(6000)}
""".trimIndent()

    fun parse(reply: String, engine: String): Summary? {
        val title = Regex("(?im)^\\s*TITLE:\\s*(.+)$").find(reply)?.groupValues?.get(1)?.trim()
        val summary = Regex("(?is)SUMMARY:\\s*(.+?)(?=\\n\\s*ACTIONS:|\\z)").find(reply)?.groupValues?.get(1)?.trim()
        val actionsBlock = Regex("(?is)ACTIONS:\\s*(.*)").find(reply)?.groupValues?.get(1) ?: ""
        val actions = actionsBlock.lines().map { it.trim().removePrefix("-").removePrefix("*").trim() }
            .filter { it.isNotBlank() && !it.equals("none", true) && !it.equals("none.", true) }.take(6)
        if (summary.isNullOrBlank()) return null
        return Summary(title?.takeIf { it.isNotBlank() } ?: "Meeting note", summary, actions, engine)
    }
}

data class PracticeStats(
    val weekMinutes: Int,
    val streakDays: Int,
    val minutesByDay: List<Pair<LocalDate, Int>>, // last 7 days, oldest first
    val bpmTrend: List<Pair<Int, Int>>, // (date, max bpm) per day, oldest first, last 14 days with a bpm
)

object PracticeCalc {
    fun parse(d: Int): LocalDate? = StepsMapping.parseDate(d)

    fun stats(sessions: List<Message.PracticeSession>, today: LocalDate): PracticeStats {
        val byDay = HashMap<LocalDate, Int>()
        val bpmByDay = HashMap<LocalDate, Int>()
        for (s in sessions) {
            val d = parse(s.date) ?: continue
            byDay[d] = (byDay[d] ?: 0) + s.min
            if (s.bpmMax > 0) bpmByDay[d] = maxOf(bpmByDay[d] ?: 0, s.bpmMax)
        }
        val last7 = (6 downTo 0).map { today.minusDays(it.toLong()) }
        val week = last7.sumOf { byDay[it] ?: 0 }
        // streak: consecutive days with practice, ending today (or yesterday if today has none yet)
        var day = if ((byDay[today] ?: 0) > 0) today else today.minusDays(1)
        var streak = 0
        while ((byDay[day] ?: 0) > 0) { streak++; day = day.minusDays(1) }
        val trend = bpmByDay.entries.sortedBy { it.key }.takeLast(14).map { it.key.let { k -> k.year * 10000 + k.monthValue * 100 + k.dayOfMonth } to it.value }
        return PracticeStats(week, streak, last7.map { it to (byDay[it] ?: 0) }, trend)
    }

    /** Merges new sessions into the stored ones, replacing those with the same date + start time. */
    fun merge(old: List<Message.PracticeSession>, new: List<Message.PracticeSession>): List<Message.PracticeSession> {
        val map = LinkedHashMap<String, Message.PracticeSession>()
        for (s in old + new) map["${s.date}-${s.start}"] = s
        return map.values.sortedWith(compareBy({ it.date }, { it.start }))
    }
}
