// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.bugreport

/**
 * The report code the server hands back: 40 random bits written as eight Crockford base32
 * characters (no I, L, O or U). The server's id is those eight characters; people see and paste
 * `AX-XXXX-XXXX`.
 *
 * [normalize] accepts what a person is likely to type or paste: with or without the `AX` prefix and
 * the dashes, in any case, with `O` read as `0` and `I` or `L` read as `1`. That is the same set of
 * spellings the server accepts, so a code copied out of a chat message can be looked up here
 * without a round trip.
 */
object ReportCode {
    const val ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
    const val ID_LENGTH = 8
    private const val PREFIX = "AX"

    /** "7KQ4M2PD" -> "AX-7KQ4-M2PD". An id of the wrong length is returned unchanged. */
    fun format(id: String): String {
        if (id.length != ID_LENGTH) return id
        return "$PREFIX-${id.substring(0, 4)}-${id.substring(4)}"
    }

    /** The eight-character id for something typed or pasted as a code, or null if it is not one. */
    fun normalize(text: String): String? {
        var s = text.uppercase().filter { it.isLetterOrDigit() }
        if (s.length == ID_LENGTH + PREFIX.length && s.startsWith(PREFIX)) s = s.substring(PREFIX.length)
        if (s.length != ID_LENGTH) return null
        val mapped = buildString(ID_LENGTH) {
            for (c in s) {
                append(
                    when (c) {
                        'O' -> '0'
                        'I', 'L' -> '1'
                        else -> c
                    },
                )
            }
        }
        return mapped.takeIf { id -> id.all { it in ALPHABET } }
    }
}
