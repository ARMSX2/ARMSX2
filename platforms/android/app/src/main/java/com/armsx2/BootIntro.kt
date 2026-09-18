package com.armsx2

import android.content.Context
import android.net.Uri
import androidx.compose.runtime.mutableStateOf
import androidx.core.content.edit
import com.armsx2.runtime.MainActivityRuntime
import java.io.File

/**
 * A user-chosen boot intro, played by [BootSplashActivity] in place of the bundled one.
 *
 * Same model as [LibraryMusic]'s custom track: the picked video is COPIED into app-private
 * storage rather than referenced by its content URI. An intro plays on every cold start, so
 * it has to survive everything a URI does not -- the grant expiring, the source being moved or
 * deleted, an SD card that is not mounted yet at boot. A local file has none of those failure
 * modes, and it is what the splash reads fastest.
 *
 * The FILE is the setting. [BootSplashActivity] runs before Main has initialised
 * [MainActivityRuntime.prefs], so it cannot read a preference from there; it just asks whether
 * [customFile] exists and is non-empty. The display name in prefs is for the settings row only.
 */
object BootIntro {
    private const val CustomNameKey = "ui.bootIntro.customName"

    /**
     * Refuse anything larger. An intro is seconds long; a file this size is a film picked by
     * mistake, and copying it would quietly eat storage and slow every cold boot.
     */
    const val MaxBytes = 200L * 1024 * 1024

    /** The chosen file's display name, or null when the bundled intro is in use. */
    val customName = mutableStateOf<String?>(null)

    /** Where the copy lives. Read directly by the splash, so it must not touch prefs. */
    fun customFile(context: Context): File =
        File(File(context.filesDir, "bootintro").apply { mkdirs() }, "intro")

    /** True when a usable custom intro is on disk. Safe to call before Main is up. */
    fun hasCustom(context: Context): Boolean = customFile(context).length() > 0L

    fun load(context: Context) {
        // Trust the file over the pref: if the copy is gone (cleared storage), the bundled intro
        // is what will actually play, and the settings row should say so.
        customName.value = if (hasCustom(context)) {
            MainActivityRuntime.prefs.getString(CustomNameKey, null) ?: "Custom intro"
        } else {
            null
        }
    }

    enum class SetResult { OK, TOO_LARGE, UNREADABLE }

    fun setCustom(context: Context, uri: Uri, displayName: String): SetResult {
        val size = runCatching {
            androidx.documentfile.provider.DocumentFile.fromSingleUri(context, uri)?.length() ?: -1L
        }.getOrDefault(-1L)
        if (size > MaxBytes) return SetResult.TOO_LARGE

        // Copy to a temp name and rename on success, so a failed or partial copy never replaces
        // an intro that was working.
        val target = customFile(context)
        val temp = File(target.parentFile, "intro.part")
        val ok = runCatching {
            context.contentResolver.openInputStream(uri)?.use { ins ->
                temp.outputStream().use { out ->
                    // Enforced while copying too: a provider that reports no size (-1) still
                    // cannot slip a huge file past the cap.
                    val buf = ByteArray(64 * 1024)
                    var total = 0L
                    while (true) {
                        val n = ins.read(buf)
                        if (n < 0) break
                        total += n
                        if (total > MaxBytes) return SetResult.TOO_LARGE.also { temp.delete() }
                        out.write(buf, 0, n)
                    }
                }
            } != null
        }.getOrDefault(false)
        if (!ok || temp.length() == 0L) {
            temp.delete()
            return SetResult.UNREADABLE
        }
        target.delete()
        if (!temp.renameTo(target)) {
            temp.delete()
            return SetResult.UNREADABLE
        }
        customName.value = displayName
        MainActivityRuntime.prefs.edit { putString(CustomNameKey, displayName) }
        return SetResult.OK
    }

    /** Drop the custom intro and go back to the bundled one. */
    fun clearCustom(context: Context) {
        customFile(context).delete()
        customName.value = null
        MainActivityRuntime.prefs.edit { remove(CustomNameKey) }
    }
}
