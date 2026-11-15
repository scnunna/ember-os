package com.nunna.embershared

import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * AES-256-GCM box: output = 12-byte random IV + ciphertext + 16-byte tag. The key comes from the caller; on Android
 * it is an AndroidKeyStore key that never leaves secure hardware/keystore, in tests it is an in-memory key.
 */
class AesGcmBox(private val key: () -> SecretKey) {
    private val rnd = SecureRandom()

    fun encrypt(plain: ByteArray, aad: ByteArray = ByteArray(0)): ByteArray {
        val iv = ByteArray(12).also { rnd.nextBytes(it) }
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, key(), GCMParameterSpec(128, iv))
        if (aad.isNotEmpty()) c.updateAAD(aad)
        return iv + c.doFinal(plain)
    }

    /** Throws if the data was changed or the key is wrong. */
    fun decrypt(box: ByteArray, aad: ByteArray = ByteArray(0)): ByteArray {
        require(box.size >= 12 + 16) { "too short" }
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, box.copyOfRange(0, 12)))
        if (aad.isNotEmpty()) c.updateAAD(aad)
        return c.doFinal(box, 12, box.size - 12)
    }

    fun encryptString(s: String, aad: String = "") = encrypt(s.toByteArray(Charsets.UTF_8), aad.toByteArray())
    fun decryptString(b: ByteArray, aad: String = "") = String(decrypt(b, aad.toByteArray()), Charsets.UTF_8)
}
