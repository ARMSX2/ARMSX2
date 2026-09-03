package com.armsx2

import android.content.Context
import android.net.Uri
import com.armsx2.i18n.I18n
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File

/**
 * One-shot setup for the host:-filesystem "quick loading" method (obsrv's Biohazard Outbreak
 * guide, and anything else shaped like it).
 *
 * The method wants the disc's files sitting in a folder with one of them replaced by a modified
 * ELF, and the original disc still mounted alongside. On desktop the user mounts the ISO in their
 * OS file manager and drags the files out. **Android cannot mount an ISO at all**, so that step is
 * not something a user here can perform -- which is why the method has never worked on Android
 * regardless of what anyone put where. Doing it in-app is not a convenience, it is the only way it
 * can work.
 *
 * The whole flow, from one long-press:
 *   1. extract every file from the ISO into hostfs/<serial>/     (NativeApp.extractIsoToHostfs)
 *   2. copy the user's modified ELF in beside them
 *   3. pair that ELF with the original ISO                       (NativeApp.setElfDiscOverride)
 * after which the library's own hostfs scan surfaces the ELF as a normal entry and launching it
 * boots the game. The ELF lands on a real path, so the core derives host:'s root from its folder
 * the same way it does on desktop.
 */
object QuickLoadSetup {

    /**
     * What the core can actually open. A file:// URI is a percent-encoded wrapper around a real
     * path -- CDVD takes a plain filesystem path and chokes on both the scheme and the %20s, which
     * is why extraction failed on any library entry with a space in its name. uri.path decodes it.
     * content:// must stay a URI: FileSystem routes those through the SAF layer by scheme.
     *
     * Same conversion MainActivityRuntime.launchGame already does before booting.
     */
    private fun corePath(uri: Uri): String =
        if (uri.scheme.equals("file", ignoreCase = true)) (uri.path ?: uri.toString())
        else uri.toString()

    /** Folder this game's quick-load files live in, whether or not it exists yet. */
    fun folderFor(iso: GameInfo): File? =
        MainActivityRuntime.hostfsDir()?.let { File(it, subdirFor(iso)) }

    /** True when [game] is an ELF we installed -- i.e. it sits under hostfs. */
    fun isInstalledElf(game: GameInfo): Boolean {
        val root = MainActivityRuntime.hostfsDir()?.absolutePath ?: return false
        val path = if (game.uri.scheme.equals("file", true)) game.uri.path else null
        return path != null && path.startsWith(root)
    }

    /** Remove one game's quick-load files, and the ELF/disc pairing that pointed at them. */
    fun remove(elf: GameInfo): Boolean {
        val path = elf.uri.path ?: return false
        val dir = File(path).parentFile ?: return false
        val root = MainActivityRuntime.hostfsDir() ?: return false
        // Never delete outside hostfs, whatever gets passed in.
        if (!dir.absolutePath.startsWith(root.absolutePath) || dir.absolutePath == root.absolutePath) return false
        // Drop the pairing first: an orphaned DiscPath in gamesettings/<CRC>.ini would otherwise
        // outlive the files and quietly re-apply if the same ELF ever came back.
        runCatching { NativeApp.setElfDiscOverride(path, "") }
        return runCatching { dir.deleteRecursively() }.getOrDefault(false)
    }

    /** Bytes the disc will take once extracted. The ISO's own size is the right estimate --
     *  extraction copies its files out, minus filesystem overhead. */
    fun estimatedBytes(context: Context, iso: GameInfo): Long = runCatching {
        if (iso.uri.scheme.equals("file", ignoreCase = true)) {
            File(iso.uri.path ?: return@runCatching 0L).length()
        } else {
            context.contentResolver.query(iso.uri, null, null, null, null)?.use { c ->
                val i = c.getColumnIndex(android.provider.OpenableColumns.SIZE)
                if (i >= 0 && c.moveToFirst()) c.getLong(i) else 0L
            } ?: 0L
        }
    }.getOrDefault(0L)

    /** Free space where the files will land. */
    fun freeBytes(): Long =
        runCatching { MainActivityRuntime.hostfsDir()?.usableSpace ?: 0L }.getOrDefault(0L)

    private fun subdirFor(iso: GameInfo): String {
        val serial = iso.serial?.takeIf { it.isNotBlank() }
            ?: iso.uri.lastPathSegment?.substringAfterLast('/')?.substringBeforeLast('.')
            ?: "quickload"
        return serial.replace(Regex("[^A-Za-z0-9._-]"), "_")
    }

    /** Runs the whole setup. Blocking -- call on Dispatchers.IO. Returns a message to show. */
    fun run(context: Context, iso: GameInfo, elfUri: Uri): String {
        val subdir = subdirFor(iso)

        // VMManager only takes its ELF branch for a filename ending in .elf, so the name matters.
        // The guide makes the user rename the file by hand -- and the packages ship seven of their
        // eight variants WITHOUT the extension, so that step is where people get stuck. We know we
        // were handed a boot ELF, so just add it. Picking "SLPM_654.28" now works as-is.
        val picked = displayName(context, elfUri)
        val elfName = if (picked.endsWith(".elf", ignoreCase = true)) picked else "$picked.elf"

        val written = runCatching { NativeApp.extractIsoToHostfs(corePath(iso.uri), subdir) }
            .getOrDefault(-1)
        if (written <= 0) return I18n.get("games.quickLoad.extractFailed")

        val dest = MainActivityRuntime.hostfsDir()?.let { File(it, subdir) }
            ?: return I18n.get("games.quickLoad.extractFailed")

        // Copied in AFTER extraction on purpose: the ELF replaces a file the disc also carries
        // (SLPM_xxx.xx), so extracting second would overwrite the modified copy with the stock one.
        val elfFile = File(dest, elfName)
        // Belt and braces: extraction created this natively, but if the two sides ever disagree
        // about the root again, fail by writing a usable folder rather than by ENOENT.
        runCatching { elfFile.parentFile?.mkdirs() }
        // Logged, not swallowed: a bare runCatching here cost a debugging round-trip once
        // already -- the failure message named a step but not a reason.
        val copyError = runCatching {
            val opened = context.contentResolver.openInputStream(elfUri)
                ?: error("openInputStream returned null for $elfUri")
            opened.use { input -> elfFile.outputStream().use { output -> input.copyTo(output) } }
            null
        }.getOrElse { it }
        if (copyError != null || !elfFile.isFile || elfFile.length() == 0L) {
            android.util.Log.w(
                "QuickLoadSetup",
                "ELF copy failed: dest=${elfFile.absolutePath} exists=${elfFile.isFile} " +
                    "size=${elfFile.length()} uri=$elfUri",
                copyError,
            )
            return I18n.get("games.quickLoad.elfFailed")
        }

        // The guide has the modified file OVERWRITE the disc's own SLPM_xxx.xx and then get
        // renamed, so the stock copy is gone by the end. Extraction wrote it back, so drop it --
        // leaving both would differ from every working desktop setup for no reason.
        val stock = File(dest, elfName.removeSuffix(".elf"))
        if (stock.isFile && stock.absolutePath != elfFile.absolutePath) runCatching { stock.delete() }

        // Pair it with the disc. Without this the ELF boots against NoDisc and the game hangs on
        // its loading screen -- the exact symptom this whole feature exists to fix.
        val paired = runCatching {
            NativeApp.setElfDiscOverride(elfFile.absolutePath, corePath(iso.uri))
        }.getOrDefault(false)
        if (!paired) return I18n.get("games.quickLoad.pairFailed")

        return String.format(I18n.get("games.quickLoad.done"), written, elfFile.name)
    }

    /** The picked file's display name, so the ELF keeps the name the guide had the user give it. */
    private fun displayName(context: Context, uri: Uri): String {
        runCatching {
            context.contentResolver.query(uri, null, null, null, null)?.use { c ->
                val i = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
                if (i >= 0 && c.moveToFirst()) return c.getString(i)
            }
        }
        return uri.lastPathSegment?.substringAfterLast('/').orEmpty()
    }
}
