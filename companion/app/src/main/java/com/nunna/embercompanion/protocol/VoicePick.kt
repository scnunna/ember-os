package com.nunna.embercompanion.protocol

import java.util.Locale

/** What we need to know about one Android TTS voice (copied from android.speech.tts.Voice so this stays pure Kotlin). */
data class VoiceInfo(
    val name: String, val localeTag: String, val quality: Int, val latency: Int,
    val needsNetwork: Boolean, val notInstalled: Boolean = false,
)

object VoicePick {
    /** Android Voice.QUALITY_* : 100 very low, 200 low, 300 normal, 400 high, 500 very high. */
    fun qualityWord(q: Int) = when { q >= 500 -> "very high quality"; q >= 400 -> "high quality"; q >= 300 -> "normal quality"; else -> "basic quality" }

    /** Installed offline English voices only. */
    fun offlineEnglish(all: List<VoiceInfo>): List<VoiceInfo> = all.filter {
        !it.needsNetwork && !it.notInstalled && it.localeTag.lowercase().replace('_', '-').startsWith("en")
    }

    /** Best first: en-US, then higher quality, then lower latency, then name. */
    fun sorted(all: List<VoiceInfo>): List<VoiceInfo> = offlineEnglish(all).sortedWith(
        compareByDescending<VoiceInfo> { it.localeTag.replace('_', '-').equals("en-US", true) }
            .thenByDescending { it.quality }.thenBy { it.latency }.thenBy { it.name },
    )

    /** The saved choice if it is still an installed offline voice, else the best one (null = no offline voice). */
    fun choose(all: List<VoiceInfo>, saved: String?): VoiceInfo? {
        val s = sorted(all)
        return s.firstOrNull { it.name == saved } ?: s.firstOrNull()
    }

    /** "English (United States), voice 2, high quality". Voices of one locale are numbered in the sorted order. */
    fun friendlyNames(all: List<VoiceInfo>): Map<String, String> {
        val s = sorted(all)
        val perLocale = HashMap<String, Int>()
        return s.associate { v ->
            val n = perLocale.merge(v.localeTag, 1, Int::plus)!!
            val loc = Locale.forLanguageTag(v.localeTag.replace('_', '-')).getDisplayName(Locale.ENGLISH)
            v.name to "$loc, voice $n, ${qualityWord(v.quality)}"
        }
    }
}
