package com.nunna.embercompanion

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import com.nunna.embercompanion.android.*
import com.nunna.embercompanion.link.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = android.app.Application::class)
class CheckinPersistenceTest {
    private val ctx: Context get() = ApplicationProvider.getApplicationContext()

    @org.junit.Before fun key() { AppCrypto.useKeyForTests(javax.crypto.KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()) }
    @org.junit.After fun unkey() { AppCrypto.useKeyForTests(null) }

    @Test fun queueSurvivesRestartAndIsEncrypted() {
        val p = CheckinPersistence(ctx)
        val log = CheckinLog(p.load(), { p.save(it) })
        log.append(Checkin(7, 20261115, 720, 1_794_744_000, "on", listOf("block-secret-title-id")))
        log.append(Checkin(8, 20261115, 960, 1_794_744_100, "off", listOf("b2")))
        log.markSent(setOf(Checkin(7, 20261115, 720, 1_794_744_000, "on", listOf("block-secret-title-id")).key))
        val back = CheckinLog(CheckinPersistence(ctx).load(), {})
        assertEquals(2, back.all.value.size)
        assertEquals(1, back.pendingCount())
        assertEquals(listOf("b2"), back.all.value[1].ids)
        val raw = java.io.File(ctx.applicationInfo.dataDir, "shared_prefs/checkins.xml").readText()
        assertFalse(raw.contains("block-secret-title-id"))
    }
}
