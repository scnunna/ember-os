package com.nunna.embercompanion

import com.nunna.embercompanion.link.AmberFlow
import org.junit.Assert.*
import org.junit.Test

/** Whisper captions for "no speech" never reach Amber; real sentences always do. */
class NoiseCaptionTest {
    @Test fun captionsAreDropped() {
        for (t in listOf("(sad music)", "(sad music", "[Music]", "[BLANK_AUDIO]", "(silence)", "♪♪", "...", "(upbeat music)", "[ Inaudible ]", "*wind blowing*", "(coughs)"))
            assertTrue(t, AmberFlow.isNoiseCaption(t))
    }
    @Test fun realSpeechIsKept() {
        for (t in listOf("play some music", "How many pounds is 5?", "caution", "Move gym to 6 pm", "(remind me) call mom at 5", "What is the noise level at the office tomorrow?"))
            assertFalse(t, AmberFlow.isNoiseCaption(t))
    }
}
