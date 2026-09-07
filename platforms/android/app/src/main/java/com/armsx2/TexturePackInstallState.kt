package com.armsx2

import com.armsx2.runtime.MainActivityRuntime
import org.json.JSONObject
import java.io.File

/**
 * Which catalog packs are installed, and at which version.
 *
 * The texture folder itself cannot answer this: a pack extracts to loose replacement files under
 * `textures/<SERIAL>/replacements`, with nothing recording where they came from. Without this the
 * catalog could only ever offer "Get", never "Installed" or "Update available".
 */
object TexturePackInstallState {
    private const val FILE = "texture-packs.json"

    /**
     * One installed pack. [schema]/[format]/[archiveRevision]/[sha256] are absent from state
     * written before the B2 catalog existed; their defaults describe exactly that legacy ZIP
     * install, so old records load unchanged and are only rewritten when a real install happens.
     */
    data class Installed(
        val packId: String,
        val serial: String,
        val version: String,
        val name: String,
        val schema: Int = 1,
        val format: String = "zip",
        val archiveRevision: Long = 0L,
        val sha256: String = "",
    ) {
        val isB2TarZstd: Boolean get() = schema >= 2 && format == "tar+zstd"
    }

    /** What the catalog row offers relative to what is installed. */
    enum class InstallAction { INSTALL, UPDATE, INSTALLED, CONFLICT }

    /**
     * Pure update-eligibility rule shared by the UI and tests.
     *
     * B2 tar+zstd archives are the upgrade target: a legacy ZIP install always updates to B2.
     * Within B2 state, [TextureCatalog.Pack.archiveRevision] is monotonic — a greater revision
     * updates, a lower one is suppressed (a lagging CDN must not walk a device backwards), and an
     * equal revision with a different digest is a conflict rather than a silent reinstall.
     * A schema-1 ZIP fallback can never downgrade B2 state, whatever its version string says.
     */
    fun actionFor(installed: Installed?, pack: TextureCatalog.Pack): InstallAction {
        if (installed == null) return InstallAction.INSTALL
        return when {
            pack.format == TextureCatalog.ArchiveFormat.TAR_ZSTD -> when {
                !installed.isB2TarZstd -> InstallAction.UPDATE
                pack.archiveRevision > installed.archiveRevision -> InstallAction.UPDATE
                pack.archiveRevision < installed.archiveRevision -> InstallAction.INSTALLED
                pack.sha256.equals(installed.sha256, ignoreCase = true) -> InstallAction.INSTALLED
                else -> InstallAction.CONFLICT
            }
            installed.isB2TarZstd -> InstallAction.INSTALLED
            installed.version == pack.version -> InstallAction.INSTALLED
            else -> InstallAction.UPDATE
        }
    }

    /** Bumped on every change so Compose re-reads. */
    val revision = androidx.compose.runtime.mutableStateOf(0)

    private fun stateFile(): File = File(
        MainActivityRuntime.assetCopyRoot(MainActivityRuntime.instance!!.applicationContext), FILE,
    )

    private fun read(): JSONObject {
        val f = stateFile()
        runCatching { if (f.isFile) return JSONObject(f.readText()) }
        // Fall back to the previous good copy. Without this, one unreadable/truncated state file
        // silently reads as "nothing installed", and the next record() then persists a file
        // containing only that one pack — which is how 60 installs collapsed to the most recent
        // couple ("Texture Packs forget being installed", SKrazy).
        val bak = File(f.parentFile, "$FILE.bak")
        runCatching { if (bak.isFile) return JSONObject(bak.readText()) }
        return JSONObject()
    }

    private fun write(root: JSONObject) {
        val ok = runCatching {
            val dest = stateFile()
            dest.parentFile?.mkdirs()
            val tmp = File(dest.parentFile, "$FILE.tmp")
            tmp.writeText(root.toString())

            // Keep the last good copy BEFORE replacing, so a failure here is recoverable by read().
            if (dest.isFile)
                runCatching { dest.copyTo(File(dest.parentFile, "$FILE.bak"), overwrite = true) }

            // ATOMIC_MOVE, not delete()+renameTo(). The old sequence deleted the destination first
            // and then renamed; if the rename failed for any reason the state file was simply GONE
            // and the whole install record with it — and because the failure was swallowed, it
            // looked like a successful save. An atomic move has no window where nothing exists.
            java.nio.file.Files.move(
                tmp.toPath(), dest.toPath(),
                java.nio.file.StandardCopyOption.REPLACE_EXISTING,
                java.nio.file.StandardCopyOption.ATOMIC_MOVE,
            )
            true
        }.getOrElse { e ->
            // Surfaced, not swallowed: losing this file costs the user a re-download of every pack.
            android.util.Log.e("ARMSX2", "texture-pack state write FAILED: ${e.message}")
            false
        }
        if (ok) revision.value = revision.value + 1
    }

    fun all(): Map<String, Installed> {
        val root = read()
        val out = HashMap<String, Installed>()
        val keys = root.keys()
        while (keys.hasNext()) {
            val id = keys.next()
            val o = root.optJSONObject(id) ?: continue
            out[id] = Installed(
                packId = id,
                serial = o.optString("serial"),
                version = o.optString("version"),
                name = o.optString("name"),
                schema = o.optInt("schema", 1),
                format = o.optString("format", "zip"),
                archiveRevision = o.optLong("archiveRevision", 0L),
                sha256 = o.optString("sha256"),
            )
        }
        return out
    }

    /** [format]/[schema]/[archiveRevision]/[sha256] describe the archive that was installed, not
     *  the catalog entry that described it, so update eligibility survives a catalog outage. */
    fun record(
        packId: String,
        serial: String,
        version: String,
        name: String,
        format: String = "zip",
        schema: Int = 1,
        archiveRevision: Long = 0L,
        sha256: String = "",
    ) {
        val root = read()
        root.put(packId, JSONObject().apply {
            put("serial", serial)
            put("version", version)
            put("name", name)
            put("format", format)
            put("schema", schema)
            put("archiveRevision", archiveRevision)
            put("sha256", sha256)
        })
        write(root)
    }

    /** Called when the user deletes a pack's folder, so the catalog stops claiming it is installed. */
    fun forgetSerial(serial: String) {
        val root = read()
        val doomed = all().values.filter { it.serial.equals(serial, ignoreCase = true) }
        if (doomed.isEmpty()) return
        doomed.forEach { root.remove(it.packId) }
        write(root)
    }

    /**
     * Drop every entry whose textures are no longer on disk. [presentSerials] is the set of serials
     * that actually have a `textures/<SERIAL>/replacements` directory, upper-cased.
     *
     * This record and the filesystem are two stores that can disagree, and only one of them is the
     * truth. [forgetSerial] runs on an in-app delete and nothing else, so deleting a pack in a file
     * manager -- or an in-app delete that only partly succeeded -- left the entry behind claiming a
     * pack that is not there. The catalogue then shows a permanently greyed-out "Installed" for it,
     * and because that same flag disables the button, the user cannot reinstall it or switch to a
     * different pack for that game either. Reported by SKRazy.
     *
     * Reconciling one way only: presence on disk retires a stale entry, but a pack that exists with
     * no entry is left alone -- that is a hand-copied folder, which we cannot name a version for and
     * must not invent one.
     */
    fun reconcile(presentSerials: Set<String>) {
        val root = read()
        val known = all().values
        // An EMPTY scan against a non-empty record is almost always a failed scan — File.listFiles()
        // returning null on a FUSE/SAF path, or the textures root not resolved yet — not the user
        // deleting every pack at once. Trusting it wiped the entire record. Refuse and log instead;
        // a genuine "deleted everything" still reconciles one refresh later, once a scan succeeds
        // and reports at least one pack.
        if (presentSerials.isEmpty() && known.isNotEmpty()) {
            android.util.Log.w(
                "ARMSX2",
                "texture-pack reconcile skipped: scan found 0 packs but ${known.size} are recorded",
            )
            return
        }
        val doomed = known.filter { it.serial.uppercase() !in presentSerials }
        if (doomed.isEmpty()) return
        doomed.forEach { root.remove(it.packId) }
        write(root)
    }
}
