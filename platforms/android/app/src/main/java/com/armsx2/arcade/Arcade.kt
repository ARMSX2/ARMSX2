// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.arcade

import android.content.Context
import android.database.Cursor
import android.net.Uri
import android.provider.DocumentsContract
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import com.armsx2.i18n.I18n
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File

/**
 * Namco System 246/256 arcade games.
 *
 * The board itself is emulated in the core (pcsx2/DEV9/AC* and the hooks around them, ported from
 * PCSX2x6, https://github.com/PS2Homebrew-arcade/pcsx2x6, by Matías Israelson (El_isra), Tovarichtch
 * and DiscoStarslayer). A game is an .acgame: PCSX2x6's small INI that names the game ID and the
 * files around it, laid out the way PCSX2x6's game library template lays them out:
 *
 *     NM00004.acgame
 *     NM00004/boot.elf       the boot program
 *     NM00004/NM00004.chd    the game's disc or hard drive image
 *     memcards/NM00004.ps2   the security dongle
 *
 * Only the game ID is required; every other name has a default ([parse]). This is the Android half:
 * reading the .acgame, finding those files in a library folder (on Android usually a document tree,
 * which has no paths for the core to follow), putting the dongle where the core mounts memory cards
 * from (a dongle next to the .acgame is copied there once, so it need not be put there by hand), and
 * the session the on-screen controls follow.
 */
object Arcade {
    const val EXTENSION = "acgame"

    /** The library badge for an arcade game (GameInfo.extension). */
    const val BADGE = "ARCADE"

    /** The games that start but only reach their attract demo, in PCSX2x6 as here (its compatibility
     *  list, https://github.com/PS2Homebrew-arcade/pcsx2x6/issues/9): Dragon Chronicle, Dragon Chronicle
     *  Online and The IDOLM@STER. The NAMCO screen says so instead of "Ready to play". */
    val ATTRACT_ONLY = setOf("NM00014", "NM00020", "NM00022")

    private const val MAX_ACGAME_BYTES = 64 * 1024
    private val GAME_ID = Regex("NM\\d{5}")

    /** The running arcade game's controls (NativeApp.ARCADE_MODE_*), or -1 when none is running. */
    val sessionMode = mutableIntStateOf(-1)

    /** The running arcade game's ID, or null when none is running (its Arcade controls in the pause menu). */
    val sessionGameId = mutableStateOf<String?>(null)

    /** A message about the last arcade launch, shown over the library until dismissed. */
    val notice = mutableStateOf<String?>(null)

    /** Whether the touchscreen gun layer is the game's own control: its light gun, or its touch panel. */
    val usesGunLayer: Boolean
        get() = sessionMode.intValue == NativeApp.ARCADE_MODE_LIGHTGUN ||
            sessionMode.intValue == NativeApp.ARCADE_MODE_TOUCH

    data class AcGame(
        val gameId: String,
        val name: String,
        /** The folder the ELF, media and SRAM are in, beside the .acgame; empty for the .acgame's own. */
        val subdir: String,
        val elf: String,
        val mediaSrc: String,
        val sram: String,
        val dongle: String,
        /** A second memory card (Soul Calibur II's Conquest card), empty for none. */
        val card: String,
        val jvsMode: String,
    )

    /** What a launch found, to hand to the core (NativeApp.setArcadeLaunchFiles), and the .acgame it boots
     *  ([manifest]): the game's own, or the one [prepare] writes for a game kept as its own files. */
    data class Launch(
        val game: AcGame,
        val elf: String,
        val media: String,
        val sram: String,
        val mode: Int,
        val manifest: String,
    )

    fun isAcGameName(name: String?): Boolean = name?.endsWith(".$EXTENSION", ignoreCase = true) == true

    /** Whether a launch is an arcade game: by its name, or, for a document whose URI does not carry
     *  its name, by what the library found it to be. */
    fun isArcadeLaunch(path: String, game: com.armsx2.GameInfo?): Boolean =
        isAcGameName(path) || game?.extension == BADGE || looksLikeArcadeImage(path)

    /** An image named after an arcade game, by its path or a URI that carries its name: a launch the
     *  library did not make (Recently Played, another app) still goes to the board. */
    private fun looksLikeArcadeImage(path: String): Boolean {
        val name = Uri.decode(path).substringAfterLast('/').substringAfterLast(':')
        return ArcadeFiles.idIn(name) != null &&
            ArcadeFiles.kindOf(name) { ArcadeFiles.MAX_CARD_BYTES + 1 }.let { it == ArcadeFiles.Kind.IMAGE || it == ArcadeFiles.Kind.PACKED_IMAGE }
    }

    /**
     * Parses an .acgame the way the core's INI reader does: sections and keys in any case, `;` and
     * `#` comments, values trimmed. Null when it names no game ID (NM and five digits), the one entry
     * PCSX2x6 cannot do without. The defaults are PCSX2x6's, and the core's (VMManager::OpenArcadeGame).
     */
    fun parse(text: String): AcGame? {
        val values = HashMap<String, String>()
        var section = ""
        text.removePrefix("﻿").lineSequence().forEach { raw ->
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith(";") || line.startsWith("#")) return@forEach
            if (line.startsWith("[") && line.endsWith("]")) {
                section = line.substring(1, line.length - 1).trim().lowercase()
                return@forEach
            }
            val eq = line.indexOf('=')
            if (eq <= 0) return@forEach
            values["$section.${line.substring(0, eq).trim().lowercase()}"] = line.substring(eq + 1).trim()
        }
        val id = values["game.gameid"].orEmpty()
        if (!GAME_ID.matches(id)) return null
        fun named(key: String, default: String) = values["data.$key"]?.takeIf { it.isNotEmpty() } ?: default
        return AcGame(
            gameId = id,
            name = values["game.name"].orEmpty(),
            subdir = values["data.subdir"] ?: id,
            elf = named("elf", "boot.elf"),
            mediaSrc = named("mediasrc", "$id.chd"),
            sram = named("sram", "sram.bin"),
            dongle = named("dongle", "$id.ps2"),
            card = values["data.card"].orEmpty(),
            jvsMode = values["data.jvsmode"].orEmpty().lowercase(),
        )
    }

    /** Reads the .acgame at [location], a path or a content:// URI. */
    fun read(context: Context, location: String): AcGame? = runCatching {
        val bytes = if (location.startsWith("content://")) {
            context.contentResolver.openInputStream(Uri.parse(location))?.use { input ->
                // An .acgame is a few lines; anything this big is something else.
                val buffer = java.io.ByteArrayOutputStream()
                val chunk = ByteArray(8192)
                while (buffer.size() <= MAX_ACGAME_BYTES) {
                    val n = input.read(chunk)
                    if (n < 0) break
                    buffer.write(chunk, 0, n)
                }
                buffer.toByteArray()
            }
        } else {
            File(location).takeIf { it.isFile && it.length() <= MAX_ACGAME_BYTES }?.readBytes()
        }
        bytes?.takeIf { it.size <= MAX_ACGAME_BYTES }?.let { parse(String(it, Charsets.UTF_8)) }
    }.getOrNull()

    /** The cabinet controls (NativeApp.ARCADE_MODE_*) the game gets: from its game ID, unless the
     *  .acgame names some (jvsmode, as the core reads it). */
    fun modeOf(game: AcGame): Int = when (game.jvsMode) {
        "" -> runCatching { NativeApp.arcadeModeForGameId(game.gameId) }.getOrDefault(NativeApp.ARCADE_MODE_GENERIC)
        "lightgun" -> NativeApp.ARCADE_MODE_LIGHTGUN
        "fighting" -> NativeApp.ARCADE_MODE_FIGHTING
        "drum" -> NativeApp.ARCADE_MODE_DRUM
        "racing" -> NativeApp.ARCADE_MODE_RACING
        "standard" -> NativeApp.ARCADE_MODE_STANDARD
        "twinstick" -> NativeApp.ARCADE_MODE_TWINSTICK
        "touch" -> NativeApp.ARCADE_MODE_TOUCH
        else -> NativeApp.ARCADE_MODE_GENERIC
    }

    /** The folder the core mounts memory cards from. */
    fun memcardsDir(context: Context): File = File(MainActivityRuntime.assetCopyRoot(context), "memcards")

    /** Where a game's board settings (its SRAM, what the test menu saves) are kept: in the data folder,
     *  since a library folder is often one the core can read and not write. */
    private fun sramFile(context: Context, game: AcGame): File =
        File(File(File(MainActivityRuntime.assetCopyRoot(context), "arcade"), game.gameId), File(game.sram).name)

    /**
     * Gets an arcade game ready to boot: finds its files, puts its dongle (and card) in the memory
     * cards folder, seeds its SRAM, and hands the core where everything is. A failure says, in the
     * player's language, what is missing and where it was looked for.
     */
    fun prepare(context: Context, at: String): Result<Launch> = runCatching {
        val location = pathOf(at)
        val game = read(context, location) ?: return@runCatching prepareLoose(context, location)
        if (!hasArcadeBios(context)) fail("arcade.error.bios")
        requireBiosFor(game.gameId)
        val files = locate(context, location) ?: fail("arcade.error.notInLibrary")

        val found = files.find(game.elf, inSubdir = game.subdir) ?: fail("arcade.error.elf", game.elf)
        // The BIOS loads the boot program through host:, which reads from the boot program's own folder.
        // A content:// URI (a folder picked through Android's picker, as on the Play build) has no folder
        // the core can read, so the boot program goes to the core as a copy beside the game's SRAM, made
        // fresh every boot (it is small).
        val elf = if (!found.startsWith("content://")) found else {
            val copy = File(sramFile(context, game).parentFile, File(game.elf).name)
            if (!files.copy(found, copy)) fail("arcade.error.elf", game.elf)
            copy.absolutePath
        }
        val media = files.find(game.mediaSrc, inSubdir = game.subdir) ?: fail("arcade.error.media", game.mediaSrc)

        // Soul Calibur II's Conquest card, when its .acgame names none: the core takes it from the memory cards
        // folder by its name. Before the dongle, as a card 2.8.1 took for the dongle is moved out of its way.
        if (game.card.isEmpty() && game.gameId == ArcadeFiles.CONQUEST_GAME) conquestCard(context)

        // The dongle, and the second card if the game has one: in the memory cards folder, or copied
        // there from beside the .acgame.
        val cards = memcardsDir(context).apply { mkdirs() }
        for ((name, missingKey) in listOf(game.dongle to "arcade.error.dongle", game.card to "arcade.error.card")) {
            if (name.isEmpty()) continue
            val target = File(cards, File(name).name)
            if (target.isFile && target.length() > 0) continue
            val source = files.find(name, inSubdir = game.subdir, alsoBeside = true) ?: fail(missingKey, name)
            if (!files.copy(source, target)) fail(missingKey, name)
            println("@@ANDROID_ARCADE@@ copied ${File(name).name} into the memory cards folder")
        }

        // The SRAM is the core's to write; a file shipped beside the game (from a real board) seeds it once.
        val sram = sramFile(context, game)
        if (!sram.isFile) {
            sram.parentFile?.mkdirs()
            files.find(game.sram, inSubdir = game.subdir)?.let { files.copy(it, sram) }
        }

        Launch(game, elf, media, sram.absolutePath, modeOf(game), location)
    }

    /**
     * A game kept as its own files ([ArcadeFiles]): its image at [location], its dongle beside it. The
     * image is read where it is; the .acgame the core boots from is written into the app's storage, beside
     * the game's SRAM, with the boot program from the boot files.
     */
    private fun prepareLoose(context: Context, location: String): Launch {
        val files = locate(context, location) ?: fail("arcade.error.notInLibrary")
        val name = displayName(context, location) ?: fail("arcade.error.unreadable")
        val siblings = files.list()
        val folderId = ArcadeFiles.idIn(files.folderName())
        val id = ArcadeFiles.idIn(name) ?: folderId ?: fail("arcade.error.unreadable")
        val size = siblings.firstOrNull { it.first == name }?.second ?: 0L
        when (ArcadeFiles.kindOf(name) { size }) {
            ArcadeFiles.Kind.IMAGE -> Unit
            ArcadeFiles.Kind.PACKED_IMAGE -> fail("arcade.error.packed", name)
            else -> fail("arcade.error.unreadable")
        }
        if (!hasArcadeBios(context)) fail("arcade.error.bios")
        requireBiosFor(id)

        val dir = gameDir(context, id).apply { mkdirs() }
        val boot = ArcadeLibrary.bootProgram(context, id) ?: fail("arcade.error.bootFiles")
        val elf = File(dir, "boot.elf")
        if (!copyInto(elf) { boot.inputStream() }) fail("arcade.error.elf", elf.name)
        // Soul Calibur II's Conquest card before its dongle: it is a card file beside the game too, and 2.8.1
        // could take it for the dongle.
        val card = if (id == ArcadeFiles.CONQUEST_GAME) conquestCard(context, files, siblings, folderId) else null
        val dongle = looseDongle(context, files, siblings, id, folderId) ?: fail("arcade.error.looseDongle", id)

        val title = ArcadeLibrary.titles().firstOrNull { it.id == id }?.name?.takeIf { it.isNotBlank() } ?: id
        val manifest = File(dir, "$id.$EXTENSION")
        val text = "[game]\nname=$title\ngameid=$id\n\n[data]\nsubdir=\ndongle=$dongle\n" +
            card?.let { "card=$it\n" }.orEmpty()
        if (!copyInto(manifest) { text.byteInputStream() }) fail("arcade.error.unreadable")
        val game = AcGame(
            gameId = id, name = title, subdir = "", elf = elf.name, mediaSrc = name, sram = "sram.bin",
            dongle = dongle, card = card.orEmpty(), jvsMode = "",
        )
        return Launch(game, elf.absolutePath, location, sramFile(context, game).absolutePath, modeOf(game), manifest.absolutePath)
    }

    /** The app's own folder for the game [id]: its SRAM, and for a game kept as its own files the .acgame
     *  and boot program it boots from. */
    private fun gameDir(context: Context, id: String): File =
        File(File(MainActivityRuntime.assetCopyRoot(context), "arcade"), id)

    /**
     * The dongle of the game [id], in the memory cards folder: already there (from an earlier launch, or
     * put there by hand), else copied there from beside its image (unpacked, if it is a .gz). It is named
     * after the game and what it holds ([ArcadeFiles.dongleName]). Never Soul Calibur II's Conquest card, a
     * card file as well. Null when there is none.
     */
    private fun looseDongle(
        context: Context,
        files: Files,
        siblings: List<Pair<String, Long>>,
        id: String,
        folderId: String?,
    ): String? {
        val cards = memcardsDir(context).apply { mkdirs() }
        listOf("$id.ps2", "$id.bin").firstOrNull { File(cards, it).let { f -> f.isFile && f.length() > 0 && !isConquestCardIn(id, f) } }
            ?.let { return it }
        // A .ps2 first, then a .bin, then a packed one: the likelier a file is the dongle, the earlier.
        val candidates = siblings
            .filter { (n, bytes) -> (ArcadeFiles.idIn(n) ?: folderId) == id && ArcadeFiles.kindOf(n) { bytes } == ArcadeFiles.Kind.CARD }
            .filterNot { (n, _) -> isConquestCardBeside(files, id, n) }
            .sortedBy { (n, _) -> if (n.endsWith(".ps2", ignoreCase = true)) 0 else if (n.endsWith(".gz", ignoreCase = true)) 2 else 1 }
        for ((n, _) in candidates) {
            val source = files.find(n, inSubdir = "") ?: continue
            val temp = File(cards, ".$id.dongle")
            if (!files.copy(source, temp, unpack = n.endsWith(".gz", ignoreCase = true))) continue
            val bytes = temp.length()
            val target = File(cards, ArcadeFiles.dongleName(id, bytes))
            if (bytes in ArcadeFiles.MIN_CARD_BYTES..ArcadeFiles.MAX_CARD_BYTES && temp.renameTo(target)) {
                println("@@ANDROID_ARCADE@@ copied $n into the memory cards folder as ${target.name}")
                return target.name
            }
            temp.delete()
        }
        return null
    }

    // ---- Soul Calibur II's Conquest card ----------------------------------------------------------

    /** The blank Conquest card that comes with the app: bin/cardmaterial.bin of SC2MAKER
     *  (https://github.com/israpps/SC2MAKER, by Matías Israelson (El_isra), GPL-3.0), made from Conquest cards
     *  its users gave and dumped. Packed with gzip; unpacked, 8,650,752 bytes, SHA-256
     *  fe7eac4c5566fa4f16680e2ce9ea682215207f87688dabc4b9065a30050c71f8. Named .gzip, not .gz: the build
     *  unpacks a .gz asset into the APK under the name without it. */
    private const val CONQUEST_ASSET = "arcade/NM00007.conquestcard.gzip"

    /**
     * Soul Calibur II's Conquest card, which the game reads in slot 2: [ArcadeFiles.CONQUEST_CARD] in the
     * memory cards folder. It is put there once and kept from then on, as the game writes its Conquest mode
     * to it. It is the first of: the card already there; a Conquest card that 2.8.1 took for the dongle and
     * copied in as one (moved, since it never was the dongle); the player's own beside the game's image
     * ([files], [siblings]); else the blank card that comes with the app ([CONQUEST_ASSET]). Its name there,
     * or null when none could be put there: the game then starts without one, as before.
     */
    private fun conquestCard(
        context: Context,
        files: Files? = null,
        siblings: List<Pair<String, Long>> = emptyList(),
        folderId: String? = null,
    ): String? {
        val id = ArcadeFiles.CONQUEST_GAME
        val cards = memcardsDir(context).apply { mkdirs() }
        val target = File(cards, ArcadeFiles.CONQUEST_CARD)
        if (target.isFile && target.length() > 0) return target.name

        val stale = File(cards, "$id.ps2")
        if (stale.length() == ArcadeFiles.CONQUEST_CARD_BYTES && isConquestCardIn(id, stale) && stale.renameTo(target)) {
            println("@@ANDROID_ARCADE@@ ${stale.name} was the Conquest card, not the dongle: now ${target.name}")
            return target.name
        }
        if (files != null) {
            for ((n, bytes) in siblings) {
                if ((ArcadeFiles.idIn(n) ?: folderId) != id || ArcadeFiles.kindOf(n) { bytes } != ArcadeFiles.Kind.CARD) continue
                val source = files.find(n, inSubdir = "") ?: continue
                val packed = n.endsWith(".gz", ignoreCase = true)
                if (!ArcadeFiles.isConquestCard(files.head(source, ArcadeFiles.CONQUEST_HEADER_BYTES, packed))) continue
                if (files.copy(source, target, unpack = packed) && target.length() == ArcadeFiles.CONQUEST_CARD_BYTES) {
                    println("@@ANDROID_ARCADE@@ copied $n into the memory cards folder as ${target.name}")
                    return target.name
                }
                // Unreadable, or dumped without its spare bytes, which hold the game's checksums: not one it reads.
                println("@@ANDROID_ARCADE@@ $n could not be used as the Conquest card")
                target.delete()
            }
        }
        if (copyInto(target) { java.util.zip.GZIPInputStream(context.assets.open(CONQUEST_ASSET)) } &&
            target.length() == ArcadeFiles.CONQUEST_CARD_BYTES
        ) {
            println("@@ANDROID_ARCADE@@ put the blank Conquest card in the memory cards folder as ${target.name}")
            return target.name
        }
        target.delete()
        println("@@ANDROID_ARCADE@@ no Conquest card could be put in place; the game starts without one")
        return null
    }

    /** Whether [file] in the memory cards folder is Soul Calibur II's Conquest card, and not the dongle of the
     *  game [id]: only that game has one. */
    private fun isConquestCardIn(id: String, file: File): Boolean =
        id == ArcadeFiles.CONQUEST_GAME && file.isFile &&
            ArcadeFiles.isConquestCard(headOf(ArcadeFiles.CONQUEST_HEADER_BYTES, unpack = false) { file.inputStream() })

    /** The same for the card file [name] beside the game's image. */
    private fun isConquestCardBeside(files: Files, id: String, name: String): Boolean {
        if (id != ArcadeFiles.CONQUEST_GAME) return false
        val source = files.find(name, inSubdir = "") ?: return false
        return ArcadeFiles.isConquestCard(
            files.head(source, ArcadeFiles.CONQUEST_HEADER_BYTES, unpack = name.endsWith(".gz", ignoreCase = true)),
        )
    }

    /** A location as the rest of this works with it: a path, or a content:// URI. The library lists a
     *  game in a folder it reads directly by a file:// URI. */
    private fun pathOf(location: String): String =
        if (location.startsWith("file://")) Uri.parse(location).path ?: location else location

    /** A document's or a file's own name. */
    private fun displayName(context: Context, location: String): String? {
        if (!location.startsWith("content://")) return File(location).name
        return runCatching {
            context.contentResolver.query(
                Uri.parse(location), arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null,
            )?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
        }.getOrNull()
    }

    /**
     * What keeps the arcade game at [location] from starting, in the player's words (empty when nothing
     * does): the checks [prepare] makes, without copying anything. For the NAMCO screen's list.
     */
    fun missing(context: Context, at: String): List<String> = runCatching {
        val location = pathOf(at)
        val parts = ArrayList<String>()
        val bios = hasArcadeBios(context)
        if (!bios) parts.add(I18n.get("arcade.part.bios"))
        // An arcade BIOS the game does not run on is none for it (Battle Gear 3 and the System 256 one).
        fun biosFor(id: String) {
            if (bios) biosNeed(id).takeIf { it.isNotEmpty() }?.let { parts.add(I18n.get("arcade.part.biosBoard").format(it)) }
        }
        val files = locate(context, location) ?: return@runCatching parts
        val game = read(context, location)
        val cards = memcardsDir(context)
        fun inCards(name: String) = File(cards, File(name).name).let { it.isFile && it.length() > 0 }
        if (game != null) {
            biosFor(game.gameId)
            if (files.find(game.elf, inSubdir = game.subdir) == null) parts.add(I18n.get("arcade.part.boot"))
            if (!inCards(game.dongle) && files.find(game.dongle, inSubdir = game.subdir, alsoBeside = true) == null)
                parts.add(I18n.get("arcade.part.dongle"))
            if (game.card.isNotEmpty() && !inCards(game.card) && files.find(game.card, inSubdir = game.subdir, alsoBeside = true) == null)
                parts.add(I18n.get("arcade.part.card"))
            return@runCatching parts
        }
        val name = displayName(context, location) ?: return@runCatching parts
        val siblings = files.list()
        val folderId = ArcadeFiles.idIn(files.folderName())
        val id = ArcadeFiles.idIn(name) ?: folderId ?: return@runCatching parts
        biosFor(id)
        if (id !in ArcadeLibrary.bootGames(context)) parts.add(I18n.get("arcade.part.boot"))
        val size = siblings.firstOrNull { it.first == name }?.second ?: 0L
        if (ArcadeFiles.kindOf(name) { size } == ArcadeFiles.Kind.PACKED_IMAGE) parts.add(I18n.get("arcade.part.unpacked"))
        // Soul Calibur II's Conquest card is a card file too, and never its dongle.
        fun dongleIn(n: String) = inCards(n) && !isConquestCardIn(id, File(cards, n))
        val dongle = dongleIn("$id.ps2") || dongleIn("$id.bin") || siblings.any { (n, bytes) ->
            (ArcadeFiles.idIn(n) ?: folderId) == id && ArcadeFiles.kindOf(n) { bytes } == ArcadeFiles.Kind.CARD &&
                !isConquestCardBeside(files, id, n)
        }
        if (!dongle) parts.add(I18n.get("arcade.part.dongle"))
        parts
    }.getOrDefault(emptyList())

    /** Whether the BIOS folder has an arcade board's BIOS for the core to boot (VMManager picks it). */
    private fun hasArcadeBios(context: Context): Boolean {
        val dir = MainActivityRuntime.internalBiosDir(context)
        fun arcade(file: File) = file.isFile && file.length() in ARCADE_BIOS_SIZES &&
            runCatching { NativeApp.isArcadeBios(file.absolutePath) }.getOrDefault(false)
        return dir.listFiles()?.any(::arcade) == true
    }

    /** Fails when the game [id] runs on none of the arcade BIOS files there, naming the one it needs. */
    private fun requireBiosFor(id: String) {
        biosNeed(id).takeIf { it.isNotEmpty() }?.let { fail("arcade.error.biosBoard", it) }
    }

    /** The board whose BIOS the game [id] needs when none of the arcade BIOS files there runs it ("System
     *  246" for Battle Gear 3, which rejects the System 256 one), else "". The core's own choice at boot. */
    private fun biosNeed(id: String): String =
        if (!MainActivityRuntime.nativeReady.value) "" else runCatching { NativeApp.getArcadeBiosNeed(id) }.getOrDefault("")

    /** What the core takes as an arcade BIOS (BiosTools' MIN_ARCADE_BIOS_SIZE to MAX_BIOS_SIZE): the
     *  boards' BIOS is a 2 MB flash chip, dumped that way (MAME's sys246/sys256, r27v1602f.*). */
    val ARCADE_BIOS_SIZES = (2L shl 20)..(8L shl 20)

    private fun fail(key: String, vararg args: Any): Nothing =
        throw IllegalStateException(if (args.isEmpty()) I18n.get(key) else I18n.get(key).format(*args))

    /** The files around one .acgame, wherever its folder is. */
    private interface Files {
        /** The file named [name] in [inSubdir] beside the .acgame ("" for its own folder), and, when
         *  [alsoBeside], in the .acgame's own folder after that. A path or a content:// URI. */
        fun find(name: String, inSubdir: String, alsoBeside: Boolean = false): String?

        /** Copies [source] to [target], unpacking it on the way when [unpack] (a .gz). */
        fun copy(source: String, target: File, unpack: Boolean = false): Boolean

        /** The first [n] bytes of [source], unpacked when [unpack] (a .gz); fewer when there are fewer. */
        fun head(source: String, n: Int, unpack: Boolean): ByteArray

        /** The files in the game's own folder (the .acgame's or the image's), with their sizes (0 when the
         *  provider does not say). */
        fun list(): List<Pair<String, Long>>

        /** That folder's own name. */
        fun folderName(): String?
    }

    private fun locate(context: Context, location: String): Files? {
        if (!location.startsWith("content://"))
            return File(location).parentFile?.let { PathFiles(it) }
        val uri = Uri.parse(location)
        // A document in a library folder: walk that folder's tree.
        treeFiles(context, uri)?.let { return it }
        // A lone document (a frontend's launch): its path, when the app can read storage directly.
        val posix = posixOf(uri)?.let(::File)?.takeIf { it.isFile }
        return posix?.parentFile?.let { PathFiles(it) }
    }

    /** A document of the external storage provider, as the path it stands for ("primary:Games/x"). */
    private fun posixOf(uri: Uri): String? = runCatching {
        if (uri.authority != "com.android.externalstorage.documents") return null
        val parts = DocumentsContract.getDocumentId(uri).split(":", limit = 2)
        if (parts.size != 2) return null
        if (parts[0] == "primary") "/storage/emulated/0/${parts[1]}" else "/storage/${parts[0]}/${parts[1]}"
    }.getOrNull()

    private class PathFiles(private val dir: File) : Files {
        override fun find(name: String, inSubdir: String, alsoBeside: Boolean): String? {
            File(name).takeIf { it.isAbsolute && it.isFile }?.let { return it.absolutePath }
            val folders = buildList {
                add(if (inSubdir.isEmpty()) dir else File(dir, inSubdir))
                if (alsoBeside && inSubdir.isNotEmpty()) add(dir)
            }
            for (folder in folders) {
                File(folder, name).takeIf { it.isFile }?.let { return it.absolutePath }
                // Names in an .acgame are often typed by hand; take a file that differs only in case.
                folder.listFiles()?.firstOrNull { it.isFile && it.name.equals(name, ignoreCase = true) }
                    ?.let { return it.absolutePath }
            }
            return null
        }

        override fun copy(source: String, target: File, unpack: Boolean): Boolean =
            copyInto(target) { File(source).inputStream().let { if (unpack) java.util.zip.GZIPInputStream(it) else it } }

        override fun head(source: String, n: Int, unpack: Boolean): ByteArray =
            headOf(n, unpack) { File(source).inputStream() }

        override fun list(): List<Pair<String, Long>> =
            dir.listFiles()?.filter { it.isFile }?.map { it.name to it.length() }.orEmpty()

        override fun folderName(): String? = dir.name
    }

    private fun treeFiles(context: Context, uri: Uri): Files? = runCatching {
        val treeId = DocumentsContract.getTreeDocumentId(uri)
        val treeUri = DocumentsContract.buildTreeDocumentUri(uri.authority, treeId)
        val parentId = parentDocumentId(context, uri, treeId) ?: return null
        TreeFiles(context, treeUri, parentId)
    }.getOrNull()

    /** The folder a document in a tree is in. findDocumentPath walks the tree whatever the provider's
     *  document IDs look like; the external storage provider's are paths, which also do. */
    private fun parentDocumentId(context: Context, uri: Uri, treeId: String): String? {
        runCatching { DocumentsContract.findDocumentPath(context.contentResolver, uri)?.path }.getOrNull()
            ?.let { path -> if (path.size >= 2) return path[path.size - 2] }
        val id = DocumentsContract.getDocumentId(uri)
        val slash = id.lastIndexOf('/')
        return if (slash > 0) id.substring(0, slash) else treeId
    }

    private class TreeFiles(
        private val context: Context,
        private val treeUri: Uri,
        private val parentId: String,
    ) : Files {
        override fun find(name: String, inSubdir: String, alsoBeside: Boolean): String? {
            val folders = buildList {
                if (inSubdir.isEmpty()) add(parentId) else child(parentId, inSubdir, directory = true)?.let(::add)
                if (alsoBeside && inSubdir.isNotEmpty()) add(parentId)
            }
            for (folder in folders) {
                child(folder, name, directory = false)?.let {
                    return DocumentsContract.buildDocumentUriUsingTree(treeUri, it).toString()
                }
            }
            return null
        }

        /** The document called [name] in folder [folderId], matching case only when it has to. */
        private fun child(folderId: String, name: String, directory: Boolean): String? {
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, folderId)
            val projection = arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
            )
            var caseless: String? = null
            runCatching {
                context.contentResolver.query(children, projection, null, null, null)?.use { c: Cursor ->
                    while (c.moveToNext()) {
                        val isDir = c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR
                        if (isDir != directory) continue
                        val display = c.getString(1) ?: continue
                        if (display == name) return c.getString(0)
                        if (caseless == null && display.equals(name, ignoreCase = true)) caseless = c.getString(0)
                    }
                }
            }
            return caseless
        }

        override fun copy(source: String, target: File, unpack: Boolean): Boolean = copyInto(target) {
            val input = context.contentResolver.openInputStream(Uri.parse(source)) ?: error("unreadable")
            if (unpack) java.util.zip.GZIPInputStream(input) else input
        }

        override fun head(source: String, n: Int, unpack: Boolean): ByteArray = headOf(n, unpack) {
            context.contentResolver.openInputStream(Uri.parse(source)) ?: error("unreadable")
        }

        override fun list(): List<Pair<String, Long>> = runCatching {
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentId)
            val projection = arrayOf(
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_SIZE,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
            )
            context.contentResolver.query(children, projection, null, null, null)?.use { c: Cursor ->
                buildList {
                    while (c.moveToNext()) {
                        if (c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR) continue
                        val display = c.getString(0) ?: continue
                        add(display to (if (c.isNull(1)) 0L else c.getLong(1)))
                    }
                }
            }
        }.getOrNull().orEmpty()

        override fun folderName(): String? = runCatching {
            val folder = DocumentsContract.buildDocumentUriUsingTree(treeUri, parentId)
            context.contentResolver.query(folder, arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)
                ?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
        }.getOrNull()
    }

    // ---- The arcade BIOS ---------------------------------------------------------------------------

    /**
     * There is no arcade BIOS to pick: every one in the BIOS folder is in use (the core's FindArcadeBiosFor),
     * the System 256 one for every game (PCSX2x6's default) and the System 246 one for a game that refuses
     * it (Battle Gear 3). Never the console BIOS: an arcade BIOS cannot run console
     * games, nor the other way. 2.8 had one picked for every arcade game; that pick is forgotten, in the
     * app's preferences and in the core's settings ([Filenames] ArcadeBIOS), once the core is up.
     */
    fun forgetArcadeBiosPick() {
        MainActivityRuntime.prefs.edit().remove("arcadeBios").apply()
        if (!MainActivityRuntime.nativeReady.value) return
        runCatching {
            NativeApp.setSetting("Filenames", "ArcadeBIOS", "string", "")
            NativeApp.commitSettings()
        }
    }

    /** Whether a BIOS, as the core describes it, is an arcade board's. */
    fun isArcadeBios(info: com.armsx2.BiosInfo): Boolean = info.zone == "COH-H"

    // ---- The cabinet's own buttons, for the in-game menu -------------------------------------------

    private val mainHandler by lazy { android.os.Handler(android.os.Looper.getMainLooper()) }
    private val releaseService = Runnable { runCatching { NativeApp.arcadeService(false) } }

    /** A coin in player 1's (0) or player 2's (1) slot. Select on that player's controller does the same. */
    fun insertCoin(player: Int) {
        runCatching { NativeApp.arcadeInsertCoin(player) }
    }

    /** A press of the cabinet's Service button, held long enough for the game to see it. Called with
     *  the game running again: a press made while it is paused would be over before it looks. */
    fun pressService() {
        mainHandler.removeCallbacks(releaseService)
        runCatching { NativeApp.arcadeService(true) }
        mainHandler.postDelayed(releaseService, SERVICE_PRESS_MS)
    }

    /** Flips the board's Test switch: the game's test menu (its settings and input tests), or out of it. */
    fun toggleTest() {
        runCatching { NativeApp.arcadeToggleTest() }
    }

    /** Several JVS polls (one a frame). */
    private const val SERVICE_PRESS_MS = 200L

    /** The first [n] bytes of what [open] opens, unpacked first when [unpack] (a .gz): fewer when there are
     *  fewer, none when it cannot be read. */
    private fun headOf(n: Int, unpack: Boolean, open: () -> java.io.InputStream): ByteArray = runCatching {
        open().use { raw ->
            val input = if (unpack) java.util.zip.GZIPInputStream(raw) else raw
            input.use {
                val bytes = ByteArray(n)
                var got = 0
                while (got < n) {
                    val read = it.read(bytes, got, n - got)
                    if (read < 0) break
                    got += read
                }
                bytes.copyOf(got)
            }
        }
    }.getOrDefault(ByteArray(0))

    /** Writes a temporary file and renames it over [target], so a failed copy leaves nothing behind. */
    private fun copyInto(target: File, open: () -> java.io.InputStream): Boolean = runCatching {
        target.parentFile?.mkdirs()
        val temp = File(target.parentFile, ".${target.name}.copying")
        open().use { input -> temp.outputStream().use { input.copyTo(it) } }
        if (temp.length() == 0L || !temp.renameTo(target)) {
            temp.delete()
            error("copy failed")
        }
        true
    }.getOrDefault(false)
}
