package com.nunna.embershared

import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

/** One line of the gateway audit log. */
@Serializable
data class AuditEntry(
    val timeMs: Long,
    val kind: String,
    val method: String,
    val host: String,
    val url: String,
    val bytes: Long,
    /** AUTO, APPROVED, DENIED, EXPIRED, BLOCKED or FAILED. */
    val outcome: String,
    val note: String = "",
)

object AuditLog {
    private val json = Json { ignoreUnknownKeys = true }

    fun encode(entries: List<AuditEntry>): String = json.encodeToString(entries)
    fun decode(text: String): List<AuditEntry> = runCatching { json.decodeFromString<List<AuditEntry>>(text) }.getOrDefault(emptyList())

    /** Newest first, capped so the file cannot grow forever. */
    fun add(list: List<AuditEntry>, e: AuditEntry, max: Int = 500): List<AuditEntry> = (listOf(e) + list).take(max)

    fun hostOf(url: String): String = runCatching { java.net.URI(url).host ?: "" }.getOrDefault("")

    /** Never keeps the one-time watch key in the log. */
    fun scrub(url: String): String = url.replace(Regex("([?&]k=)[0-9a-fA-F]+"), "$1***")
}
