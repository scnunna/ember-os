package com.nunna.embercompanion.protocol

import java.io.ByteArrayOutputStream

class WavData(val sampleRate: Int, val samples: ShortArray) {
    val seconds get() = samples.size.toDouble() / sampleRate
}

/** Reads 16-bit PCM WAV (mono or stereo, which is mixed to mono). Tolerates a data chunk size of 0 or too big (streamed files). */
object WavReader {
    fun read(bytes: ByteArray): WavData {
        require(bytes.size >= 12 && String(bytes, 0, 4, Charsets.US_ASCII) == "RIFF" && String(bytes, 8, 4, Charsets.US_ASCII) == "WAVE") { "not a WAV file" }
        var pos = 12
        var rate = 0; var channels = 1; var bits = 16; var fmtTag = 1
        while (pos + 8 <= bytes.size) {
            val id = String(bytes, pos, 4, Charsets.US_ASCII)
            var size = le32(bytes, pos + 4)
            val body = pos + 8
            if (id == "fmt ") {
                fmtTag = le16(bytes, body); channels = le16(bytes, body + 2); rate = le32(bytes, body + 4); bits = le16(bytes, body + 14)
            } else if (id == "data") {
                require(rate > 0) { "WAV has no fmt chunk" }
                require(bits == 16 && (fmtTag == 1 || fmtTag == 0xFFFE)) { "only 16-bit PCM WAV is supported" }
                if (size <= 0 || body + size > bytes.size) size = bytes.size - body
                val frames = size / (2 * channels)
                val out = ShortArray(frames)
                for (i in 0 until frames) {
                    var sum = 0
                    for (c in 0 until channels) sum += le16s(bytes, body + (i * channels + c) * 2)
                    out[i] = (sum / channels).toShort()
                }
                return WavData(rate, out)
            }
            pos = body + size + (size and 1)
        }
        throw IllegalArgumentException("WAV has no data chunk")
    }

    /** Writes mono 16-bit PCM. */
    fun write(samples: ShortArray, rate: Int): ByteArray {
        val o = ByteArrayOutputStream()
        fun w32(v: Int) { for (i in 0..3) o.write((v shr (8 * i)) and 0xFF) }
        fun w16(v: Int) { o.write(v and 0xFF); o.write((v shr 8) and 0xFF) }
        o.write("RIFF".toByteArray()); w32(36 + samples.size * 2); o.write("WAVE".toByteArray())
        o.write("fmt ".toByteArray()); w32(16); w16(1); w16(1); w32(rate); w32(rate * 2); w16(2); w16(16)
        o.write("data".toByteArray()); w32(samples.size * 2)
        for (s in samples) w16(s.toInt())
        return o.toByteArray()
    }

    private fun le16(b: ByteArray, p: Int) = (b[p].toInt() and 0xFF) or ((b[p + 1].toInt() and 0xFF) shl 8)
    private fun le16s(b: ByteArray, p: Int) = ((b[p].toInt() and 0xFF) or (b[p + 1].toInt() shl 8)).toShort().toInt()
    private fun le32(b: ByteArray, p: Int) = le16(b, p) or (le16(b, p + 2) shl 16)
}

object Resampler {
    /** Resamples to [toRate] as floats in [-1, 1]. Averages over a triangular window to avoid aliasing when downsampling. */
    fun toFloat(src: ShortArray, fromRate: Int, toRate: Int = 16000): FloatArray {
        if (src.isEmpty()) return FloatArray(0)
        if (fromRate == toRate) return FloatArray(src.size) { src[it] / 32768f }
        val ratio = fromRate.toDouble() / toRate
        val n = (src.size / ratio).toInt()
        val out = FloatArray(n)
        val half = if (ratio > 1) ratio else 1.0
        for (i in 0 until n) {
            val center = i * ratio
            val lo = Math.ceil(center - half).toInt().coerceAtLeast(0)
            val hi = Math.floor(center + half).toInt().coerceAtMost(src.size - 1)
            var acc = 0.0
            var wsum = 0.0
            for (j in lo..hi) {
                val w = 1.0 - Math.abs(j - center) / half
                if (w > 0) { acc += w * src[j]; wsum += w }
            }
            out[i] = if (wsum > 0) (acc / wsum / 32768.0).toFloat() else 0f
        }
        return out
    }
}
