package com.armsx2

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TexturePackInstallerTest {
    @Test
    fun recognizesKtxTexturesCaseInsensitively() {
        assertTrue(TexturePackInstaller.isTextureFile("texture.ktx"))
        assertTrue(TexturePackInstaller.isTextureFile("nested/TEXTURE.KTX"))
        assertFalse(TexturePackInstaller.isTextureFile("texture.ktx2"))
        assertFalse(TexturePackInstaller.isTextureFile("texture.ktx.txt"))
    }
}
