package com.nunna.embercompanion.protocol

import java.io.ByteArrayOutputStream

class WavData(val sampleRate: Int, val samples: ShortArray) {
    val seconds get() = samples.size.toDouble() / sampleRate
}

/**
 * Reads 16-bit PCM WAV (mono or stereo, mixed to mono) and mono IMA-ADPCM WAV (format 0x11: the watch's compressed
 * meeting recordings). Tolerates a data chunk size of 0 or too big (streamed / cut-off files).
 */
object WavReader {
    fun read(bytes: ByteArray): WavData {
        require(bytes.size >= 12 && String(bytes, 0, 4, Charsets.US_ASCII) == "RIFF" && String(bytes, 8, 4, Charsets.US_ASCII) == "WAVE") { "not a WAV file" }
        var pos = 12
        var rate = 0; var channels = 1; var bits = 16; var fmtTag = 1; var blockAlign = 0; var spb = 0; var factSamples = -1
        while (pos + 8 <= bytes.size) {
            val id = String(bytes, pos, 4, Charsets.US_ASCII)
            var size = le32(bytes, pos + 4)
            val body = pos + 8
            if (id == "fmt ") {
                fmtTag = le16(bytes, body); channels = le16(bytes, body + 2); rate = le32(bytes, body + 4); bits = le16(bytes, body + 14)
                blockAlign = le16(bytes, body + 12)
                if (size >= 20) spb = le16(bytes, body + 18)
            } else if (id == "fact" && size >= 4) {
                factSamples = le32(bytes, body)
            } else if (id == "data") {
                require(rate > 0) { "WAV has no fmt chunk" }
                if (size <= 0 || body + size > bytes.size) size = bytes.size - body
                if (fmtTag == 0x11) {
                    require(channels == 1 && bits == 4 && blockAlign >= 5) { "only mono 4-bit IMA-ADPCM is supported" }
                    if (spb <= 0) spb = (blockAlign - 4) * 2 + 1
                    return WavData(rate, ImaWav.decode(bytes, body, size, blockAlign, spb, factSamples))
                }
                require(bits == 16 && (fmtTag == 1 || fmtTag == 0xFFFE)) { "only 16-bit PCM WAV is supported" }
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

/** Standard IMA-ADPCM WAV decoder (mono): each block = int16 first sample + step index + reserved, then 4-bit codes. */
object ImaWav {
    private val STEP = intArrayOf(7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
        73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
        724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
        4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767)
    private val IDX = intArrayOf(-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)

    fun decode(b: ByteArray, start: Int, size: Int, blockAlign: Int, samplesPerBlock: Int, factSamples: Int = -1): ShortArray {
        val blocks = size / blockAlign                       // a cut-off last block is dropped
        val out = ShortArray(blocks * samplesPerBlock)
        var o = 0
        for (k in 0 until blocks) {
            val p = start + k * blockAlign
            var pred = ((b[p].toInt() and 0xFF) or (b[p + 1].toInt() shl 8)).toShort().toInt()
            var idx = (b[p + 2].toInt() and 0xFF).coerceIn(0, 88)
            out[o++] = pred.toShort()
            for (i in 1 until samplesPerBlock) {
                val j = i - 1
                val byte = b[p + 4 + (j shr 1)].toInt() and 0xFF
                val nib = if (j and 1 == 0) byte and 0x0F else byte shr 4
                val step = STEP[idx]
                var dq = step shr 3
                if (nib and 1 != 0) dq += step shr 2
                if (nib and 2 != 0) dq += step shr 1
                if (nib and 4 != 0) dq += step
                pred = (if (nib and 8 != 0) pred - dq else pred + dq).coerceIn(-32768, 32767)
                idx = (idx + IDX[nib]).coerceIn(0, 88)
                out[o++] = pred.toShort()
            }
        }
        return if (factSamples in 0 until out.size) out.copyOf(factSamples) else out
    }
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
