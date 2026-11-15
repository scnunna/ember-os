package com.nunna.embercompanion.protocol

data class Summary(val title: String, val summary: String, val actions: List<String>, val engine: String)

/** Turns a transcript into a short summary + action items. Implementations: [ExtractiveSummarizer] (always) and an optional on-device LLM. */
interface Summarizer {
    val name: String
    suspend fun summarize(transcript: String): Summary
}

/** Offline, no model: scores sentences by word frequency (+ a small position bonus) and finds action items by cue phrases. */
class ExtractiveSummarizer(private val maxSentences: Int = 3, private val maxActions: Int = 6) : Summarizer {
    override val name = "Simple (built in)"

    override suspend fun summarize(transcript: String): Summary = summarizeNow(transcript)

    fun summarizeNow(transcript: String): Summary {
        val sentences = splitSentences(transcript)
        if (sentences.isEmpty()) return Summary("Empty note", "", emptyList(), name)
        val freq = HashMap<String, Int>()
        for (s in sentences) for (w in words(s)) freq[w] = (freq[w] ?: 0) + 1
        val scored = sentences.mapIndexed { i, s ->
            val ws = words(s)
            val base = if (ws.isEmpty()) 0.0 else ws.sumOf { (freq[it] ?: 0).toDouble() } / Math.pow(ws.size.toDouble(), 0.7)
            val pos = if (i == 0) 1.15 else if (i == sentences.lastIndex) 1.05 else 1.0
            val len = if (ws.size < 4) 0.3 else 1.0
            Triple(i, s, base * pos * len)
        }
        val picked = scored.sortedByDescending { it.third }.take(maxSentences).sortedBy { it.first }
        val summary = picked.joinToString(" ") { it.second }.take(600)
        val title = scored.maxByOrNull { it.third }!!.second.split(' ').take(6).joinToString(" ").trim(' ', '.', ',', '!', '?')
        return Summary(title.ifBlank { "Meeting note" }, summary, extractActions(sentences), name)
    }

    companion object {
        private val DAYS = "monday|tuesday|wednesday|thursday|friday|saturday|sunday|tomorrow|tonight|next week|end of (?:the )?(?:day|week|month)|eod|asap"
        private val CUES = Regex(
            "\\b(will|won't|need to|needs to|have to|has to|must|should|going to|gonna|action item|action|follow[- ]?up|to-?do|todo|" +
                "remember to|make sure|don't forget|let's|i'll|we'll|assign(?:ed)?|deadline|due|by (?:$DAYS))\\b",
            RegexOption.IGNORE_CASE,
        )
        private val NAME_VERB = Regex("^(?:[A-Z][a-z]{1,15})\\s+(?:will|to|should|needs|can|is going to|agreed to|volunteered to)\\b")
        private val STOP = ("the a an and or but if then so of to in on at for with is are was were be been it this that these those i you he she we they " +
            "my your our their as by from not no yes do does did have has had will would can could just very really also about there here what when which who " +
            "um uh like okay ok well").split(' ').toSet()

        fun splitSentences(text: String): List<String> =
            text.replace('\n', ' ').split(Regex("(?<=[.!?])\\s+")).map { it.trim() }.filter { it.length > 2 }

        fun words(s: String): List<String> =
            Regex("[\\p{L}']+").findAll(s.lowercase()).map { it.value }.filter { it.length > 2 && it !in STOP }.toList()

        fun isAction(sentence: String): Boolean {
            if (sentence.trim().endsWith("?")) return false
            return CUES.containsMatchIn(sentence) || NAME_VERB.containsMatchIn(sentence.trim())
        }

        fun extractActions(sentences: List<String>, max: Int = 6): List<String> =
            sentences.filter { isAction(it) }.map { it.trim().trimEnd('.', '!').take(110) }.distinct().take(max)
    }
}
