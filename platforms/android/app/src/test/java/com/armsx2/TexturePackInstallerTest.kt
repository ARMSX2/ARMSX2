package com.armsx2

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TexturePackInstallerTest {
    @Test
    fun recognizesKtxTexturesCaseInsensitively() {
        // The policy itself moved to TextureArchivePath so ZIP and tar extraction share it.
        assertTrue(TextureArchivePath.isTextureFile("texture.ktx"))
        assertTrue(TextureArchivePath.isTextureFile("nested/TEXTURE.KTX"))
        assertFalse(TextureArchivePath.isTextureFile("texture.ktx2"))
        assertFalse(TextureArchivePath.isTextureFile("texture.ktx.txt"))
    }

    @Test
    fun tarPathsValidate() {
        // The producer's leading "./" is stripped; traversal and absolute paths are refused.
        assertTrue(TextureArchivePath.validateTarPath("./replacements/a.ktx") == "replacements/a.ktx")
        assertTrue(TextureArchivePath.validateTarPath("./") == "")
        assertTrue(TextureArchivePath.validateTarPath("/etc/passwd") == null)
        assertTrue(TextureArchivePath.validateTarPath("replacements/../../x") == null)
        assertTrue(TextureArchivePath.validateTarPath("a//b") == null)
        assertTrue(TextureArchivePath.validateTarPath("a/" + "c".repeat(300)) == null)
    }

    @Test
    fun collisionKeyIsNfcAndLocaleIndependent() {
        val a = TextureArchivePath.collisionKey("Replacements/Tür.ktx")
        val b = TextureArchivePath.collisionKey("replacements/tu\u0308r.ktx") // decomposed ü
        assertTrue(a == b)
        // Locale-independent: Turkish "I" must not dot itself, regardless of device locale.
        assertTrue(TextureArchivePath.collisionKey("FILE.KTX") == TextureArchivePath.collisionKey("file.ktx"))
    }
}
