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
    private const val OCTET_STREAM = "application/octet-stream"
    private val BOOT_ENTRY = Regex("""(?:^|/)bin/(NM\d{5})/boot\.elf$""", RegexOption.IGNORE_CASE)

    /** An arcade game the database knows. [board] is its region field (System246, System256,
     *  System SUPER256), [media] CD, DVD or HDD. */
    data class Title(val id: String, val name: String, val board: String, val media: String)

    /** An .acgame in the arcade folder, and what it still lacks. */
    data class Installed(val id: String, val name: String, val missing: List<Part>)

    enum class Part { IMAGE, DONGLE, CARD, BOOT }

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

    /** The .acgame files in the arcade folder, with what each one still lacks: where
     *  [Arcade.prepare] looks for it, it is not. [titles] names the games whose .acgame has none. */
    fun installed(context: Context, titles: List<Title>): List<Installed> {
        val root = folder()?.let { DocumentFile.fromTreeUri(context, it) } ?: return emptyList()
        // Every name and kind is a provider query, so each child's is asked for once.
        val children = runCatching {
            root.listFiles().map { Child(it, it.name.orEmpty(), it.isDirectory) }
        }.getOrDefault(emptyList())
        val beside = children.filter { !it.isDirectory }.map { it.name.lowercase() }.toSet()
        val cards = Arcade.memcardsDir(context)
        val dbNames = titles.associate { it.id to it.name }
        return children.mapNotNull { child ->
            if (child.isDirectory || !Arcade.isAcGameName(child.name)) return@mapNotNull null
            val game = Arcade.read(context, child.file.uri.toString()) ?: return@mapNotNull null
            val inDir = if (game.subdir.isEmpty()) beside else children
                .firstOrNull { it.isDirectory && it.name.equals(game.subdir, ignoreCase = true) }
                ?.let { d -> runCatching { d.file.listFiles().mapNotNull { it.name?.lowercase() }.toSet() }.getOrNull() }
                .orEmpty()
            fun card(name: String): Boolean {
                val file = File(name).name
                return File(cards, file).let { it.isFile && it.length() > 0 } ||
                    file.lowercase() in beside || file.lowercase() in inDir
            }
            val missing = buildList {
                if (game.mediaSrc.lowercase() !in inDir) add(Part.IMAGE)
                if (!card(game.dongle)) add(Part.DONGLE)
                if (game.card.isNotEmpty() && !card(game.card)) add(Part.CARD)
                if (game.elf.lowercase() !in inDir) add(Part.BOOT)
            }
            Installed(game.gameId, game.name.ifBlank { dbNames[game.gameId] ?: game.gameId }, missing)
        }.sortedBy { it.name.lowercase() }
    }

    /**
     * Imports [title] into the arcade folder: its boot program, its [image] (copied, which for a hard
     * drive or DVD image takes a while; [onProgress] follows it), its [dongle] (and [card], Soul
     * Calibur II's Conquest card), and an .acgame naming them. The dongle and card also go into the
     * memory cards folder, where the core mounts them from, replacing an older copy there only after
     * keeping it as a .bak. A failure's message says what went wrong, in the player's language.
     */
    suspend fun import(
        context: Context,
        title: Title,
        image: Uri,
        dongle: Uri,
        card: Uri?,
        onProgress: (Float) -> Unit,
    ): Result<Unit> = withContext(Dispatchers.IO) {
        runCatching {
            val root = folder()?.let { DocumentFile.fromTreeUri(context, it) }
            if (root == null || !root.canWrite()) fail("arcade.import.error.folder")
            val id = title.id

            // The dongle, and Soul Calibur II's Conquest card: memory card files, read whole first, so
            // that picking something else (the image again, say) fails before anything is written.
            val dongleBytes = readCard(context, dongle, "arcade.import.error.dongle")
            val cardBytes = card?.let { readCard(context, it, "arcade.import.error.card") }

            // The boot program, from the boot files.
            val boot = runCatching {
                ZipFile(bootFiles(context)).use { zip ->
                    val entry = zip.entries().asSequence().firstOrNull {
                        BOOT_ENTRY.find(it.name)?.groupValues?.get(1).equals(id, ignoreCase = true)
                    } ?: return@use null
                    zip.getInputStream(entry).use { it.readBytes() }
                }
            }.getOrNull() ?: fail("arcade.import.error.boot")

            val dongleName = "$id.${extension(context, dongle, "ps2")}"
            val cardName = card?.let { "${id}_card.${extension(context, it, "bin")}" }
            val imageName = "$id.${extension(context, image, "chd")}"
            val acgameName = "$id.${Arcade.EXTENSION}"
            val acgame = acgame(title, dongleName, imageName, cardName).toByteArray(Charsets.UTF_8)

            // A new game's .acgame goes first: from then on the library and this screen know the
            // folder and the files beside it are this game's, so an import cut short (the app closed
            // during the copy) lists nothing stray, and shows here with what it is missing. One that
            // is there already stays as it is until everything is copied, so a failed import again
            // leaves that game as it was.
            val existed = root.findFile(acgameName)?.isFile == true
            if (!existed) write(context, root, acgameName, acgame)

            val dir = root.findFile(id)?.takeIf { it.isDirectory } ?: root.createDirectory(id)
                ?: fail("arcade.import.error.folder")
            write(context, dir, "boot.elf", boot)

            put(context, dongle, dongleBytes, root, dongleName)
            installCard(context, dongleBytes, dongleName)
            if (card != null && cardBytes != null && cardName != null) {
                put(context, card, cardBytes, root, cardName)
                installCard(context, cardBytes, cardName)
            }

            // The image, last: it is the long one.
            copy(context, image, dir, imageName, onProgress)

            if (existed) write(context, root, acgameName, acgame)
            println("@@ANDROID_ARCADE@@ imported $id ($imageName, $dongleName${cardName?.let { ", $it" } ?: ""})")
        }
    }

    /** The .acgame PCSX2x6's template writes, plus the card when there is one. */
    private fun acgame(title: Title, dongle: String, image: String, card: String?): String = buildString {
        append("[game]\n")
        append("name=").append(title.name).append('\n')
        append("gameid=").append(title.id).append('\n')
        when (title.board) {
            "System256" -> append("platform=256\n")
            "System SUPER256" -> append("platform=super256\n")
            "System246" -> append("platform=246\n")
        }
        append("\n[data]\n")
        append("subdir=").append(title.id).append('\n')
        append("elf=boot.elf\n")
        append("dongle=").append(dongle).append('\n')
        if (card != null) append("card=").append(card).append('\n')
        append("mediasrc=").append(image).append('\n')
        if (title.media.isNotBlank()) append("media=").append(title.media).append('\n')
    }

    /** A memory card file (a dongle or a card), read whole: [errorKey]'s message when it cannot be
     *  one, being empty or far bigger than any memory card. */
    private fun readCard(context: Context, uri: Uri, errorKey: String): ByteArray {
        // A game image picked by mistake is told apart by its size, before any of it is read.
        if (size(context, uri) > MAX_CARD_BYTES) fail(errorKey, displayName(context, uri))
        val bytes = try {
            (context.contentResolver.openInputStream(uri) ?: fail("arcade.import.error.read")).use { input ->
                val out = java.io.ByteArrayOutputStream()
                val chunk = ByteArray(64 * 1024)
                while (out.size() <= MAX_CARD_BYTES) {
                    val n = input.read(chunk)
                    if (n < 0) break
                    out.write(chunk, 0, n)
                }
                out.toByteArray()
            }
        } catch (e: IOException) {
            fail("arcade.import.error.read")
        } catch (e: SecurityException) {
            fail("arcade.import.error.read")
        }
        if (bytes.isEmpty() || bytes.size > MAX_CARD_BYTES) fail(errorKey, displayName(context, uri))
        return bytes
    }

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

    /** Copies [source] into [dir] as [name], replacing a file of that name, unless [source] already
     *  IS that file (picked from the arcade folder itself). */
    private fun copy(context: Context, source: Uri, dir: DocumentFile, name: String, onProgress: (Float) -> Unit) {
        if (dir.findFile(name)?.takeIf { it.isFile }?.let { same(it.uri, source) } == true) {
            onProgress(1f)
            return
        }
        write(context, dir, name, size(context, source), onProgress) {
            runCatching { context.contentResolver.openInputStream(source) }.getOrNull()
        }
    }

    private fun write(context: Context, dir: DocumentFile, name: String, bytes: ByteArray) =
        write(context, dir, name, bytes.size.toLong(), {}) { bytes.inputStream() }

    /**
     * Writes [name] in [dir] from [open], replacing a file of that name. Into a temporary file first,
     * swapped in only once it is complete: a copy that fails half way, or a source that turns out to be
     * the very file being replaced, can then never leave the real one cut short.
     */
    private fun write(context: Context, dir: DocumentFile, name: String, total: Long, onProgress: (Float) -> Unit, open: () -> InputStream?) {
        val resolver = context.contentResolver
        val partName = "$name.part"
        dir.findFile(partName)?.let { runCatching { DocumentsContract.deleteDocument(resolver, it.uri) } }
        val part = dir.createFile(OCTET_STREAM, partName) ?: fail("arcade.import.error.folder")
        try {
            val input = open() ?: fail("arcade.import.error.read")
            input.use { i ->
                (resolver.openOutputStream(part.uri, "w") ?: fail("arcade.import.error.folder")).use { o ->
                    copy(i, o, total, Long.MAX_VALUE, onProgress)
                }
            }
        } catch (e: Exception) {
            runCatching { DocumentsContract.deleteDocument(resolver, part.uri) }
            if (e is IllegalStateException) throw e
            val full = e.message?.let { it.contains("ENOSPC") || it.contains("No space left", ignoreCase = true) } == true
            fail(if (full) "arcade.import.error.space" else "arcade.import.error.write", name)
        }
        dir.findFile(name)?.takeIf { it.isFile }?.let { old ->
            if (!runCatching { DocumentsContract.deleteDocument(resolver, old.uri) }.getOrDefault(false)) {
                runCatching { DocumentsContract.deleteDocument(resolver, part.uri) }
                fail("arcade.import.error.write", name)
            }
        }
        if (runCatching { DocumentsContract.renameDocument(resolver, part.uri, name) }.getOrNull() != null)
            return
        // A provider that cannot rename: copy the finished file across instead.
        val final = dir.createFile(OCTET_STREAM, name) ?: fail("arcade.import.error.folder")
        val copied = runCatching {
            resolver.openInputStream(part.uri)!!.use { i ->
                resolver.openOutputStream(final.uri, "w")!!.use { o -> copy(i, o, total, Long.MAX_VALUE) {} }
            }
        }.isSuccess
        if (!copied) {
            // The finished file stays, as <name>.part: only the copy of it is incomplete.
            runCatching { DocumentsContract.deleteDocument(resolver, final.uri) }
            fail("arcade.import.error.write", name)
        }
        runCatching { DocumentsContract.deleteDocument(resolver, part.uri) }
    }

    private fun copy(input: InputStream, out: java.io.OutputStream, total: Long, max: Long, onProgress: (Float) -> Unit) {
        val buffer = ByteArray(COPY_BUFFER)
        var done = 0L
        var reported = -1
        while (true) {
            val n = input.read(buffer)
            if (n < 0) break
            done += n
            if (done > max) throw IOException("larger than $max bytes")
            out.write(buffer, 0, n)
            if (total > 0) {
                val percent = ((done * 100) / total).toInt().coerceIn(0, 100)
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

    /** The extension of a picked document's name, lower case, or [default] when it has none. */
    private fun extension(context: Context, uri: Uri, default: String): String {
        val ext = displayName(context, uri).substringAfterLast('.', "").lowercase()
        return ext.takeIf { it.isNotEmpty() && it.length <= 12 && it.all(Char::isLetterOrDigit) } ?: default
    }

    // ---- The BIOS --------------------------------------------------------------------------------

    /** The arcade BIOS the core will boot with: the one picked, else the first System 256 one, else
     *  the first arcade one (BiosTools' FindArcadeBiosImage order). Its file name, or null. */
    fun biosName(context: Context): String? {
        val dir = MainActivityRuntime.internalBiosDir(context)
        val files = dir.listFiles()?.filter { it.isFile && it.length() in (4L shl 20)..(8L shl 20) }.orEmpty()
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
