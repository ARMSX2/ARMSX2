package com.armsx2

import android.content.Context
import com.armsx2.runtime.MainActivityRuntime
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * One-file export/import of everything the user would lose by reinstalling: save states, memory
 * cards, artwork, per-game settings, controller profiles, patches, and every preference.
 *
 * Reinstalling currently wipes all of it. ROMs and BIOS survive because they live outside the app
 * (true-SAF), but app-private data does not — and SharedPreferences are wiped even when the data
 * folder is reused, which is why settings alone are not enough to back up.
 *
 * Archives are portable between the sideload and Play builds: the preference file has a fixed name
 * ("ARMSX2", not the usual "<package>_preferences"), and the data-root entries are stored relative
 * to whatever [MainActivityRuntime.assetCopyRoot] resolves to, so moving between packages — or onto
 * a device with a different data folder — restores into the right place either way.
 *
 * Deliberately does NOT include:
 *  - `textures/` and `videos/` — texture packs run to gigabytes and are re-downloadable; putting
 *    them in would make the archive impossible to hand around, which defeats the purpose.
 *  - `bios/` — the user's own dumps; they keep those themselves and we should not copy them around.
 *  - `cache/`, `logs/`, `pgo/`, `resources/`, `shaders/` — all regenerated on demand. Shader caches
 *    in particular are large, GPU-specific, and actively harmful to restore onto another device.
 */
object BackupManager {
    private const val MANIFEST = "armsx2-backup.json"
    private const val PREFS_DIR = "prefs/"
    private const val FILES_DIR = "files/"

    /** Data-root entries worth preserving. Anything absent is skipped silently. */
    private val INCLUDED = listOf(
        // memcard-backups rides along deliberately. It is the per-card snapshot rotation
        // ([MemoryCardBackup]), and a card restored onto a new phone from this archive is exactly
        // as likely to be the broken one — so the history that can undo it has to travel too. A
        // card is mostly erased space, so the compressed rotation is small next to sstates.
        "sstates", "memcards", "memcard-backups", "covers", "gamesettings", "inputprofiles",
        "cheats", "patches", "snaps",
    )
    private val INCLUDED_FILES = listOf(
        "armsx2-settings.json", "PCSX2-Android.ini", "achievements.ini",
    )

    /** Named to avoid colliding with `kotlin.Result`, which is a default import. */
    data class BackupResult(val ok: Boolean, val detail: String)

    private fun prefsDir(context: Context): File =
        File(context.applicationInfo.dataDir, "shared_prefs")

    // ---- export ----------------------------------------------------------------------------

    /** Blocking. Writes a backup archive to [out]. */
    fun export(context: Context, out: OutputStream): BackupResult {
        val root = File(MainActivityRuntime.assetCopyRoot(context))
        var files = 0
        var bytes = 0L
        return runCatching {
            ZipOutputStream(out.buffered()).use { zip ->
                zip.putNextEntry(ZipEntry(MANIFEST))
                zip.write(
                    ("{\"schemaVersion\":1,\"package\":\"${context.packageName}\"," +
                        "\"versionName\":\"${appVersion(context)}\"}").toByteArray()
                )
                zip.closeEntry()

                // Preferences live outside the data root (/data/data/<pkg>/shared_prefs) and hold
                // controller mappings, touch layouts, playtime, theme and the settings tiers. They
                // are wiped on reinstall even if the data folder is reused, so they matter most.
                prefsDir(context).listFiles()
                    ?.filter { it.isFile && it.name.endsWith(".xml") }
                    ?.forEach { f ->
                        bytes += addFile(zip, f, PREFS_DIR + f.name)
                        files++
                    }

                INCLUDED_FILES.forEach { name ->
                    val f = File(root, name)
                    if (f.isFile) { bytes += addFile(zip, f, FILES_DIR + name); files++ }
                }
                INCLUDED.forEach { dir ->
                    val d = File(root, dir)
                    if (!d.isDirectory) return@forEach
                    d.walkTopDown().filter { it.isFile }.forEach { f ->
                        val rel = FILES_DIR + dir + "/" +
                            f.relativeTo(d).path.replace(File.separatorChar, '/')
                        bytes += addFile(zip, f, rel)
                        files++
                    }
                }
            }
            BackupResult(true, "$files files, ${bytes / 1024 / 1024} MB")
        }.getOrElse { BackupResult(false, it.message ?: "export failed") }
    }

    private fun addFile(zip: ZipOutputStream, f: File, entryName: String): Long {
        zip.putNextEntry(ZipEntry(entryName))
        val n = f.inputStream().use { it.copyTo(zip) }
        zip.closeEntry()
        return n
    }

    // ---- import ----------------------------------------------------------------------------

    /** Blocking. Restores from [input]. The app must be restarted afterwards: preferences are read
     *  once at startup, so a live process would keep serving the old values and then overwrite the
     *  restored file on its next write. */
    fun restore(context: Context, input: InputStream): BackupResult {
        val root = File(MainActivityRuntime.assetCopyRoot(context))
        val prefsDir = prefsDir(context)
        var files = 0
        return runCatching {
            ZipInputStream(input.buffered()).use { zip ->
                while (true) {
                    val e = zip.nextEntry ?: break
                    val name = e.name
                    if (e.isDirectory || name == MANIFEST) { zip.closeEntry(); continue }
                    val dest = when {
                        name.startsWith(PREFS_DIR) -> File(prefsDir, name.removePrefix(PREFS_DIR))
                        name.startsWith(FILES_DIR) -> File(root, name.removePrefix(FILES_DIR))
                        else -> { zip.closeEntry(); continue }
                    }
                    // Zip-slip guard: a crafted entry name like "../../x" would otherwise write
                    // anywhere the app can reach. Compare canonical paths, not the raw strings.
                    val base = if (name.startsWith(PREFS_DIR)) prefsDir else root
                    if (!dest.canonicalPath.startsWith(base.canonicalPath + File.separator)) {
                        zip.closeEntry(); continue
                    }
                    dest.parentFile?.mkdirs()
                    dest.outputStream().use { zip.copyTo(it) }
                    files++
                    zip.closeEntry()
                }
            }
            if (files == 0) BackupResult(false, "not an ARMSX2 backup")
            else BackupResult(true, "$files files")
        }.getOrElse { BackupResult(false, it.message ?: "restore failed") }
    }

    fun suggestedName(context: Context): String =
        "ARMSX2-backup-${appVersion(context)}.zip"

    private fun appVersion(context: Context): String = runCatching {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "unknown"
    }.getOrDefault("unknown")
}
