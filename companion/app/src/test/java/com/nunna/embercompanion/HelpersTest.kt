package com.nunna.embercompanion

import com.nunna.embercompanion.link.TranslateFlow
import com.nunna.embercompanion.protocol.*
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.junit.Assert.*
import org.junit.Test
import java.time.LocalDate

class HelpersTest {
    @Test fun resamplerAndWav() {
        // 1 second 440 Hz at 44.1 kHz -> 16 kHz keeps the tone and the length
        val src = ShortArray(44100) { (10000 * Math.sin(2 * Math.PI * 440 * it / 44100.0)).toInt().toShort() }
        val wav = WavReader.read(WavReader.write(src, 44100))
        assertEquals(44100, wav.sampleRate)
        assertArrayEquals(src, wav.samples)
        val f = Resampler.toFloat(wav.samples, 44100, 16000)
        assertEquals(16000, f.size)
        val expected = (0 until 16000).map { 10000 / 32768.0 * Math.sin(2 * Math.PI * 440 * it / 16000.0) }
        val mid = (2000 until 14000)
        val err = mid.maxOf { Math.abs(f[it] - expected[it]) }
        assertTrue("max err $err", err < 0.02)
        // same rate is a straight conversion
        assertEquals(0.5f, Resampler.toFloat(shortArrayOf(16384), 16000, 16000)[0], 1e-6f)
    }

    @Test fun wavWithZeroDataSizeAndStereo() {
        val mono = WavReader.write(shortArrayOf(1, 2, 3, 4), 16000)
        for (i in 40..43) mono[i] = 0 // streamed file: data size 0
        assertEquals(4, WavReader.read(mono).samples.size)
        assertThrows(IllegalArgumentException::class.java) { WavReader.read(ByteArray(20)) }
    }

    @Test fun chunkerCutsAtQuietSpot() {
        val rate = 16000
        val audio = FloatArray(rate * 40) { 0.3f * Math.sin(it * 0.05).toFloat() }
        for (i in rate * 23 until rate * 23 + 4000) audio[i] = 0f // a pause at 23 s
        val chunks = Chunker.split(audio, rate, 25, 4)
        assertEquals(2, chunks.size)
        assertEquals(audio.size, chunks.sumOf { it.samples.size })
        val cut = chunks[1].startSample
        assertTrue("cut at $cut", cut in (rate * 23 - 100)..(rate * 23 + 4100))
        assertTrue(chunks.all { it.samples.size <= 25 * rate })
        assertEquals(cut * 1000L / rate, chunks[1].startMs())
    }

    @Test fun noteRecordMarksAndShare() {
        val n = NoteRecord("M.wav", "Standup", 1L, listOf(TranscriptSegment(0, "hello there."), TranscriptSegment(25000, "second part."), TranscriptSegment(50000, "third.")),
            listOf(1000, 30000, 70000), "Short.", listOf("Do it"), "Simple")
        assertEquals(listOf(0, 1, 2), n.marks.map { n.segmentForMark(it) })
        assertTrue(n.shareText().contains("- Do it"))
        val json = Json.encodeToString(n)
        assertEquals(n, Json.decodeFromString<NoteRecord>(json))
        val m = n.toWatchMessage()
        assertEquals("M.wav", m.id)
        assertTrue(MessageCodec.encodeBytes(m).size < 4096)
    }

    @Test fun llmReplyParsing() {
        val s = LlmFormat.parse("TITLE: Ship plan\nSUMMARY: We agreed to ship Friday. Sam owns the report.\nACTIONS:\n- Sam sends report by Friday\n- Order SD cards\n", "Gemma")!!
        assertEquals("Ship plan", s.title)
        assertTrue(s.summary.startsWith("We agreed"))
        assertEquals(listOf("Sam sends report by Friday", "Order SD cards"), s.actions)
        assertTrue(LlmFormat.parse("SUMMARY: ok\nACTIONS:\n- none", "x")!!.actions.isEmpty())
        assertNull(LlmFormat.parse("garbage", "x"))
    }

    @Test fun practiceStats() {
        val today = LocalDate.of(2026, 11, 14)
        fun s(d: Int, min: Int, bpm: Int, start: String = "18:00") = Message.PracticeSession(d, start, min, "metronome", bpm, 0)
        val list = listOf(s(20261114, 25, 120), s(20261113, 10, 110), s(20261112, 30, 0), s(20261110, 40, 100), s(20261108, 5, 90))
        val st = PracticeCalc.stats(list, today)
        assertEquals(25 + 10 + 30 + 40 + 5, st.weekMinutes)
        assertEquals(3, st.streakDays)
        assertEquals(7, st.minutesByDay.size)
        assertEquals(listOf(20261108 to 90, 20261110 to 100, 20261113 to 110, 20261114 to 120), st.bpmTrend)
        // streak survives a day with no practice yet today
        assertEquals(2, PracticeCalc.stats(list.drop(1), today).streakDays)
        assertEquals(0, PracticeCalc.stats(emptyList(), today).streakDays)
        val merged = PracticeCalc.merge(list, listOf(s(20261114, 30, 125)))
        assertEquals(5, merged.size)
        assertEquals(30, merged.last().min)
    }

    @Test fun translateFlowBothSides() = runBlocking<Unit> {
        val sent = ArrayList<Message>()
        val spoken = ArrayList<Pair<String, String>>()
        val flow = TranslateFlow(
            transcribe = { _, lang -> if (lang == "en") "where is the station" else "la gare est la" },
            translate = { t, from, to -> "[$from>$to] $t" },
            speak = { t, lang -> spoken += t to lang },
            send = { sent += it },
        )
        val audio = FloatArray(16000)
        val a = flow.run("me", audio, "en", "fr")
        assertNull(a.error)
        assertEquals(Message.Tr("me", "where is the station", "[en>fr] where is the station"), sent[0])
        assertEquals("fr", spoken[0].second)
        val b = flow.run("them", audio, "en", "fr")
        assertEquals(Message.Tr("them", "la gare est la", "[fr>en] la gare est la"), sent[1])
        assertEquals("en", spoken[1].second)
        assertNotNull(flow.run("me", FloatArray(10), "en", "fr").error)
        assertEquals(2, sent.size)
        val failing = TranslateFlow({ _, _ -> throw IllegalStateException("x") }, { t, _, _ -> t }, { _, _ -> }, { sent += it })
        assertTrue(failing.run("me", audio, "en", "fr").error!!.startsWith("TR-02"))
        b.hashCode()
    }
}
