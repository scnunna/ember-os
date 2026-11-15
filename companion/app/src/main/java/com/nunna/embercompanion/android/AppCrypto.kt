package com.nunna.embercompanion.android

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import com.nunna.embershared.AesGcmBox
import java.security.KeyStore
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey

/**
 * S7: everything the app stores (notes, transcripts, steps, practice) is encrypted with an AES-256-GCM key that lives in
 * the Android Keystore. Tests can swap in a plain key with [useKeyForTests] because Robolectric has no Keystore.
 */
object AppCrypto {
    private const val ALIAS = "ember_companion_data"
    @Volatile private var testKey: SecretKey? = null

    fun useKeyForTests(k: SecretKey?) { testKey = k }

    private fun key(): SecretKey {
        testKey?.let { return it }
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        val g = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        g.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).setKeySize(256).build(),
        )
        return g.generateKey()
    }

    val box = AesGcmBox { key() }

    fun sealToString(plain: String, context: String): String = Base64.encodeToString(box.encryptString(plain, context), Base64.NO_WRAP)
    fun openFromString(s: String, context: String): String = box.decryptString(Base64.decode(s, Base64.NO_WRAP), context)
}
