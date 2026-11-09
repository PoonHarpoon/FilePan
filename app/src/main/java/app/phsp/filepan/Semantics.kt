// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The native UI's accessibility semantics, as SemanticsTree (ui/semantics.hpp) serializes them.
 * Plain Kotlin, so it's unit tested on the JVM; A11yBridge maps it onto Android's accessibility
 * APIs.
 */
object SemanticsWire {
    const val MAGIC = 0x4D535046
    const val VERSION = 1
    const val FLAG_FULL = 1

    // ui::Role, in order.
    const val ROLE_NONE = 0
    const val ROLE_BUTTON = 1
    const val ROLE_SWITCH = 2
    const val ROLE_CHECKBOX = 3
    const val ROLE_RADIO_BUTTON = 4
    const val ROLE_TEXT = 5
    const val ROLE_HEADING = 6
    const val ROLE_LIST = 7
    const val ROLE_LIST_ITEM = 8
    const val ROLE_GRID = 9
    const val ROLE_TEXT_FIELD = 10
    const val ROLE_IMAGE = 11
    const val ROLE_SCROLL = 12
    const val ROLE_TAB = 13
    const val ROLE_TAB_LIST = 14
    const val ROLE_PROGRESS = 15
    const val ROLE_SLIDER = 16
    const val ROLE_DIALOG = 17
    const val ROLE_MENU = 18
    const val ROLE_MENU_ITEM = 19

    // ui::semantics_flags.
    const val FLAG_CHECKABLE = 1 shl 0
    const val FLAG_CHECKED = 1 shl 1
    const val FLAG_SELECTED = 1 shl 2
    const val FLAG_DISABLED = 1 shl 3
    const val FLAG_EXPANDABLE = 1 shl 4
    const val FLAG_EXPANDED = 1 shl 5
    const val FLAG_FOCUSABLE = 1 shl 6
    const val FLAG_FOCUSED = 1 shl 7
    const val FLAG_PASSWORD = 1 shl 8
    const val FLAG_LIVE_REGION = 1 shl 9
    const val FLAG_MULTILINE = 1 shl 10

    // ui::Action bits.
    const val ACTION_CLICK = 1 shl 0
    const val ACTION_LONG_CLICK = 1 shl 1
    const val ACTION_SCROLL_FORWARD = 1 shl 2
    const val ACTION_SCROLL_BACKWARD = 1 shl 3
    const val ACTION_SET_TEXT = 1 shl 4
    const val ACTION_FOCUS = 1 shl 5
    const val ACTION_CLEAR_FOCUS = 1 shl 6
    const val ACTION_DISMISS = 1 shl 7
    const val ACTION_EXPAND = 1 shl 8
    const val ACTION_COLLAPSE = 1 shl 9
    const val ACTION_SET_SELECTION = 1 shl 10
    const val ACTION_CUSTOM = 1 shl 31
}

class SemanticsCustomAction(val id: Int, val label: String)

class SemanticsNode(
    val id: Int,
    val parent: Int,
    val left: Int,
    val top: Int,
    val right: Int,
    val bottom: Int,
    val role: Int,
    val flags: Int,
    val actions: Int,
    val label: String,
    val value: String,
    val hint: String,
    val children: IntArray,
    val customActions: List<SemanticsCustomAction>,
) {
    fun hasFlag(flag: Int) = flags and flag != 0
    fun hasAction(action: Int) = actions and action != 0
    fun contains(x: Float, y: Float) = x >= left && x < right && y >= top && y < bottom
}

class SemanticsUpdate(
    val full: Boolean,
    val rootId: Int,
    val removed: IntArray,
    val nodes: List<SemanticsNode>,
)

class SemanticsFormatException(message: String) : Exception(message)

object SemanticsParser {
    fun parse(bytes: ByteArray): SemanticsUpdate {
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        try {
            if (buffer.int != SemanticsWire.MAGIC) throw SemanticsFormatException("Bad magic")
            val version = buffer.short.toInt() and 0xFFFF
            if (version != SemanticsWire.VERSION) {
                throw SemanticsFormatException("Unsupported version $version")
            }
            val flags = buffer.short.toInt() and 0xFFFF
            val rootId = buffer.int
            val removed = IntArray(count(buffer, 4)) { buffer.int }
            val nodeCount = count(buffer, 40)
            val nodes = ArrayList<SemanticsNode>(nodeCount)
            repeat(nodeCount) {
                val id = buffer.int
                val parent = buffer.int
                val left = buffer.int
                val top = buffer.int
                val right = buffer.int
                val bottom = buffer.int
                val role = buffer.get().toInt() and 0xFF
                buffer.get()
                val nodeFlags = buffer.short.toInt() and 0xFFFF
                val actions = buffer.int
                val label = string(buffer)
                val value = string(buffer)
                val hint = string(buffer)
                val children = IntArray(count(buffer, 4)) { buffer.int }
                val customActions = List(count(buffer, 8)) {
                    SemanticsCustomAction(buffer.int, string(buffer))
                }
                nodes += SemanticsNode(
                    id, parent, left, top, right, bottom, role, nodeFlags, actions, label, value,
                    hint, children, customActions,
                )
            }
            if (buffer.hasRemaining()) throw SemanticsFormatException("Trailing bytes")
            return SemanticsUpdate(flags and SemanticsWire.FLAG_FULL != 0, rootId, removed, nodes)
        } catch (e: java.nio.BufferUnderflowException) {
            throw SemanticsFormatException("Truncated")
        }
    }

    // A count of records of at least minSize bytes each, checked against what's left.
    private fun count(buffer: ByteBuffer, minSize: Int): Int {
        val count = buffer.int
        if (count < 0 || count.toLong() * minSize > buffer.remaining()) {
            throw SemanticsFormatException("Bad count $count")
        }
        return count
    }

    private fun string(buffer: ByteBuffer): String {
        val length = count(buffer, 1)
        val bytes = ByteArray(length)
        buffer.get(bytes)
        return Utf8.decode(bytes)
    }
}

/** What the accessibility provider knows: the latest snapshot, kept up to date by deltas. */
class SemanticsTree {
    private val nodes = HashMap<Int, SemanticsNode>()
    var rootId = 0
        private set

    val size get() = nodes.size

    operator fun get(id: Int): SemanticsNode? = nodes[id]

    /** Applies an update; returns the ids it removed or changed. */
    fun apply(update: SemanticsUpdate): Set<Int> {
        val touched = HashSet<Int>()
        if (update.full) {
            touched += nodes.keys
            nodes.clear()
        }
        rootId = update.rootId
        for (id in update.removed) {
            if (nodes.remove(id) != null) touched += id
        }
        for (node in update.nodes) {
            nodes[node.id] = node
            touched += node.id
        }
        return touched
    }

    fun clear() {
        nodes.clear()
        rootId = 0
    }

    /** The deepest node at (x, y) in window px, last child first (it's drawn on top), or 0. */
    fun hitTest(x: Float, y: Float): Int {
        var node = nodes[rootId] ?: return 0
        if (!node.contains(x, y)) return 0
        while (true) {
            var next: SemanticsNode? = null
            for (i in node.children.indices.reversed()) {
                val child = nodes[node.children[i]] ?: continue
                if (child.contains(x, y)) {
                    next = child
                    break
                }
            }
            node = next ?: return node.id
        }
    }
}
