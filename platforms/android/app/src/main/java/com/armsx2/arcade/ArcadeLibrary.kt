// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.arcade

import android.content.Context
import android.os.ParcelFileDescriptor
import androidx.core.content.edit
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
 * What the NAMCO System 246/256 screen sets up for every arcade game: the boot files they start from,
 * the list of games the database knows, and the arcade BIOS in use. The games themselves are found in
 * the player's game folders, where they are ([ArcadeFiles], and PCSX2x6's .acgame layout); nothing of
 * them is copied.
 *
 * The boot files are Proverb (https://github.com/PS2Homebrew-arcade/proverb, AFL-3.0), the
 * PS2Homebrew-arcade bootloader PCSX2x6's game library template is made from: one zip holding
 * bin/<id>/boot.elf for every game. It is downloaded once into the app's own storage, and a game kept as
 * its own files boots from its game's ELF in it.
 */
object ArcadeLibrary {
    private const val TAG = "ARMSX2-Arcade"
    private const val PREF_BOOT_TIME = "arcade.bootFilesTime"
    private const val BOOT_FILES_URL = "https://github.com/PS2Homebrew-arcade/proverb/releases/download/nightly/proverb.zip"
    private const val MAX_BOOT_FILES_BYTES = 64L shl 20
    private const val COPY_BUFFER = 1 shl 20
    private val BOOT_ENTRY = Regex("""(?:^|/)bin/(NM\d{5})/boot\.elf$""", RegexOption.IGNORE_CASE)

    /** A game the database knows: its ID, name, board and media. */
    data class Title(val id: String, val name: String, val board: String, val media: String)

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

    /** [id]'s boot program, from the boot files; null when they are not downloaded or have none for it. */
    fun bootProgram(context: Context, id: String): ByteArray? = runCatching {
        ZipFile(bootFiles(context)).use { zip ->
            val entry = zip.entries().asSequence().firstOrNull {
                BOOT_ENTRY.find(it.name)?.groupValues?.get(1).equals(id, ignoreCase = true)
            } ?: return@use null
            zip.getInputStream(entry).use { it.readBytes() }
        }
    }.getOrNull()

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

    // ---- The arcade BIOS --------------------------------------------------------------------------

    /** The arcade BIOS the core boots arcade games with, every one there in use: the System 256 one for
     *  every game, another for a game that refuses it (BiosTools' FindArcadeBiosFor). Their boards as the
     *  core names them ("System 246 Rack C, System 256"), else their file names; null for none. */
    fun biosNames(context: Context): String? {
        val dir = MainActivityRuntime.internalBiosDir(context)
        val files = dir.listFiles()?.filter { it.isFile && it.length() in Arcade.ARCADE_BIOS_SIZES }.orEmpty()
        val names = files.mapNotNull { f ->
            val info = runCatching {
                NativeApp.getBiosInfoFromFd(ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY).detachFd())
            }.getOrNull()
            // The core's description is the zone, the board and the EXTINFO serial.
            if (info == null || !Arcade.isArcadeBios(info)) null
            else info.description.trim().removePrefix(info.zone).trim().substringBeforeLast(' ', "").trim().ifEmpty { f.name }
        }
        return names.distinct().sorted().takeIf { it.isNotEmpty() }?.joinToString(", ")
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
}
