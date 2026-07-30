package com.armsx2

import com.armsx2.runtime.MainActivityRuntime
import org.json.JSONObject
import java.io.File

/**
 * Which catalog packs are installed, and at which version.
 *
 * The texture folder itself cannot answer this: a pack extracts to loose .png/.dds files under
 * `textures/<SERIAL>/replacements`, with nothing recording where they came from. Without this the
 * catalog could only ever offer "Get", never "Installed" or "Update available".
 */
object TexturePackInstallState {
    private const val FILE = "texture-packs.json"

    data class Installed(val packId: String, val serial: String, val version: String, val name: String)

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
            )
        }
        return out
    }

    fun record(packId: String, serial: String, version: String, name: String) {
        val root = read()
        root.put(packId, JSONObject().apply {
            put("serial", serial)
            put("version", version)
            put("name", name)
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
