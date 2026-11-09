// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SemanticsTest {
    private class Node(
        val id: Int,
        val parent: Int,
        val bounds: IntArray,
        val role: Int = SemanticsWire.ROLE_NONE,
        val flags: Int = 0,
        val actions: Int = 0,
        val label: String = "",
        val value: String = "",
        val hint: String = "",
        val children: IntArray = IntArray(0),
        val custom: List<Pair<Int, String>> = emptyList(),
    )

    // Writes the wire format as SemanticsTree::write does.
    private fun encode(full: Boolean, root: Int, removed: IntArray, nodes: List<Node>): ByteArray {
        val buffer = ByteBuffer.allocate(4096).order(ByteOrder.LITTLE_ENDIAN)
        fun str(s: String) {
            val bytes = s.toByteArray(Charsets.UTF_8)
            buffer.putInt(bytes.size).put(bytes)
        }
        buffer.putInt(SemanticsWire.MAGIC).putShort(1).putShort(if (full) 1 else 0).putInt(root)
        buffer.putInt(removed.size)
        removed.forEach { buffer.putInt(it) }
        buffer.putInt(nodes.size)
        for (n in nodes) {
            buffer.putInt(n.id).putInt(n.parent)
            n.bounds.forEach { buffer.putInt(it) }
            buffer.put(n.role.toByte()).put(0).putShort(n.flags.toShort()).putInt(n.actions)
            str(n.label)
            str(n.value)
            str(n.hint)
            buffer.putInt(n.children.size)
            n.children.forEach { buffer.putInt(it) }
            buffer.putInt(n.custom.size)
            n.custom.forEach { (id, label) ->
                buffer.putInt(id)
                str(label)
            }
        }
        return buffer.array().copyOf(buffer.position())
    }

    private val snapshot = encode(
        full = true, root = 1, removed = IntArray(0),
        nodes = listOf(
            Node(1, 0, intArrayOf(0, 0, 1080, 2400), SemanticsWire.ROLE_LIST, children = intArrayOf(2, 3)),
            Node(
                2, 1, intArrayOf(0, 0, 1080, 120), SemanticsWire.ROLE_LIST_ITEM,
                actions = SemanticsWire.ACTION_CLICK or SemanticsWire.ACTION_CUSTOM,
                label = "Résumé 😀.pdf", value = "2.4 MB", custom = listOf(7 to "Share"),
            ),
            Node(
                3, 1, intArrayOf(0, 100, 1080, 220), SemanticsWire.ROLE_SWITCH,
                flags = SemanticsWire.FLAG_CHECKABLE or SemanticsWire.FLAG_CHECKED, label = "Hidden files",
            ),
        ),
    )

    @Test
    fun parsesAFullSnapshot() {
        val update = SemanticsParser.parse(snapshot)
        assertTrue(update.full)
        assertEquals(1, update.rootId)
        assertEquals(3, update.nodes.size)
        val row = update.nodes[1]
        assertEquals(2, row.id)
        assertEquals(1, row.parent)
        assertEquals(120, row.bottom)
        assertEquals("Résumé 😀.pdf", row.label)
        assertEquals("2.4 MB", row.value)
        assertTrue(row.hasAction(SemanticsWire.ACTION_CLICK))
        assertTrue(row.hasAction(SemanticsWire.ACTION_CUSTOM))
        assertEquals(7, row.customActions.single().id)
        assertEquals("Share", row.customActions.single().label)
        assertTrue(update.nodes[2].hasFlag(SemanticsWire.FLAG_CHECKED))
        assertArrayEquals(intArrayOf(2, 3), update.nodes[0].children)
    }

    @Test
    fun appliesDeltasToTheTree() {
        val tree = SemanticsTree()
        tree.apply(SemanticsParser.parse(snapshot))
        assertEquals(3, tree.size)

        val delta = encode(
            full = false, root = 1, removed = intArrayOf(3),
            nodes = listOf(
                Node(1, 0, intArrayOf(0, 0, 1080, 2400), SemanticsWire.ROLE_LIST, children = intArrayOf(2)),
                Node(2, 1, intArrayOf(0, 0, 1080, 120), SemanticsWire.ROLE_LIST_ITEM, label = "Renamed"),
            ),
        )
        val touched = tree.apply(SemanticsParser.parse(delta))
        assertEquals(setOf(1, 2, 3), touched)
        assertEquals(2, tree.size)
        assertNull(tree[3])
        assertEquals("Renamed", tree[2]!!.label)

        // A full snapshot replaces everything.
        tree.apply(SemanticsParser.parse(encode(true, 0, IntArray(0), emptyList())))
        assertEquals(0, tree.size)
        assertEquals(0, tree.rootId)
    }

    @Test
    fun hitTestsTheTopmostDeepestNode() {
        val tree = SemanticsTree()
        tree.apply(SemanticsParser.parse(snapshot))
        // Rows 2 and 3 overlap at y 100..120; the later child is drawn on top.
        assertEquals(3, tree.hitTest(10f, 110f))
        assertEquals(2, tree.hitTest(10f, 50f))
        assertEquals(1, tree.hitTest(10f, 1000f))
        assertEquals(0, tree.hitTest(10f, 3000f))
    }

    @Test
    fun rejectsMalformedInput() {
        fun rejects(bytes: ByteArray) {
            try {
                SemanticsParser.parse(bytes)
            } catch (e: SemanticsFormatException) {
                return
            }
            throw AssertionError("parsed ${bytes.size} bad bytes")
        }
        rejects(ByteArray(0))
        rejects(snapshot.copyOf(snapshot.size - 1))
        rejects(snapshot + byteArrayOf(0))
        rejects(snapshot.copyOf().also { it[0] = 0 })
        // A count larger than the data.
        rejects(snapshot.copyOf().also { it[12] = 0x7F })
        assertFalse(SemanticsParser.parse(encode(false, 0, IntArray(0), emptyList())).full)
    }
}
