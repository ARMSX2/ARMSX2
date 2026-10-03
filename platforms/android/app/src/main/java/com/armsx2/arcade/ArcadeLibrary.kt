// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.arcade

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import androidx.core.content.edit
import androidx.documentfile.provider.DocumentFile
import com.armsx2.BuildConfig
import com.armsx2.TextureCatalog
import com.armsx2.i18n.I18n
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.util.zip.GZIPInputStream
import java.util.zip.ZipException
import java.util.zip.ZipFile

/**
 * The arcade games folder the NAMCO System 246/256 screen keeps: where it is, the boot files it
 * fills games from, and importing a game into it in PCSX2x6's library layout, the one its game
 * library template makes, so the folder also works as it is in PCSX2x6 on a PC:
 *
 *     <folder>/NM00004.acgame
 *     <folder>/NM00004.ps2           the dongle (also copied into the memory cards folder)
 *     <folder>/NM00004/boot.elf      from the boot files
 *     <folder>/NM00004/NM00004.chd   the image
 *
 * The boot files are Proverb (https://github.com/PS2Homebrew-arcade/proverb, AFL-3.0), the
 * PS2Homebrew-arcade bootloader the PCSX2x6 template is made from: one zip holding bin/<id>/boot.elf
 * for every game. It is downloaded once into the app's own storage, and each import takes its game's
 * ELF from it, so only the games the player has end up in their library.
 */
object ArcadeLibrary {
    private const val TAG = "ARMSX2-Arcade"
    private const val PREF_FOLDER = "arcade.folder"
    private const val PREF_BOOT_TIME = "arcade.bootFilesTime"
    private const val BOOT_FILES_URL = "https://github.com/PS2Homebrew-arcade/proverb/releases/download/nightly/proverb.zip"
    private const val MAX_BOOT_FILES_BYTES = 64L shl 20
    private const val COPY_BUFFER = 1 shl 20
    /** Above any memory card file (the biggest the core makes, 64 MB with its ECC, is 66 MB): only a
     *  wrong pick, a game image say, is bigger, and reading that whole would run out of memory. */
    private const val MAX_CARD_BYTES = 72L shl 20
    /** The smallest PS2 memory card (8 MB without its ECC). Arcade sets carry small board ROMs that are
     *  easily taken for the dongle; anything smaller than this is not one. */
    private const val MIN_CARD_BYTES = 8L shl 20
    /** An .acgame is a few lines (Arcade's own limit). */
    private const val MAX_ACGAME_BYTES = 64 * 1024
    private const val OCTET_STREAM = "application/octet-stream"
    private val BOOT_ENTRY = Regex("""(?:^|/)bin/(NM\d{5})/boot\.elf$""", RegexOption.IGNORE_CASE)

    /** An arcade game the database knows. [board] is its region field (System246, System256,
     *  System SUPER256), [media] CD, DVD or HDD. */
    data class Title(val id: String, val name: String, val board: String, val media: String)

    /** An imported game (an .acgame in the arcade folder with its image), and what it still lacks.
     *  [board] and [media] are the database's, blank for a game it does not know. */
    data class Installed(val id: String, val name: String, val board: String, val media: String, val missing: List<Part>)

    /** A game's files: its image (disc or hard drive), its dongle, Soul Calibur II's Conquest card, its
     *  boot program. The first three are imported one at a time; the boot program comes with each. */
    enum class Part { IMAGE, DONGLE, CARD, BOOT }

    /** An import the player cancelled (the progress window's Cancel); nothing of it is left behind. */
    class ImportCancelled : Exception("import cancelled")

    /** What of a game is in the arcade folder: whether its .acgame is ([exists]), and the file names of
     *  its image, dongle and Conquest card where [Arcade.prepare] will find them, null for each one that
     *  is not there. */
    data class GameFiles(val id: String, val exists: Boolean, val image: String?, val dongle: String?, val card: String?)

    // ---- The folder --------------------------------------------------------------------------------

    fun folder(): Uri? =
        MainActivityRuntime.prefs.getString(PREF_FOLDER, null)?.takeIf { it.isNotBlank() }?.let(Uri::parse)

    fun folderName(context: Context): String? =
        folder()?.let { uri -> runCatching { DocumentFile.fromTreeUri(context, uri)?.name }.getOrNull() }

    /**
     * Makes [uri] the arcade folder: keeps read and write access to it across restarts, and puts it
     * in the game library unless a library folder already holds it (scanning both would list every
     * game twice).
     */
    fun setFolder(context: Context, uri: Uri) {
        runCatching {
            context.contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
            )
        }
        MainActivityRuntime.prefs.edit { putString(PREF_FOLDER, uri.toString()) }
        val dirs = MainActivityRuntime.romsDirs.value
        if (dirs.none { holds(it, uri) })
            MainActivityRuntime.setRomsDirs(dirs + uri.toString())
    }

    /** Whether the library folder [libraryDir] is, or holds, [folder]. Document IDs of the external
     *  storage provider are paths ("primary:Games/Arcade"), which is what makes this answerable. */
    private fun holds(libraryDir: String, folder: Uri): Boolean = runCatching {
        if (libraryDir == folder.toString()) return true
        val library = Uri.parse(libraryDir)
        if (library.authority != folder.authority) return false
        val outer = DocumentsContract.getTreeDocumentId(library)
        val inner = DocumentsContract.getTreeDocumentId(folder)
        inner == outer || inner.startsWith(outer.trimEnd('/') + "/") || (outer.endsWith(":") && inner.startsWith(outer))
    }.getOrDefault(false)

    // ---- The boot files ----------------------------------------------------------------------------

    private fun bootFiles(context: Context): File = File(File(context.filesDir, "arcade"), "proverb.zip")

    /** When the boot files were downloaded (ms), or 0 for never. */
    fun bootFilesTime(): Long = MainActivityRuntime.prefs.getLong(PREF_BOOT_TIME, 0L)

    /** The game IDs the downloaded boot files have a boot program for. */
    fun bootGames(context: Context): Set<String> = runCatching {
        ZipFile(bootFiles(context)).use { zip ->
            zip.entries().asSequence().mapNotNull { BOOT_ENTRY.find(it.name)?.groupValues?.get(1)?.uppercase() }.toSet()
        }
    }.getOrDefault(emptySet())

    /** Downloads the boot files (about 1 MB). Kept only if it really holds boot programs. */
    suspend fun downloadBootFiles(context: Context, onProgress: (Float) -> Unit): Boolean = withContext(Dispatchers.IO) {
        val target = bootFiles(context)
        target.parentFile?.mkdirs()
        val part = File(target.parentFile, target.name + ".part")
        val conn = TextureCatalog.RedirectingHttps.open(BOOT_FILES_URL, connectTimeoutMs = 15_000, readTimeoutMs = 30_000, tag = TAG) {
            setRequestProperty("User-Agent", "ARMSX2/" + runCatching { BuildConfig.VERSION_NAME }.getOrDefault("dev"))
            setRequestProperty("Accept-Encoding", "identity")
        } ?: return@withContext false
        val got = try {
            if (conn.responseCode != HttpURLConnection.HTTP_OK) {
                false
            } else {
                val total = conn.contentLengthLong
                if (total > MAX_BOOT_FILES_BYTES) {
                    false
                } else {
                    conn.inputStream.use { input -> part.outputStream().use { out -> copy(input, out, total, MAX_BOOT_FILES_BYTES, onProgress) } }
                    true
                }
            }
        } catch (e: IOException) {
            println("@@ANDROID_ARCADE@@ boot files download failed: $e")
            false
        } finally {
            conn.disconnect()
        }
        val programs = if (got) runCatching {
            ZipFile(part).use { zip -> zip.entries().asSequence().count { BOOT_ENTRY.containsMatchIn(it.name) } }
        }.getOrDefault(0) else 0
        if (programs == 0 || !(part.renameTo(target) || (target.delete() && part.renameTo(target)))) {
            part.delete()
            return@withContext false
        }
        MainActivityRuntime.prefs.edit { putLong(PREF_BOOT_TIME, System.currentTimeMillis()) }
        println("@@ANDROID_ARCADE@@ boot files downloaded: $programs games")
        true
    }

    // ---- The games -------------------------------------------------------------------------------

    /** Every arcade game the database knows, by name. Empty until the core is up: the database
     *  loads once per process, and a load before the core knows its resources folder stays empty. */
    fun titles(): List<Title> = (if (!MainActivityRuntime.nativeReady.value) "" else runCatching { NativeApp.getArcadeGames() }.getOrDefault(""))
        .lineSequence()
        .mapNotNull { line ->
            val f = line.split('\t')
            if (f.size < 4 || f[0].isBlank()) null else Title(f[0], f[1].ifBlank { f[0] }, f[2], f[3])
        }
        .sortedBy { it.name.lowercase() }
        .toList()

    /** The board, as the screen names it. */
    fun boardName(board: String): String = when (board) {
        "System256" -> I18n.get("arcade.board.256")
        "System SUPER256" -> I18n.get("arcade.board.super256")
        else -> I18n.get("arcade.board.246")
    }

    private class Child(val file: DocumentFile, val name: String, val isDirectory: Boolean)

    /** The arcade folder's files and folders. Every name and kind is a provider query, so each one's
     *  is asked for once, here. */
    private fun children(context: Context): List<Child> {
        val root = folder()?.let { DocumentFile.fromTreeUri(context, it) } ?: return emptyList()
        return runCatching { root.listFiles().map { Child(it, it.name.orEmpty(), it.isDirectory) } }.getOrDefault(emptyList())
    }

    /** Which of [game]'s files are where [Arcade.prepare] looks for them: among [children] (the arcade
     *  folder's), and for a memory card file also in the memory cards folder. */
    private class Present(context: Context, children: List<Child>, private val game: Arcade.AcGame) {
        private val beside = children.filter { !it.isDirectory }.map { it.name.lowercase() }.toSet()
        private val inDir = if (game.subdir.isEmpty()) beside else children
            .firstOrNull { it.isDirectory && it.name.equals(game.subdir, ignoreCase = true) }
            ?.let { d -> runCatching { d.file.listFiles().mapNotNull { it.name?.lowercase() }.toSet() }.getOrNull() }
            .orEmpty()
        private val cards = Arcade.memcardsDir(context)

        val image: Boolean get() = game.mediaSrc.lowercase() in inDir
        val boot: Boolean get() = game.elf.lowercase() in inDir

        fun card(name: String): Boolean {
            val file = File(name).name
            return File(cards, file).let { it.isFile && it.length() > 0 } ||
                file.lowercase() in beside || file.lowercase() in inDir
        }
    }

    /** The games imported into the arcade folder (an .acgame whose image is there, as the library
     *  lists them), with what each one still lacks: where [Arcade.prepare] looks for it, it is not.
     *  [titles] names the games whose .acgame has none. */
    fun installed(context: Context, titles: List<Title>): List<Installed> {
        val children = children(context)
        val db = titles.associateBy { it.id }
        return children.mapNotNull { child ->
            if (child.isDirectory || !Arcade.isAcGameName(child.name)) return@mapNotNull null
            val game = Arcade.read(context, child.file.uri.toString()) ?: return@mapNotNull null
            val present = Present(context, children, game)
            // Without its image it is not imported yet (PCSX2x6's template writes an .acgame for every game).
            if (!present.image) return@mapNotNull null
            val missing = buildList {
                if (!present.card(game.dongle)) add(Part.DONGLE)
                if (game.card.isNotEmpty() && !present.card(game.card)) add(Part.CARD)
                if (!present.boot) add(Part.BOOT)
            }
            val known = db[game.gameId]
            Installed(
                game.gameId, game.name.ifBlank { known?.name ?: game.gameId },
                known?.board.orEmpty(), known?.media.orEmpty(), missing,
            )
        }.sortedBy { it.name.lowercase() }
    }

    /** What of the game [id] is in the arcade folder, by its .acgame: none of it without one. */
    fun files(context: Context, id: String): GameFiles {
        val children = children(context)
        val acgame = children.firstOrNull { !it.isDirectory && it.name.equals("$id.${Arcade.EXTENSION}", ignoreCase = true) }
            ?: return GameFiles(id, false, null, null, null)
        val game = Arcade.read(context, acgame.file.uri.toString()) ?: return GameFiles(id, true, null, null, null)
        val present = Present(context, children, game)
        return GameFiles(
            id,
            exists = true,
            image = game.mediaSrc.takeIf { present.image },
            dongle = game.dongle.takeIf { present.card(it) },
            card = game.card.takeIf { it.isNotEmpty() && present.card(it) },
        )
    }

    /**
     * Uninstalls the game [id] from the arcade folder: the files its .acgame names there (its image and
     * boot program, its dongle and card), anything else named for it in its own folder (an older image a
     * later import replaced), what an import cut short left of them (<name>.part), its folder once that
     * is empty, then the .acgame, last, so an uninstall cut short is finished from the same place. Nothing
     * else: the memory cards folder's copies of its cards and its SRAM (its board settings) stay, for if
     * it is imported again. [onProgress] goes file by file; a failure's message names the file.
     */
    suspend fun uninstall(context: Context, id: String, onProgress: (Float) -> Unit): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            val root = folder()?.let { DocumentFile.fromTreeUri(context, it) }
            if (root == null || !root.canWrite()) fail("arcade.import.error.folder")
            val children = children(context)
            val acgameName = "$id.${Arcade.EXTENSION}"
            val acgame = children.firstOrNull { !it.isDirectory && it.name.equals(acgameName, ignoreCase = true) }
                ?: return@runCatching onProgress(1f)
            val game = Arcade.read(context, acgame.file.uri.toString())

            fun names(vararg files: String?): Set<String> = files.filterNotNull().filter { it.isNotEmpty() }
                .map { File(it).name.lowercase() }.flatMap { listOf(it, "$it.part", "$it.old") }.toSet()
            val cards = names(game?.dongle, game?.card)
            val own = names(game?.mediaSrc, game?.elf)
            val subdir = game?.subdir.orEmpty()
            val dir = subdir.takeIf { it.isNotEmpty() }
                ?.let { s -> children.firstOrNull { it.isDirectory && it.name.equals(s, ignoreCase = true) } }
            // In the template's folder, named for the game, everything named for the game is the game's.
            val template = dir != null && subdir.equals(id, ignoreCase = true)
            val prefix = "${id.lowercase()}."
            val inDir = dir?.let { d ->
                runCatching { d.file.listFiles().map { Child(it, it.name.orEmpty(), it.isDirectory) } }.getOrDefault(emptyList())
            }.orEmpty().filter { f ->
                val n = f.name.lowercase()
                !f.isDirectory && (n in own || n in cards || (template && n.startsWith(prefix)))
            }
            val besideNames = cards + (if (subdir.isEmpty()) own else emptySet()) +
                "${acgameName.lowercase()}.part" + "${acgameName.lowercase()}.old"
            val beside = children.filter { !it.isDirectory && it.name.lowercase() in besideNames }

            val doomed = inDir + beside
            val steps = doomed.size + 2  // and its folder, and its .acgame
            var done = 0
            for (f in doomed) {
                deleteDocument(context, f.file, f.name)
                onProgress(++done / steps.toFloat())
            }
            if (dir != null && runCatching { dir.file.listFiles().isEmpty() }.getOrDefault(false))
                deleteDocument(context, dir.file, dir.name)
            onProgress(++done / steps.toFloat())
            deleteDocument(context, acgame.file, acgame.name)
            onProgress(1f)
            println("@@ANDROID_ARCADE@@ uninstalled $id (${doomed.size + 1} files)")
        }
    }

    private fun deleteDocument(context: Context, file: DocumentFile, name: String) {
        if (!runCatching { DocumentsContract.deleteDocument(context.contentResolver, file.uri) }.getOrDefault(false))
            fail("arcade.uninstall.error", name)
    }

    /**
     * Imports one [part] of [title] into the arcade folder from [source]: its image (copied, which for a
     * hard drive or DVD image takes a while; [onProgress] follows it), its dongle, or Soul Calibur II's
     * Conquest card, each on its own and in any order, with the boot program alongside. The game's
     * .acgame names its files: a new one is PCSX2x6's template, naming a part not imported yet as the
     * core assumes it is called; in one that is there already only the imported part's line changes.
     * The dongle and card also go into the memory cards folder, where the core mounts them from,
     * replacing an older copy there only after keeping it as a .bak. A failure's message says what went
     * wrong, in the player's language.
     */
    suspend fun import(
        context: Context,
        title: Title,
        part: Part,
        source: Uri,
        onProgress: (Float) -> Unit,
        stop: () -> Boolean = { false },
    ): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            val root = folder()?.let { DocumentFile.fromTreeUri(context, it) }
            if (root == null || !root.canWrite()) fail("arcade.import.error.folder")
            val id = title.id
            val acgameName = "$id.${Arcade.EXTENSION}"
            val existing = root.findFile(acgameName)?.takeIf { it.isFile }
            val text = existing?.let { readText(context, it.uri) }
            val before = text?.let(Arcade::parse)

            // A memory card file is read whole first, so that picking something else (the image, say)
            // fails before anything is written.
            val bytes = when (part) {
                Part.DONGLE -> readCard(context, source, "arcade.import.error.dongle")
                Part.CARD -> readCard(context, source, "arcade.import.error.card")
                else -> null
            }
            // A disc dump often comes gzipped (an .iso.gz), which the board's drive cannot read: it is
            // unpacked as it is copied, so the phone never needs room for both.
            val gzip = part == Part.IMAGE && isGzip(context, source)
            val (key, name) = when (part) {
                Part.IMAGE -> "mediasrc" to "$id.${extension(context, source, if (gzip) "iso" else "chd", underGzip = gzip)}"
                Part.DONGLE -> "dongle" to "$id.${cardExtension(bytes!!)}"
                Part.CARD -> "card" to "${id}_card.${cardExtension(bytes!!)}"
                Part.BOOT -> error("the boot program comes with every part")
            }
            val acgame = (if (text == null || before == null) acgame(title, part, name) else withData(text, key, name))
                .toByteArray(Charsets.UTF_8)

            // The boot program goes where the template puts it; an .acgame laid out otherwise keeps its own.
            val subdir = before?.subdir ?: id
            val template = before == null || (subdir.equals(id, ignoreCase = true) && before.elf.equals("boot.elf", ignoreCase = true))
            val boot = if (template) bootProgram(context, id) else null

            // A new game's .acgame goes first: from then on the library and this screen know the folder
            // and the files beside it are this game's, so an import cut short (the app closed during the
            // copy) lists nothing stray. One that is there already changes only once the part is in, so a
            // failed import leaves that game as it was.
            if (before == null) write(context, root, acgameName, acgame)

            val dir = if (subdir.isEmpty()) root else root.findFile(subdir)?.takeIf { it.isDirectory }
                ?: root.createDirectory(subdir) ?: fail("arcade.import.error.folder")
            if (boot != null) write(context, dir, "boot.elf", boot)

            when (part) {
                Part.IMAGE -> copy(context, source, dir, name, gzip, onProgress, stop)
                else -> {
                    put(context, source, bytes!!, root, name)
                    installCard(context, bytes, name)
                }
            }

            if (before != null) write(context, root, acgameName, acgame)
            println("@@ANDROID_ARCADE@@ imported $id ${part.name.lowercase()}: $name")
        }
    }

    /** [id]'s boot program, from the boot files. */
    private fun bootProgram(context: Context, id: String): ByteArray = runCatching {
        ZipFile(bootFiles(context)).use { zip ->
            val entry = zip.entries().asSequence().firstOrNull {
                BOOT_ENTRY.find(it.name)?.groupValues?.get(1).equals(id, ignoreCase = true)
            } ?: return@use null
            zip.getInputStream(entry).use { it.readBytes() }
        }
    }.getOrNull() ?: fail("arcade.import.error.boot")

    /** The .acgame PCSX2x6's template writes for [title], with [part] called [name]. A part not imported
     *  yet has the name the core assumes for it (Arcade.parse), the one its import most often gives it. */
    private fun acgame(title: Title, part: Part, name: String): String = buildString {
        val id = title.id
        append("[game]\n")
        append("name=").append(title.name).append('\n')
        append("gameid=").append(id).append('\n')
        when (title.board) {
            "System256" -> append("platform=256\n")
            "System SUPER256" -> append("platform=super256\n")
            "System246" -> append("platform=246\n")
        }
        append("\n[data]\n")
        append("subdir=").append(id).append('\n')
        append("elf=boot.elf\n")
        append("dongle=").append(if (part == Part.DONGLE) name else "$id.ps2").append('\n')
        if (part == Part.CARD) append("card=").append(name).append('\n')
        append("mediasrc=").append(if (part == Part.IMAGE) name else "$id.chd").append('\n')
        if (title.media.isNotBlank()) append("media=").append(title.media).append('\n')
    }

    /**
     * [text], an .acgame, with [key] in its [data] section set to [value]: every line for it replaced
     * (the core takes the last), or one added under the section's header when it has none; everything
     * else as it was. Sections and keys in any case, as the core reads them (Arcade.parse).
     */
    private fun withData(text: String, key: String, value: String): String {
        val lines = text.removePrefix("\uFEFF").lines().toMutableList()
        var section = ""
        var header = -1
        var found = false
        for (i in lines.indices) {
            val line = lines[i].trim()
            if (line.startsWith("[") && line.endsWith("]")) {
                section = line.substring(1, line.length - 1).trim().lowercase()
                if (section == "data" && header < 0) header = i
                continue
            }
            val eq = line.indexOf('=')
            if (section == "data" && eq > 0 && line.substring(0, eq).trim().equals(key, ignoreCase = true)) {
                lines[i] = "$key=$value"
                found = true
            }
        }
        if (found) return lines.joinToString("\n")
        if (header < 0) return text.removePrefix("\uFEFF").trimEnd() + "\n\n[data]\n$key=$value\n"
        lines.add(header + 1, "$key=$value")
        return lines.joinToString("\n")
    }

    /** An .acgame's text, or null when it cannot be read or is too big to be one. */
    private fun readText(context: Context, uri: Uri): String? = runCatching {
        context.contentResolver.openInputStream(uri)?.use(::readBounded)
    }.getOrNull()?.takeIf { it.size <= MAX_ACGAME_BYTES }?.toString(Charsets.UTF_8)

    /** A memory card file (a dongle or a card), read whole and unpacked if gzipped (a .bin.gz):
     *  [errorKey]'s message when it cannot be one, being smaller or far bigger than any memory card. */
    private fun readCard(context: Context, uri: Uri, errorKey: String): ByteArray {
        // A game image picked by mistake is told apart by its size, before any of it is read.
        if (size(context, uri) > MAX_CARD_BYTES) fail(errorKey, displayName(context, uri))
        val read = try {
            (context.contentResolver.openInputStream(uri) ?: fail("arcade.import.error.read")).use(::readBounded)
        } catch (e: IOException) {
            fail("arcade.import.error.read")
        } catch (e: SecurityException) {
            fail("arcade.import.error.read")
        }
        val bytes = if (!isGzip(read)) read else try {
            GZIPInputStream(read.inputStream()).use(::readBounded)
        } catch (e: IOException) {
            fail(errorKey, displayName(context, uri))
        }
        if (bytes.size < MIN_CARD_BYTES || bytes.size > MAX_CARD_BYTES) fail(errorKey, displayName(context, uri))
        return bytes
    }

    /** All of [input], or, once it is bigger than any memory card, no more of it than that. */
    private fun readBounded(input: InputStream): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        val chunk = ByteArray(64 * 1024)
        while (out.size() <= MAX_CARD_BYTES) {
            val n = input.read(chunk)
            if (n < 0) break
            out.write(chunk, 0, n)
        }
        return out.toByteArray()
    }

    private fun isGzip(bytes: ByteArray): Boolean = bytes.size >= 2 && bytes[0] == 0x1f.toByte() && bytes[1] == 0x8b.toByte()

    /** Whether a picked document is gzipped, by its first two bytes. */
    private fun isGzip(context: Context, uri: Uri): Boolean = runCatching {
        context.contentResolver.openInputStream(uri)?.use { s -> s.read() == 0x1f && s.read() == 0x8b }
    }.getOrNull() == true

    /** Counts what is read through it into [count] (its one element): how far into a packed file the
     *  unpacking is, since the unpacked size is not known until the end. */
    private class CountingInputStream(input: InputStream, private val count: LongArray) : java.io.FilterInputStream(input) {
        override fun read(): Int = super.read().also { if (it >= 0) count[0]++ }
        override fun read(b: ByteArray, off: Int, len: Int): Int = super.read(b, off, len).also { if (it > 0) count[0] += it }
        override fun skip(n: Long): Long = super.skip(n).also { if (it > 0) count[0] += it }
    }

    /**
     * The extension the core reads a memory card file right by, from what the file holds, never from
     * its name. The core takes a .bin as a card without its ECC (512-byte pages) and converts it as it
     * opens and closes it (FileMemoryCard), so a dump WITH its ECC, 528-byte pages as PCSX2 keeps cards
     * (8,650,752 bytes for 8 MB, often called dongle.bin), has to be .ps2 or that conversion garbles it.
     * Without ECC a card is a whole number of megabytes; with it, it is not.
     */
    private fun cardExtension(bytes: ByteArray): String = if (bytes.size % (1 shl 20) == 0) "bin" else "ps2"

    /** [bytes], read from [source], into [dir] as [name], unless [source] already IS that file. */
    private fun put(context: Context, source: Uri, bytes: ByteArray, dir: DocumentFile, name: String) {
        if (dir.findFile(name)?.takeIf { it.isFile }?.let { same(it.uri, source) } == true) return
        write(context, dir, name, bytes)
    }

    /** Whether two document URIs are the same document. */
    private fun same(a: Uri, b: Uri): Boolean = runCatching {
        a.authority == b.authority && DocumentsContract.getDocumentId(a) == DocumentsContract.getDocumentId(b)
    }.getOrDefault(false)

    /** A memory card file into the memory cards folder. An older, different file of that name is
     *  kept beside it as <name>.<time>.bak rather than overwritten. */
    private fun installCard(context: Context, bytes: ByteArray, name: String) {
        val target = File(Arcade.memcardsDir(context).apply { mkdirs() }, name)
        if (target.isFile) {
            if (target.length() == bytes.size.toLong() && target.readBytes().contentEquals(bytes)) return
            // A rename onto the old file would replace it, so without its .bak there is no going on.
            if (!target.renameTo(File(target.parentFile, "$name.${System.currentTimeMillis()}.bak")))
                fail("arcade.import.error.write", name)
        }
        val part = File(target.parentFile, ".$name.part")
        val written = runCatching { part.writeBytes(bytes) }.isSuccess && part.renameTo(target)
        if (!written) {
            part.delete()
            fail("arcade.import.error.write", name)
        }
    }

    /** Copies [source] into [dir] as [name], unpacking it on the way when it is gzipped ([gunzip]),
     *  replacing a file of that name, unless [source] already IS that file (picked from the arcade
     *  folder itself). */
    private fun copy(
        context: Context,
        source: Uri,
        dir: DocumentFile,
        name: String,
        gunzip: Boolean,
        onProgress: (Float) -> Unit,
        stop: () -> Boolean,
    ) {
        if (!gunzip && dir.findFile(name)?.takeIf { it.isFile }?.let { same(it.uri, source) } == true) {
            onProgress(1f)
            return
        }
        // Unpacking, the progress is how much of the packed file has been read.
        val packedRead = if (gunzip) LongArray(1) else null
        write(context, dir, name, size(context, source), onProgress, packedRead?.let { c -> { c[0] } }, stop) {
            val raw = runCatching { context.contentResolver.openInputStream(source) }.getOrNull()
            if (raw == null || packedRead == null) raw else GZIPInputStream(CountingInputStream(raw, packedRead), 1 shl 16)
        }
    }

    private fun write(context: Context, dir: DocumentFile, name: String, bytes: ByteArray) =
        write(context, dir, name, bytes.size.toLong(), {}) { bytes.inputStream() }

    /**
     * Writes [name] in [dir] from [open], replacing a file of that name. Into a temporary file first,
     * swapped in only once it is complete: a copy that fails half way, is cancelled ([stop]), or reads the
     * very file being replaced can then never leave the real one cut short. The old file steps aside as
     * <name>.old and goes only once the new one has its name, so the folder always holds a whole copy
     * under a name: the new one, or the old one to go back to.
     */
    private fun write(
        context: Context,
        dir: DocumentFile,
        name: String,
        total: Long,
        onProgress: (Float) -> Unit,
        position: (() -> Long)? = null,
        stop: () -> Boolean = { false },
        open: () -> InputStream?,
    ) {
        val resolver = context.contentResolver
        val partName = "$name.part"
        dir.findFile(partName)?.let { runCatching { DocumentsContract.deleteDocument(resolver, it.uri) } }
        val part = dir.createFile(OCTET_STREAM, partName) ?: fail("arcade.import.error.folder")
        try {
            val input = open() ?: fail("arcade.import.error.read")
            input.use { i ->
                (resolver.openOutputStream(part.uri, "w") ?: fail("arcade.import.error.folder")).use { o ->
                    copy(i, o, total, Long.MAX_VALUE, onProgress, position, stop)
                }
            }
        } catch (e: Exception) {
            runCatching { DocumentsContract.deleteDocument(resolver, part.uri) }
            if (e is ImportCancelled || e is IllegalStateException) throw e
            // A gzipped dump that is damaged or cut short.
            if (e is ZipException || e is java.io.EOFException) fail("arcade.import.error.read")
            val full = e.message?.let { it.contains("ENOSPC") || it.contains("No space left", ignoreCase = true) } == true
            fail(if (full) "arcade.import.error.space" else "arcade.import.error.write", name)
        }
        dir.findFile("$name.old")?.let { runCatching { DocumentsContract.deleteDocument(resolver, it.uri) } }
        val old = dir.findFile(name)?.takeIf { it.isFile }
        val aside = old?.let { o -> runCatching { DocumentsContract.renameDocument(resolver, o.uri, "$name.old") }.getOrNull() }
        if (old != null && aside == null &&
            !runCatching { DocumentsContract.deleteDocument(resolver, old.uri) }.getOrDefault(false)
        ) {
            // A provider that renames nothing would not delete the old file either.
            runCatching { DocumentsContract.deleteDocument(resolver, part.uri) }
            fail("arcade.import.error.write", name)
        }
        if (runCatching { DocumentsContract.renameDocument(resolver, part.uri, name) }.getOrNull() != null) {
            aside?.let { runCatching { DocumentsContract.deleteDocument(resolver, it) } }
            return
        }
        if (aside != null) {
            // The old file could be renamed but the new one could not: the old one goes back.
            runCatching { DocumentsContract.renameDocument(resolver, aside, name) }
            runCatching { DocumentsContract.deleteDocument(resolver, part.uri) }
            fail("arcade.import.error.write", name)
        }
        // A provider that cannot rename: copy the finished file across instead.
        val final = dir.createFile(OCTET_STREAM, name) ?: fail("arcade.import.error.folder")
        val copied = runCatching {
            resolver.openInputStream(part.uri)!!.use { i ->
                resolver.openOutputStream(final.uri, "w")!!.use { o -> copy(i, o, total, Long.MAX_VALUE, onProgress = {}) }
            }
        }.isSuccess
        if (!copied) {
            // The finished file stays, as <name>.part: only the copy of it is incomplete.
            runCatching { DocumentsContract.deleteDocument(resolver, final.uri) }
            fail("arcade.import.error.write", name)
        }
        runCatching { DocumentsContract.deleteDocument(resolver, part.uri) }
    }

    /** [position] is where the reading is, when that is not how much has been written (unpacking). */
    private fun copy(
        input: InputStream,
        out: java.io.OutputStream,
        total: Long,
        max: Long,
        onProgress: (Float) -> Unit,
        position: (() -> Long)? = null,
        stop: () -> Boolean = { false },
    ) {
        val buffer = ByteArray(COPY_BUFFER)
        var done = 0L
        var reported = -1
        while (true) {
            if (stop()) throw ImportCancelled()
            val n = input.read(buffer)
            if (n < 0) break
            done += n
            if (done > max) throw IOException("larger than $max bytes")
            out.write(buffer, 0, n)
            if (total > 0) {
                val percent = (((position?.invoke() ?: done) * 100) / total).toInt().coerceIn(0, 100)
                if (percent != reported) {
                    reported = percent
                    onProgress(percent / 100f)
                }
            }
        }
    }

    private fun size(context: Context, uri: Uri): Long = runCatching {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { c ->
            if (c.moveToFirst() && !c.isNull(0)) c.getLong(0) else -1L
        }
    }.getOrNull() ?: -1L

    /** A picked document's name. */
    private fun displayName(context: Context, uri: Uri): String = (runCatching {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }
    }.getOrNull() ?: uri.lastPathSegment.orEmpty()).substringAfterLast('/')

    /** The extension of a picked document's name, lower case, or [default] when it has none; with
     *  [underGzip], the one under its .gz (an .iso.gz holds an .iso). */
    private fun extension(context: Context, uri: Uri, default: String, underGzip: Boolean = false): String {
        val name = displayName(context, uri).let { if (underGzip && it.endsWith(".gz", ignoreCase = true)) it.dropLast(3) else it }
        val ext = name.substringAfterLast('.', "").lowercase()
        return ext.takeIf { it.isNotEmpty() && it.length <= 12 && it.all(Char::isLetterOrDigit) } ?: default
    }

    // ---- The BIOS --------------------------------------------------------------------------------

    /** The arcade BIOS the core will boot with: the one picked, else the first System 256 one, else
     *  the first arcade one (BiosTools' FindArcadeBiosImage order). Its file name, or null. */
    fun biosName(context: Context): String? {
        val dir = MainActivityRuntime.internalBiosDir(context)
        val files = dir.listFiles()?.filter { it.isFile && it.length() in Arcade.ARCADE_BIOS_SIZES }.orEmpty()
        val arcade = files.mapNotNull { f ->
            val info = runCatching {
                NativeApp.getBiosInfoFromFd(ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY).detachFd())
            }.getOrNull()
            if (info != null && Arcade.isArcadeBios(info)) f to info else null
        }
        Arcade.loadArcadeBios()
        Arcade.arcadeBios.value?.let { picked -> arcade.firstOrNull { it.first.name == picked }?.let { return it.first.name } }
        return (arcade.firstOrNull { it.second.description.contains(S256_BIOS_SERIAL) } ?: arcade.firstOrNull())?.first?.name
    }

    /** The EXTINFO serial of the System 256 BIOS, which the core's BIOS description ends with
     *  (BiosTools' ARCADE_S256_BIOS_SERIAL). */
    private const val S256_BIOS_SERIAL = "20040519-145634"

    private fun fail(key: String, vararg args: Any): Nothing =
        throw IllegalStateException(if (args.isEmpty()) I18n.get(key) else I18n.get(key).format(*args))
}
