package com.nunna.embershared

import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * A small on-phone crash log (no network, nothing leaves the phone): every crash, and every background error that was
 * caught instead of crashing, is appended to `crash.txt` in the app's private folder. The app shows it with a Copy
 * button so the owner can paste it to whoever is fixing the app. Keeps the newest ~24 KB.
 */
class CrashLog(private val file: File, private val maxBytes: Int = 24_000) {
    @Synchronized
    fun record(kind: String, t: Throwable, thread: String = Thread.currentThread().name) {
        val sw = StringWriter()
        t.printStackTrace(PrintWriter(sw))
        val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
        val entry = "=== $stamp $kind (thread $thread)\n${scrub(sw.toString())}\n"
        runCatching {
            val old = if (file.exists()) file.readText() else ""
            val all = old + entry
            file.writeText(if (all.length > maxBytes) all.substring(all.length - maxBytes) else all)
        }
    }

    fun read(): String = runCatching { if (file.exists()) file.readText() else "" }.getOrDefault("")
    fun clear() { runCatching { file.delete() } }

    /** Installs the process-wide handler: write the crash, then let Android do its normal crash handling. */
    fun install() {
        val prev = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { th, e ->
            record("CRASH", e, th.name)
            prev?.uncaughtException(th, e)
        }
    }

    companion object {
        // never keep a device token or a Wi-Fi password in the log, even if an exception message carried one
        private val SECRET = Regex("""lw_[0-9a-f]{32}|[?&](k|token|pass)=[^&\s]+""")
        fun scrub(s: String) = s.replace(SECRET, "<hidden>")
    }
}
