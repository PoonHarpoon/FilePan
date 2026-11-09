// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Log
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Encrypts the secrets native code saves (SFTP passwords, private keys and passphrases) with
 * AES-GCM, so the settings file only holds blobs. [protect] and [unprotect] are Platform::protectSecret and
 * unprotectSecret; they return null on any failure, and native code then doesn't save the secret
 * (or asks for it again).
 *
 * The key comes from [key]: in the app, a non-exportable AES-256 key in the Android Keystore that
 * doesn't need user authentication ([keystoreKey]); in tests, a software key. Failures go to
 * [onError], the log by default.
 */
class SecretBox(
    private val key: () -> SecretKey,
    private val onError: (String, Exception) -> Unit = { message, e -> Log.w(TAG, message, e) },
) {
    fun protect(plain: ByteArray): ByteArray? = try {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        // The provider picks a fresh random IV; Keystore keys insist on it.
        cipher.init(Cipher.ENCRYPT_MODE, key())
        SecretFraming.frame(cipher.iv, cipher.doFinal(plain))
    } catch (e: Exception) {
        // GeneralSecurityException, or the Keystore failing to load (IOException).
        onError("Couldn't protect a secret", e)
        null
    }

    fun unprotect(blob: ByteArray): ByteArray? {
        val framed = SecretFraming.parse(blob) ?: return null
        return try {
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(SecretFraming.TAG_BITS, framed.iv))
            cipher.doFinal(framed.ciphertext)
        } catch (e: Exception) {
            // Tampered, or sealed by a key that's gone (the app's data was restored elsewhere).
            onError("Couldn't unprotect a secret", e)
            null
        }
    }

    companion object {
        private const val TAG = "FilePan"
        private const val TRANSFORMATION = "AES/GCM/NoPadding"
        private const val KEYSTORE = "AndroidKeyStore"
        private const val ALIAS = "filepan.secrets.v1"

        /** The app's key, made the first time it's needed. */
        fun keystoreKey(): SecretKey {
            val store = KeyStore.getInstance(KEYSTORE).apply { load(null) }
            (store.getKey(ALIAS, null) as? SecretKey)?.let { return it }
            val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE)
            generator.init(
                KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setKeySize(256)
                    .setRandomizedEncryptionRequired(true)
                    .setUserAuthenticationRequired(false)
                    .build(),
            )
            return generator.generateKey()
        }
    }
}

/**
 * The blob layout: a version byte (1), the IV's length (one byte, 12 for GCM), the IV, then the
 * ciphertext with its 16-byte GCM tag.
 */
object SecretFraming {
    const val VERSION: Byte = 1
    const val TAG_BITS = 128
    private const val TAG_BYTES = TAG_BITS / 8

    class Framed(val iv: ByteArray, val ciphertext: ByteArray)

    fun frame(iv: ByteArray, ciphertext: ByteArray): ByteArray {
        require(iv.size in 1..255) { "bad IV length ${iv.size}" }
        val out = ByteArray(2 + iv.size + ciphertext.size)
        out[0] = VERSION
        out[1] = iv.size.toByte()
        iv.copyInto(out, 2)
        ciphertext.copyInto(out, 2 + iv.size)
        return out
    }

    /** Null if it isn't a blob this version wrote. */
    fun parse(blob: ByteArray): Framed? {
        if (blob.size < 2 || blob[0] != VERSION) return null
        val ivLength = blob[1].toInt() and 0xFF
        if (ivLength == 0 || blob.size < 2 + ivLength + TAG_BYTES) return null
        return Framed(blob.copyOfRange(2, 2 + ivLength), blob.copyOfRange(2 + ivLength, blob.size))
    }
}
