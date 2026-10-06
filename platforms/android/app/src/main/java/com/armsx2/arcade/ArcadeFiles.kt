// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.arcade

/**
 * Arcade games kept as their own files, with no .acgame: the game's image and its security dongle, named
 * after the game's ID (NM00010.chd and NM00010.ps2, "Battle Gear 3 (NM00010).chd" too) or kept in a folder
 * named after it. The library lists the image as the game wherever it is, and [Arcade.prepare] writes the
 * .acgame the core boots from into the app's own storage, so nothing of the game is copied: only its
 * dongle goes into the memory cards folder, as it does for every arcade game.
 *
 * The files of an arcade set are never PS2 games: an image boots only on its board, and a dongle named
 * .bin would otherwise be listed as a disc. The library keeps all of them out of its list.
 */
object ArcadeFiles {
    /** The smallest PS2 memory card (8 MB without its ECC). Arcade sets carry small board ROMs that are
     *  easily taken for the dongle; anything smaller than this is not one. */
    const val MIN_CARD_BYTES = 8L shl 20

    /** Above any memory card file (the biggest the core makes, 64 MB with its ECC, is 66 MB): anything
     *  bigger is an image. */
    const val MAX_CARD_BYTES = 72L shl 20

    private val ID = Regex("(?i)(?<![a-z0-9])NM\\d{5}(?!\\d)")

    /** The game ID a file or folder name carries, upper-case: "Battle Gear 3 (NM00010).chd" is NM00010. */
    fun idIn(name: String?): String? = name?.let { ID.find(it)?.value?.uppercase() }

    enum class Kind {
        /** The game's disc or hard drive image, as the board's drive reads it. */
        IMAGE,

        /** An image still packed (.iso.gz): the drive cannot read it as it is. */
        PACKED_IMAGE,

        /** A memory card file, the dongle: .ps2, a .bin of a card's size, or one packed as .gz. Soul Calibur
         *  II's Conquest card is one too, told from its dongle by what it holds ([isConquestCard]). */
        CARD,

        /** Something else of the set (a small board ROM, a boot program): never a game. */
        OTHER,
    }

    /** What a file of an arcade set is, by its name, and by its [size] when the name cannot tell (a .bin
     *  is an image or a dongle). [size] is a packed file's own size. Null for a file no rule covers. */
    fun kindOf(name: String, size: () -> Long): Kind? {
        val lower = name.lowercase()
        val packed = lower.endsWith(".gz")
        return when (lower.removeSuffix(".gz").substringAfterLast('.', "")) {
            "chd", "iso" -> if (packed) Kind.PACKED_IMAGE else Kind.IMAGE
            "ps2", "conquestcard" -> Kind.CARD
            "bin", "img" -> {
                val bytes = size()
                when {
                    bytes > MAX_CARD_BYTES -> if (packed) Kind.PACKED_IMAGE else Kind.IMAGE
                    // A packed dongle is far smaller than its card; its size is checked once unpacked.
                    packed -> Kind.CARD
                    bytes >= MIN_CARD_BYTES -> Kind.CARD
                    else -> Kind.OTHER
                }
            }
            "elf" -> Kind.OTHER
            else -> null
        }
    }

    /** One game's image among a folder's files. */
    class Found(val id: String, val image: String, val packed: Boolean)

    /** The arcade games among one folder's files, and every file of their sets there, lower-case, which
     *  the library keeps out of its list. */
    class Folder(val games: List<Found>, val claimed: Set<String>)

    /**
     * The arcade games among [names], one folder's files: each file carries its game ID in its name, or
     * takes the folder's own ([folderId]) when the folder is named after a game. The games an .acgame there
     * covers ([covered]) are its own: their files are claimed, and not listed a second time. [sizeOf] is
     * asked only for a .bin or an .img, the one kind an image and a dongle share.
     */
    fun find(names: Collection<String>, folderId: String?, covered: Set<String>, sizeOf: (String) -> Long): Folder {
        val games = ArrayList<Found>()
        val claimed = HashSet<String>()
        for (name in names) {
            val id = idIn(name) ?: folderId ?: continue
            val kind = kindOf(name) { sizeOf(name) } ?: continue
            claimed.add(name.lowercase())
            if (id in covered) continue
            if (kind == Kind.IMAGE || kind == Kind.PACKED_IMAGE) games.add(Found(id, name, kind == Kind.PACKED_IMAGE))
        }
        return Folder(games, claimed)
    }

    /** The name the dongle of the game [id] has in the memory cards folder, by what it holds: a card with
     *  its ECC is a .ps2, one without (a whole number of megabytes) a .bin. */
    fun dongleName(id: String, bytes: Long): String = if (bytes % (1L shl 20) == 0L) "$id.bin" else "$id.ps2"

    /** Soul Calibur II, the one arcade game that reads a card of its own besides its dongle: its Conquest card,
     *  in slot 2. Every other game's card files are its dongle's. */
    const val CONQUEST_GAME = "NM00007"

    /** The Conquest card's name in the memory cards folder, where the core mounts it from when the game's
     *  .acgame names no card (VMManager::OpenArcadeGame). Never a .bin: the core converts a .bin as a card
     *  without ECC, and Soul Calibur II keeps its own checksums where the ECC would be. */
    const val CONQUEST_CARD = "$CONQUEST_GAME.conquestcard"

    /** A Conquest card with its spare bytes, the only kind the game reads: 16384 pages of 512 + 16 bytes. */
    const val CONQUEST_CARD_BYTES = 8_650_752L

    /** How a Conquest card starts (its first page, written by the game); a dongle never does. */
    private val CONQUEST_HEADER = "Memory Card for SoulCaliburII".toByteArray(Charsets.US_ASCII)

    /** How many of a card's first bytes [isConquestCard] reads. */
    val CONQUEST_HEADER_BYTES: Int get() = CONQUEST_HEADER.size

    /** Whether a card file is a Conquest card and not a dongle, by its first bytes (unpacked). */
    fun isConquestCard(head: ByteArray): Boolean =
        head.size >= CONQUEST_HEADER.size && CONQUEST_HEADER.indices.all { head[it] == CONQUEST_HEADER[it] }
}
