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

    private fun read(): JSONObject = runCatching {
        val f = stateFile()
        if (f.isFile) JSONObject(f.readText()) else JSONObject()
    }.getOrDefault(JSONObject())

    private fun write(root: JSONObject) {
        runCatching {
            val dest = stateFile()
            dest.parentFile?.mkdirs()
            // tmp + rename: a half-written state file would make installed packs look absent and
            // invite a second 1.5 GB download.
            val tmp = File(dest.parentFile, "$FILE.tmp")
            tmp.writeText(root.toString())
            if (dest.exists()) dest.delete()
            tmp.renameTo(dest)
        }
        revision.value = revision.value + 1
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
}
