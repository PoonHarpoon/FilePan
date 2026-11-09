// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class Utf8Test {
    // a (1 byte), é (2), 中 (3), 😀 (4 bytes, a surrogate pair), b.
    private val text = "aé中😀b"
    private val bytes = text.toByteArray(Charsets.UTF_8)

    @Test
    fun encodesAndDecodesLikeTheJdkForValidText() {
        assertArrayEquals(bytes, Utf8.encode(text))
        assertEquals(text, Utf8.decode(bytes))
        assertEquals("", Utf8.decode(ByteArray(0)))
    }

    @Test
    fun convertsUtf16IndicesToByteOffsets() {
        val expected = intArrayOf(0, 1, 3, 6, 6, 10, 11)
        for (index in expected.indices) {
            assertEquals("index $index", expected[index], Utf8.utf16ToUtf8Offset(text, index))
        }
        // Past the end clamps; negative (no selection) passes through.
        assertEquals(11, Utf8.utf16ToUtf8Offset(text, 99))
        assertEquals(-1, Utf8.utf16ToUtf8Offset(text, -1))
    }

    @Test
    fun convertsByteOffsetsToUtf16Indices() {
        val expected = intArrayOf(0, 1, 1, 2, 2, 2, 3, 3, 3, 3, 5, 6)
        for (offset in expected.indices) {
            assertEquals("offset $offset", expected[offset], Utf8.utf8ToUtf16Offset(bytes, offset))
        }
        assertEquals(6, Utf8.utf8ToUtf16Offset(bytes, 99))
        assertEquals(-1, Utf8.utf8ToUtf16Offset(bytes, -1))
    }

    @Test
    fun roundTripsEveryBoundary() {
        var index = 0
        while (index <= text.length) {
            val offset = Utf8.utf16ToUtf8Offset(text, index)
            assertEquals(index, Utf8.utf8ToUtf16Offset(bytes, offset))
            index += if (index < text.length && Character.isHighSurrogate(text[index])) 2 else 1
        }
    }

    @Test
    fun loneSurrogatesEncodeAsReplacement() {
        val lone = "a\uD83Db"
        assertArrayEquals(byteArrayOf(0x61, 0xEF.toByte(), 0xBF.toByte(), 0xBD.toByte(), 0x62), Utf8.encode(lone))
        assertEquals(4, Utf8.utf16ToUtf8Offset(lone, 2))
    }

    @Test
    fun invalidBytesDecodeOneReplacementEach() {
        // A truncated 3-byte sequence, a stray continuation byte and an overlong '/'.
        val invalid = byteArrayOf(
            0x61, 0xE4.toByte(), 0xB8.toByte(), 0x62, 0x80.toByte(), 0xC0.toByte(), 0xAF.toByte(),
        )
        assertEquals("a��b���", Utf8.decode(invalid))
        assertEquals(3, Utf8.utf8ToUtf16Offset(invalid, 3))
    }
}
