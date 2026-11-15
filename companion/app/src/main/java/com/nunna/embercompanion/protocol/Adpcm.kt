package com.nunna.embercompanion.protocol

/** IMA-ADPCM decoder for Ember Link AUDIO blocks (133 bytes -> 256 samples, 16 kHz mono). */
object ImaAdpcm {
    const val BLOCK_BYTES = 133
    const val SAMPLES = 256

    private val STEP = intArrayOf(
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767,
    )
    private val IDX = intArrayOf(-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)

    fun seqOf(block: ByteArray): Int = block[0].toInt() and 0xFF

    /** Decodes one block. The header resets the decoder state. Returns null if the block has the wrong size. */
    fun decodeBlock(b: ByteArray): ShortArray? {
        if (b.size != BLOCK_BYTES) return null
        var pred = ((b[1].toInt() and 0xFF) or (b[2].toInt() shl 8)).toShort().toInt()
        var index = (b[3].toInt() and 0xFF).coerceIn(0, 88)
        val out = ShortArray(SAMPLES)
        var n = 0
        for (i in 5 until BLOCK_BYTES) {
            val byte = b[i].toInt() and 0xFF
            for (half in 0..1) {
                val nib = if (half == 0) byte and 0x0F else byte shr 4
                val step = STEP[index]
                var diff = step shr 3
                if (nib and 1 != 0) diff += step shr 2
                if (nib and 2 != 0) diff += step shr 1
                if (nib and 4 != 0) diff += step
                pred = if (nib and 8 != 0) pred - diff else pred + diff
                pred = pred.coerceIn(-32768, 32767)
                index = (index + IDX[nib]).coerceIn(0, 88)
                out[n++] = pred.toShort()
            }
        }
        return out
    }
}

/**
 * Turns the AUDIO notification stream into continuous PCM. A missing block (sequence gap) is replaced by 256 zero
 * samples per lost block so the timing of the audio stays right. At most [MAX_FILL_BLOCKS] are filled for one gap
 * (a bigger jump means the stream restarted, not that 1 s+ of audio was lost).
 */
class AdpcmStream {
    private var expected = -1
    var lostBlocks = 0
        private set
    var goodBlocks = 0
        private set
    private val pcm = java.io.ByteArrayOutputStream()
    private var samples = 0

    val sampleCount get() = samples

    fun reset() {
        expected = -1; lostBlocks = 0; goodBlocks = 0; pcm.reset(); samples = 0
    }

    /** Feeds one AUDIO packet. Returns the samples appended (zeros for gaps + the block), or empty for a bad block. */
    fun feed(block: ByteArray): ShortArray {
        val dec = ImaAdpcm.decodeBlock(block) ?: return ShortArray(0)
        val seq = ImaAdpcm.seqOf(block)
        var fill = 0
        if (expected >= 0) {
            val gap = (seq - expected) and 0xFF
            if (gap in 1..MAX_FILL_BLOCKS) { fill = gap; lostBlocks += gap }
        }
        expected = (seq + 1) and 0xFF
        goodBlocks++
        val all = ShortArray(fill * ImaAdpcm.SAMPLES + dec.size)
        System.arraycopy(dec, 0, all, fill * ImaAdpcm.SAMPLES, dec.size)
        for (s in all) { pcm.write(s.toInt() and 0xFF); pcm.write((s.toInt() shr 8) and 0xFF) }
        samples += all.size
        return all
    }

    /** All PCM so far as 16-bit samples. */
    fun samples(): ShortArray {
        val b = pcm.toByteArray()
        return ShortArray(b.size / 2) { ((b[2 * it].toInt() and 0xFF) or (b[2 * it + 1].toInt() shl 8)).toShort() }
    }

    companion object { const val MAX_FILL_BLOCKS = 64 }
}
