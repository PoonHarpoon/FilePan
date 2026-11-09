// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import javax.crypto.KeyGenerator
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

// The Keystore isn't there in plain JUnit, so these run the same code on a software AES key.
class SecretBoxTest {
    private fun softwareKey() = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()

    @Test
    fun framingRoundTrips() {
        val iv = ByteArray(12) { it.toByte() }
        val ciphertext = ByteArray(20) { (100 + it).toByte() }
        val blob = SecretFraming.frame(iv, ciphertext)
        assertEquals(1.toByte(), blob[0])
        assertEquals(12.toByte(), blob[1])
        val parsed = SecretFraming.parse(blob)!!
        assertArrayEquals(iv, parsed.iv)
        assertArrayEquals(ciphertext, parsed.ciphertext)
    }

    @Test
    fun framingRejectsForeignBlobs() {
        assertNull(SecretFraming.parse(ByteArray(0)))
        assertNull(SecretFraming.parse(byteArrayOf(1)))
        // Another version.
        assertNull(SecretFraming.parse(byteArrayOf(2, 12) + ByteArray(28)))
        // Too short for the IV and the tag.
        assertNull(SecretFraming.parse(byteArrayOf(1, 12) + ByteArray(27)))
        assertNull(SecretFraming.parse(byteArrayOf(1, 0) + ByteArray(40)))
        // Plain text, as an old settings file had.
        assertNull(SecretFraming.parse("hunter2".toByteArray()))
    }

    @Test
    fun protectsAndUnprotects() {
        val key = softwareKey()
        val box = SecretBox({ key }, { _, _ -> })
        val secret = "correct horse battery staple".toByteArray()
        val blob = box.protect(secret)!!
        assertFalse(String(blob, Charsets.ISO_8859_1).contains("horse"))
        assertArrayEquals(secret, box.unprotect(blob))
        // A fresh IV each time.
        assertFalse(blob.contentEquals(box.protect(secret)!!))
        assertNotNull(box.unprotect(box.protect(ByteArray(0))!!))
    }

    @Test
    fun rejectsTamperingAndOtherKeys() {
        val key = softwareKey()
        val box = SecretBox({ key }, { _, _ -> })
        val blob = box.protect("secret".toByteArray())!!
        val tampered = blob.copyOf().also { it[it.size - 1] = (it[it.size - 1].toInt() xor 1).toByte() }
        assertNull(box.unprotect(tampered))
        val other = softwareKey()
        assertNull(SecretBox({ other }, { _, _ -> }).unprotect(blob))
    }
}
