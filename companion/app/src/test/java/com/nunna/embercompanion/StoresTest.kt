package com.nunna.embercompanion

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import com.nunna.embercompanion.android.*
import com.nunna.embercompanion.protocol.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = android.app.Application::class)
class StoresTest {
    private val ctx: Context get() = ApplicationProvider.getApplicationContext()

    @Test fun notesPersist() {
        val s = NotesStore(ctx)
        s.save(NoteRecord("M_1.wav", "Standup", 5, listOf(TranscriptSegment(0, "hi")), listOf(10), "Sum", listOf("do it"), "Simple"))
        assertEquals(1, NotesStore(ctx).notes.value.size) // a fresh store reads it back from disk
        assertEquals("Standup", NotesStore(ctx).get("M_1.wav")!!.title)
        s.delete("M_1.wav")
        assertTrue(NotesStore(ctx).notes.value.isEmpty())
    }

    @Test fun stepsAndPracticeMerge() {
        val st = StepsStore(ctx)
        st.update(listOf(Message.StepDay(20261113, 100), Message.StepDay(20261114, 5)))
        st.update(listOf(Message.StepDay(20261114, 50)))
        assertEquals(listOf(20261114 to 50, 20261113 to 100), StepsStore(ctx).days.value.map { it.date to it.steps })
        val p = PracticeStore(ctx)
        val a = Message.PracticeSession(20261114, "18:30", 25, "metronome", 120, 0)
        p.merge(listOf(a)); p.merge(listOf(a.copy(min = 30)))
        assertEquals(30, PracticeStore(ctx).sessions.value.single().min)
    }

    @Test fun notificationRules() {
        val prefs = Prefs(ctx)
        assertTrue(EmberNotificationListener.allowed(prefs, "com.whatsapp", null))
        assertTrue(EmberNotificationListener.allowed(prefs, "x.y", android.app.Notification.CATEGORY_MESSAGE))
        assertFalse(EmberNotificationListener.allowed(prefs, "com.example.game", null))
        prefs.notifApps = setOf("com.example.game")
        assertTrue(EmberNotificationListener.allowed(prefs, "com.example.game", null))
        assertFalse(EmberNotificationListener.allowed(prefs, "com.whatsapp", null))
    }

    @Test fun userErrorsHaveCodes() {
        assertEquals(Errors.WIFI_JOIN, userMessage(Errors.WIFI_JOIN, "x"))
        assertEquals("fallback", userMessage("java.lang.NullPointerException", "fallback"))
        val all = Errors::class.java.declaredFields.filter { it.type == String::class.java }.map { it.get(null) as String }
        assertTrue(all.size > 10)
        assertTrue(all.all { Regex("^[A-Z]{2}-[0-9]{2}: .+[.]$").containsMatchIn(it) || it.startsWith("WA-01") })
    }
}
