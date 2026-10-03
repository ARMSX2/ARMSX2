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

    private const val MAX_ACGAME_BYTES = 64 * 1024
    private val GAME_ID = Regex("NM\\d{5}")

    /** The running arcade game's controls (NativeApp.ARCADE_MODE_*), or -1 when none is running. */
    val sessionMode = mutableIntStateOf(-1)

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

    /** What a launch found, to hand to the core (NativeApp.setArcadeLaunchFiles). */
    data class Launch(val game: AcGame, val elf: String, val media: String, val sram: String, val mode: Int)

    fun isAcGameName(name: String?): Boolean = name?.endsWith(".$EXTENSION", ignoreCase = true) == true

    /** Whether a launch is an arcade game: by its name, or, for a document whose URI does not carry
     *  its name, by what the library found it to be. */
    fun isArcadeLaunch(path: String, game: com.armsx2.GameInfo?): Boolean =
        isAcGameName(path) || game?.extension == BADGE

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
    fun prepare(context: Context, location: String): Result<Launch> = runCatching {
        val game = read(context, location) ?: fail("arcade.error.unreadable")
        if (!hasArcadeBios(context)) fail("arcade.error.bios")
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

        Launch(game, elf, media, sram.absolutePath, modeOf(game))
    }

    /** Whether the BIOS folder has an arcade board's BIOS for the core to boot (VMManager picks it). */
    private fun hasArcadeBios(context: Context): Boolean {
        val dir = MainActivityRuntime.internalBiosDir(context)
        fun arcade(file: File) = file.isFile && file.length() in ARCADE_BIOS_SIZES &&
            runCatching { NativeApp.isArcadeBios(file.absolutePath) }.getOrDefault(false)
        arcadeBios.value?.let { if (arcade(File(dir, it))) return true }
        return dir.listFiles()?.any(::arcade) == true
    }

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

        fun copy(source: String, target: File): Boolean
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

        override fun copy(source: String, target: File): Boolean =
            copyInto(target) { File(source).inputStream() }
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

        override fun copy(source: String, target: File): Boolean =
            copyInto(target) { context.contentResolver.openInputStream(Uri.parse(source)) ?: error("unreadable") }
    }

    // ---- The arcade BIOS ---------------------------------------------------------------------------

    private const val PREF_ARCADE_BIOS = "arcadeBios"

    /**
     * The BIOS file (in the BIOS folder) the player picked for arcade games, or null to let the core
     * choose: the best arcade (COH-H) BIOS it finds, the System 256 one first, which runs System 246
     * games too. Never the console BIOS: an arcade BIOS cannot run console games, nor the other way.
     */
    val arcadeBios = mutableStateOf<String?>(null)

    fun loadArcadeBios() {
        arcadeBios.value = MainActivityRuntime.prefs.getString(PREF_ARCADE_BIOS, null)?.takeIf { it.isNotBlank() }
    }

    fun setArcadeBios(fileName: String?) {
        val name = fileName?.takeIf { it.isNotBlank() }
        arcadeBios.value = name
        MainActivityRuntime.prefs.edit().apply {
            if (name == null) remove(PREF_ARCADE_BIOS) else putString(PREF_ARCADE_BIOS, name)
        }.apply()
        pushArcadeBios()
    }

    /** Hands the core the arcade BIOS choice ([Filenames] ArcadeBIOS), once its settings exist. */
    fun pushArcadeBios() {
        if (!MainActivityRuntime.nativeReady.value) return
        runCatching {
            NativeApp.setSetting("Filenames", "ArcadeBIOS", "string", arcadeBios.value.orEmpty())
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
