package com.armsx2

import java.text.Normalizer
import java.util.Locale

/**
 * Format-neutral archive path policy shared by the ZIP and tar+zstd extraction branches (plan
 * 2026-09-06-0905, R23-R24).
 *
 * Two distinct transformations live here. [validateTarPath] enforces the tar container contract
 * BEFORE anything is extracted: no absolute or escaping paths, bounded depth/length, sane
 * components. [replacementRelativePath] then strips whatever publish wrapper the pack uses
 * (`<SERIAL>/replacements/`, a GitHub-style root) to get the path relative to the staging
 * `replacements/` directory. Collision keys apply NFC normalization plus a locale-independent
 * lowercase so case- and Unicode-equivalent destinations fail the same way on every filesystem.
 */
internal object TextureArchivePath {
    const val MAX_PATH_BYTES = 4096
    const val MAX_COMPONENTS = 32
    const val MAX_COMPONENT_BYTES = 255

    /**
     * Validates one path as it appeared in a tar header (already UTF-8-decoded by the caller).
     * Accepts and strips the producer's leading "./". Returns the cleaned path, or null when the
     * entry violates the container contract and the whole archive must be rejected.
     */
    fun validateTarPath(raw: String): String? {
        var path = raw
        if (path.length > MAX_PATH_BYTES) return null
        if (path.startsWith("./")) path = path.substring(2)
        if (path.isEmpty()) return "" // the producer's root "./" directory entry
        if (path.startsWith("/") || path == ".." || path.startsWith("../")) return null
        // A single trailing slash is how tar spells a directory; GNU writes "replacements/".
        // Strip it before component checks (mirrors the producer validator), but keep rejecting
        // doubled slashes ("a//b") via the empty-component rule below.
        path = path.trimEnd('/')
        val parts = path.split("/")
        if (parts.size > MAX_COMPONENTS) return null
        for (part in parts) {
            when (part) {
                "" -> return null // empty component ("a//b")
                ".", ".." -> return null
                else -> if (part.toByteArray(Charsets.UTF_8).size > MAX_COMPONENT_BYTES) return null
            }
        }
        return path
    }

    /**
     * Collision key: NFC-normalized, locale-independent lowercase. Locale.ROOT (not the default
     * locale) so "I"()-style locale reordering can never make two devices disagree about which
     * entries collide.
     */
    fun collisionKey(path: String): String =
        Normalizer.normalize(path, Normalizer.Form.NFC).lowercase(Locale.ROOT)

    /** The core only loads PNG, DDS, raw ASTC, and ASTC KTX; everything else is pack metadata. */
    fun isTextureFile(name: String): Boolean {
        val lower = name.lowercase(Locale.ROOT)
        return lower.endsWith(".png") || lower.endsWith(".dds") ||
            lower.endsWith(".astc") || lower.endsWith(".ktx")
    }

    fun isJunkEntry(name: String): Boolean {
        val lower = name.lowercase(Locale.ROOT)
        return lower.startsWith("__macosx/") || lower.contains("/__macosx/") ||
            lower.endsWith("/.ds_store") || lower == ".ds_store" || lower.endsWith("/thumbs.db")
    }

    /**
     * Strips whatever wrapper the archive uses so files land directly in `replacements/`.
     *
     * Packs are published three ways: rooted at the textures themselves, wrapped in `<SERIAL>/`,
     * and wrapped in `<SERIAL>/replacements/`. GitHub's own zips add a `name-<40 hex>/` root on
     * top. Take everything after the last `replacements/` segment, else after a `<SERIAL>/`
     * segment, else drop a single GitHub-style root.
     */
    fun replacementRelativePath(name: String): String? {
        val norm = name.replace('\\', '/').trimStart('/')
        if (norm.isEmpty()) return null
        val parts = norm.split('/').filter { it.isNotEmpty() && it != "." }
        if (parts.isEmpty()) return null

        val repIdx = parts.indexOfLast { it.equals("replacements", ignoreCase = true) }
        if (repIdx >= 0 && repIdx < parts.size - 1) return parts.drop(repIdx + 1).joinToString("/")

        val serialIdx = parts.indexOfLast { Regex("^[A-Za-z]{4}-?[0-9]{5}$").matches(it) }
        if (serialIdx >= 0 && serialIdx < parts.size - 1) return parts.drop(serialIdx + 1).joinToString("/")

        if (parts.size > 1 && Regex("^.+-[0-9a-f]{40}$").matches(parts[0])) {
            return parts.drop(1).joinToString("/")
        }
        return parts.joinToString("/")
    }
}
