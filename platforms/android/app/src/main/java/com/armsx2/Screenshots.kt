package com.armsx2

import android.content.ContentValues
import android.content.Context
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File

/**
 * Screenshot hotkey. The core writes the PNG itself — an upscaled capture of the emulated frame with
 * no touch overlay, OSD or letterboxing, which is exactly what the system screenshot button cannot
 * give you — but it lands in the app-private `snaps/` folder, and Android 11+ hides `Android/data`
 * from the Files app. So we copy it into the public gallery afterwards; otherwise the user presses
 * the button, sees "Saved screenshot to …" and then cannot find the file anywhere.
 */
object Screenshots {
    private const val ALBUM = "ARMSX2"

    /** Fire-and-forget: queues the capture on the GS thread, then publishes it once it lands. */
    fun capture(context: Context) {
        val dir = File(MainActivityRuntime.assetCopyRoot(context), "snaps")
        dir.mkdirs()
        // Filename must be unique and must end in .png for the core to accept it as a full path.
        val name = "ARMSX2_${System.currentTimeMillis()}.png"
        val target = File(dir, name)
        runCatching { NativeApp.saveScreenshot(target.absolutePath) }.onFailure { return }

        // The core renders and compresses off the GS thread, so the file appears a moment later.
        // Poll briefly rather than guessing a delay; give up quietly if it never shows (the core
        // already puts its own failure message on the OSD, so a second complaint adds nothing).
        kotlin.concurrent.thread(isDaemon = true, name = "screenshot-publish") {
            var waited = 0
            var lastSize = -1L
            while (waited < PUBLISH_TIMEOUT_MS) {
                Thread.sleep(POLL_MS.toLong())
                waited += POLL_MS
                if (!target.isFile) continue
                // Wait for the size to stop changing: publishing a half-written PNG would put a
                // truncated image in the user's gallery.
                val size = target.length()
                if (size > 0 && size == lastSize) {
                    runCatching { publish(context, target) }
                    return@thread
                }
                lastSize = size
            }
        }
    }

    /** Copy [png] into Pictures/ARMSX2 so it shows up in the gallery. */
    private fun publish(context: Context, png: File) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            // MediaStore owns the file; no storage permission needed on Q+.
            val values = ContentValues().apply {
                put(MediaStore.Images.Media.DISPLAY_NAME, png.name)
                put(MediaStore.Images.Media.MIME_TYPE, "image/png")
                put(MediaStore.Images.Media.RELATIVE_PATH, "${Environment.DIRECTORY_PICTURES}/$ALBUM")
                put(MediaStore.Images.Media.IS_PENDING, 1)
            }
            val resolver = context.contentResolver
            val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values) ?: return
            resolver.openOutputStream(uri)?.use { out -> png.inputStream().use { it.copyTo(out) } }
            values.clear()
            values.put(MediaStore.Images.Media.IS_PENDING, 0)
            resolver.update(uri, values, null, null)
        } else {
            // Pre-Q: a plain file write, then tell the media scanner it exists.
            val album = File(
                Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES),
                ALBUM,
            )
            album.mkdirs()
            val dest = File(album, png.name)
            png.inputStream().use { input -> dest.outputStream().use { input.copyTo(it) } }
            @Suppress("DEPRECATION")
            android.media.MediaScannerConnection.scanFile(
                context, arrayOf(dest.absolutePath), arrayOf("image/png"), null,
            )
        }
    }

    private const val POLL_MS = 100
    private const val PUBLISH_TIMEOUT_MS = 5_000
}
