package com.nunna.embercompanion.protocol

import java.util.Base64

/** Builds the app_put / app_data / app_end message sequence for a Lua app. */
object AppUpload {
    const val CHUNK = 1536
    private val NAME = Regex("^[a-z0-9_-]{1,20}$")

    fun validName(n: String) = NAME.matches(n)

    /** Turns "My Timer.lua" into "my_timer" (a-z0-9_-, max 20). Empty if nothing usable is left. */
    fun cleanName(fileName: String): String =
        fileName.substringBeforeLast('.').lowercase().replace(Regex("[^a-z0-9_-]+"), "_").trim('_').take(20)

    fun messages(name: String, source: ByteArray): List<Message> {
        require(validName(name)) { "bad app name" }
        val out = ArrayList<Message>()
        out += Message.AppPut(name, source.size)
        var i = 0
        while (i < source.size) {
            val end = minOf(i + CHUNK, source.size)
            out += Message.AppData(Base64.getEncoder().encodeToString(source.copyOfRange(i, end)))
            i = end
        }
        out += Message.AppEnd
        return out
    }
}
