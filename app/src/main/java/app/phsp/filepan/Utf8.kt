// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

/**
 * UTF-8 for the JNI boundary. Native code works in UTF-8 byte offsets, Java in UTF-16 code units,
 * so text crosses as UTF-8 bytes and offsets are converted here.
 *
 * Both directions agree with the native side (jni_util.cpp): a lone surrogate encodes as U+FFFD,
 * and each byte that doesn't start a valid sequence decodes to one U+FFFD.
 */
object Utf8 {
    private const val REPLACEMENT = 0xFFFD

    fun encode(text: CharSequence): ByteArray {
        val out = java.io.ByteArrayOutputStream(text.length + 8)
        var i = 0
        while (i < text.length) {
            val codePoint = codePointAt(text, i)
            i += if (codePoint >= 0x10000) 2 else 1
            appendUtf8(out, codePoint)
        }
        return out.toByteArray()
    }

    fun decode(bytes: ByteArray): String {
        val out = StringBuilder(bytes.size)
        var i = 0
        while (i < bytes.size) {
            val (codePoint, length) = decodeAt(bytes, i)
            out.appendCodePoint(codePoint)
            i += length
        }
        return out.toString()
    }

    /**
     * The UTF-8 byte offset of UTF-16 index [index] in [text]. Negative indices (no selection or
     * composing region) pass through; an index inside a surrogate pair rounds down to the pair.
     */
    fun utf16ToUtf8Offset(text: CharSequence, index: Int): Int {
        if (index < 0) return index
        val end = minOf(index, text.length)
        var bytes = 0
        var i = 0
        while (i < end) {
            val codePoint = codePointAt(text, i)
            val units = if (codePoint >= 0x10000) 2 else 1
            if (i + units > end) break
            bytes += utf8Length(codePoint)
            i += units
        }
        return bytes
    }

    /**
     * The UTF-16 index of UTF-8 byte offset [offset] in [bytes]. Negative offsets pass through; an
     * offset inside a sequence rounds down to its start.
     */
    fun utf8ToUtf16Offset(bytes: ByteArray, offset: Int): Int {
        if (offset < 0) return offset
        val end = minOf(offset, bytes.size)
        var units = 0
        var i = 0
        while (i < end) {
            val (codePoint, length) = decodeAt(bytes, i)
            if (i + length > end) break
            units += if (codePoint >= 0x10000) 2 else 1
            i += length
        }
        return units
    }

    private fun codePointAt(text: CharSequence, i: Int): Int {
        val c = text[i]
        if (Character.isHighSurrogate(c) && i + 1 < text.length && Character.isLowSurrogate(text[i + 1])) {
            return Character.toCodePoint(c, text[i + 1])
        }
        return if (Character.isSurrogate(c)) REPLACEMENT else c.code
    }

    private fun utf8Length(codePoint: Int): Int = when {
        codePoint < 0x80 -> 1
        codePoint < 0x800 -> 2
        codePoint < 0x10000 -> 3
        else -> 4
    }

    private fun appendUtf8(out: java.io.ByteArrayOutputStream, c: Int) {
        when {
            c < 0x80 -> out.write(c)
            c < 0x800 -> {
                out.write(0xC0 or (c shr 6))
                out.write(0x80 or (c and 0x3F))
            }
            c < 0x10000 -> {
                out.write(0xE0 or (c shr 12))
                out.write(0x80 or ((c shr 6) and 0x3F))
                out.write(0x80 or (c and 0x3F))
            }
            else -> {
                out.write(0xF0 or (c shr 18))
                out.write(0x80 or ((c shr 12) and 0x3F))
                out.write(0x80 or ((c shr 6) and 0x3F))
                out.write(0x80 or (c and 0x3F))
            }
        }
    }

    /** The code point at [i] and how many bytes it took; (U+FFFD, 1) for an invalid sequence. */
    private fun decodeAt(bytes: ByteArray, i: Int): Pair<Int, Int> {
        val b0 = bytes[i].toInt() and 0xFF
        val length: Int
        var c: Int
        val minimum: Int
        when {
            b0 < 0x80 -> return Pair(b0, 1)
            b0 and 0xE0 == 0xC0 -> { length = 2; c = b0 and 0x1F; minimum = 0x80 }
            b0 and 0xF0 == 0xE0 -> { length = 3; c = b0 and 0x0F; minimum = 0x800 }
            b0 and 0xF8 == 0xF0 -> { length = 4; c = b0 and 0x07; minimum = 0x10000 }
            else -> return Pair(REPLACEMENT, 1)
        }
        if (i + length > bytes.size) return Pair(REPLACEMENT, 1)
        for (k in 1 until length) {
            val b = bytes[i + k].toInt() and 0xFF
            if (b and 0xC0 != 0x80) return Pair(REPLACEMENT, 1)
            c = (c shl 6) or (b and 0x3F)
        }
        if (c < minimum || c > 0x10FFFF || c in 0xD800..0xDFFF) return Pair(REPLACEMENT, 1)
        return Pair(c, length)
    }
}
