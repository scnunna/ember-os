package com.nunna.embercompanion.protocol

/** Ember Link framing (docs/EMBER_LINK.md): one flag byte per GATT packet. bit0 = FIRST, bit1 = LAST. */
object Framing {
    const val FIRST = 0x01
    const val LAST = 0x02
    const val MAX_MESSAGE = 4096
    const val DEFAULT_MTU = 23

    /** Splits one message into packets of at most [mtu]-3 bytes (flag byte included). */
    fun split(message: ByteArray, mtu: Int): List<ByteArray> {
        require(message.size <= MAX_MESSAGE) { "message too long (${message.size} > $MAX_MESSAGE)" }
        val room = mtu - 3 - 1
        require(room > 0) { "mtu too small" }
        if (message.isEmpty()) return listOf(byteArrayOf((FIRST or LAST).toByte()))
        val out = ArrayList<ByteArray>()
        var i = 0
        while (i < message.size) {
            val end = minOf(i + room, message.size)
            var flag = 0
            if (i == 0) flag = flag or FIRST
            if (end == message.size) flag = flag or LAST
            val p = ByteArray(1 + end - i)
            p[0] = flag.toByte()
            System.arraycopy(message, i, p, 1, end - i)
            out += p
            i = end
        }
        return out
    }

    fun split(message: String, mtu: Int): List<ByteArray> = split(message.toByteArray(Charsets.UTF_8), mtu)
}

/** Rebuilds messages from packets. A FIRST packet discards any half-built message; oversize messages are dropped. */
class Reassembler {
    private var buf = java.io.ByteArrayOutputStream()
    private var active = false

    /** Number of packets/messages dropped (no FIRST seen, or over 4096 bytes). Useful for diagnostics. */
    var dropped = 0
        private set

    /** Returns the complete message bytes when [packet] was the LAST one, else null. */
    fun feed(packet: ByteArray): ByteArray? {
        if (packet.isEmpty()) return null
        val flag = packet[0].toInt() and 0xFF
        if (flag and Framing.FIRST != 0) {
            buf = java.io.ByteArrayOutputStream()
            active = true
        } else if (!active) {
            dropped++
            return null
        }
        buf.write(packet, 1, packet.size - 1)
        if (buf.size() > Framing.MAX_MESSAGE) {
            active = false
            dropped++
            return null
        }
        if (flag and Framing.LAST != 0) {
            active = false
            return buf.toByteArray()
        }
        return null
    }
}
