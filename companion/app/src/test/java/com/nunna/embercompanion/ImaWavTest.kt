package com.nunna.embercompanion

import com.nunna.embercompanion.protocol.WavReader
import org.junit.Assert.*
import org.junit.Test

/**
 * The watch's compressed meeting recordings (IMA-ADPCM WAV, format 0x11). ima_vector.wav is written by
 * firmware/tools/ima_wav_check.py with a port of the firmware encoder and checked there against ffmpeg;
 * ima_vector.pcm is the exact decode.
 */
class ImaWavTest {
    private fun res(name: String) = javaClass.classLoader!!.getResourceAsStream(name)!!.readBytes()
    private val wav = res("ima_vector.wav")
    private val pcm = res("ima_vector.pcm").let { b -> ShortArray(b.size / 2) { ((b[2 * it].toInt() and 0xFF) or (b[2 * it + 1].toInt() shl 8)).toShort() } }

    @Test fun decodesBitExactAndHonoursFact() {
        val d = WavReader.read(wav)
        assertEquals(16000, d.sampleRate)
        assertEquals(pcm.size, d.samples.size)          // fact chunk trims the padded last block
        assertArrayEquals(pcm, d.samples)
    }

    @Test fun cutOffRecordingStillReads() {
        // a crash mid-recording leaves a header that says less than the file holds, or a half-written last block
        val cut = wav.copyOf(wav.size - 100)
        val d = WavReader.read(cut)
        assertTrue(d.samples.isNotEmpty())
        assertArrayEquals(pcm.copyOf(d.samples.size), d.samples)
        val zeroSize = wav.copyOf().also { for (i in 56..59) it[i] = 0 }   // data size 0 = streamed
        assertTrue(WavReader.read(zeroSize).samples.size >= pcm.size - 505)
    }

    @Test fun pcmStillWorks() {
        val pcmWav = WavReader.write(shortArrayOf(1, -2, 300), 16000)
        assertArrayEquals(shortArrayOf(1, -2, 300), WavReader.read(pcmWav).samples)
    }
}
