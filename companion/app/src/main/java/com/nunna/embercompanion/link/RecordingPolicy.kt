package com.nunna.embercompanion.link

import com.nunna.embercompanion.protocol.Message

/** S7 rules for recordings, pure so they can be tested. */
object RecordingPolicy {
    /** A download counts only if its size equals the size the watch listed (and that size is known). */
    fun verify(expected: Long, got: Long) = expected > 0 && got == expected

    /** The watch is told to delete its copy only after a verified download, and only if the user left the setting on. */
    fun recDel(rec: Message.RecFile, gotBytes: Long, deleteFromWatch: Boolean): Message.RecDel? =
        if (deleteFromWatch && verify(rec.s, gotBytes)) Message.RecDel(rec.n, rec.s) else null

    /** keepDays == 0 means "delete right after transcription" (the default). */
    fun deleteAfterTranscription(keepDays: Int) = keepDays <= 0

    fun expired(lastModifiedMs: Long, nowMs: Long, keepDays: Int): Boolean =
        nowMs - lastModifiedMs > keepDays.coerceAtLeast(0) * 86_400_000L
}
