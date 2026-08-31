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

    /** Runs the whole setup. Blocking -- call on Dispatchers.IO. Returns a message to show. */
    fun run(context: Context, iso: GameInfo, elfUri: Uri): String {
        val serial = iso.serial?.takeIf { it.isNotBlank() }
            ?: iso.uri.lastPathSegment?.substringAfterLast('/')?.substringBeforeLast('.')
            ?: "quickload"
        val subdir = serial.replace(Regex("[^A-Za-z0-9._-]"), "_")

        // VMManager only takes its ELF branch for a filename ending in .elf, so the name matters.
        // The guide makes the user rename the file by hand -- and the packages ship seven of their
        // eight variants WITHOUT the extension, so that step is where people get stuck. We know we
        // were handed a boot ELF, so just add it. Picking "SLPM_654.28" now works as-is.
        val picked = displayName(context, elfUri)
        val elfName = if (picked.endsWith(".elf", ignoreCase = true)) picked else "$picked.elf"

        val written = runCatching { NativeApp.extractIsoToHostfs(iso.uri.toString(), subdir) }
            .getOrDefault(-1)
        if (written <= 0) return I18n.get("games.quickLoad.extractFailed")

        val dest = MainActivityRuntime.hostfsDir()?.let { File(it, subdir) }
            ?: return I18n.get("games.quickLoad.extractFailed")

        // Copied in AFTER extraction on purpose: the ELF replaces a file the disc also carries
        // (SLPM_xxx.xx), so extracting second would overwrite the modified copy with the stock one.
        val elfFile = File(dest, elfName)
        val copied = runCatching {
            context.contentResolver.openInputStream(elfUri)?.use { input ->
                elfFile.outputStream().use { output -> input.copyTo(output) }
            } != null
        }.getOrDefault(false)
        if (!copied) return I18n.get("games.quickLoad.elfFailed")

        // The guide has the modified file OVERWRITE the disc's own SLPM_xxx.xx and then get
        // renamed, so the stock copy is gone by the end. Extraction wrote it back, so drop it --
        // leaving both would differ from every working desktop setup for no reason.
        val stock = File(dest, elfName.removeSuffix(".elf"))
        if (stock.isFile && stock.absolutePath != elfFile.absolutePath) runCatching { stock.delete() }

        // Pair it with the disc. Without this the ELF boots against NoDisc and the game hangs on
        // its loading screen -- the exact symptom this whole feature exists to fix.
        val paired = runCatching {
            NativeApp.setElfDiscOverride(elfFile.absolutePath, iso.uri.toString())
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
