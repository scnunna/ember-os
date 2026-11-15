package com.nunna.embercompanion

import com.nunna.embercompanion.protocol.ExtractiveSummarizer
import org.junit.Assert.*
import org.junit.Test

class SummarizerTest {
    private val transcript = """
        Okay everyone, let's start the weekly planning meeting. The new watch firmware is almost ready for testing.
        Sam will send the updated battery report by Friday. The battery lasted two days in the last test.
        We need to order three more microSD cards before the demo. Does anyone have questions about the schedule?
        Priya needs to follow up with the case printer about the new enclosure. The enclosure fits the watch well.
        I think the watch firmware looks great and the testing went well. Remember to back up the firmware files.
        That is all for today. Thanks everyone.
    """.trimIndent()

    @Test fun findsActionItems() {
        val s = ExtractiveSummarizer().summarizeNow(transcript)
        val a = s.actions.joinToString("|")
        assertTrue(a, s.actions.any { it.contains("Sam will send") })
        assertTrue(a, s.actions.any { it.contains("order three more microSD") })
        assertTrue(a, s.actions.any { it.contains("Priya needs to follow up") })
        assertTrue(a, s.actions.any { it.contains("back up the firmware") })
        assertFalse("questions are not actions", s.actions.any { it.contains("Does anyone") })
        assertFalse(s.actions.any { it.contains("Thanks everyone") })
        assertTrue(s.actions.size <= 6)
    }

    @Test fun summaryIsShortAndFromTranscript() {
        val s = ExtractiveSummarizer().summarizeNow(transcript)
        assertTrue(s.summary.length in 20..600)
        assertTrue(s.summary.contains("firmware"))
        assertTrue(s.title.isNotBlank())
        assertTrue(s.summary.split(Regex("(?<=[.!?])\\s+")).size <= 3)
    }

    @Test fun emptyTranscript() {
        val s = ExtractiveSummarizer().summarizeNow("   ")
        assertEquals("", s.summary)
        assertTrue(s.actions.isEmpty())
    }
}
