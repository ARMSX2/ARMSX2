package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * JVM tests for the update-eligibility rule ([TexturePackInstallState.actionFor]): the B2
 * tar+zstd migration path, revision monotonicity, downgrade suppression, and legacy ZIP behavior.
 */
class TexturePackInstallStateTest {
    private val digest = "A".repeat(64)
    private val otherDigest = "B".repeat(64)

    private fun installed(
        schema: Int = 1,
        format: String = "zip",
        revision: Long = 0L,
        sha256: String = "",
        version: String = "v1",
    ) = TexturePackInstallState.Installed(
        packId = "pack-1", serial = "SCUS-00000", version = version, name = "Pack",
        schema = schema, format = format, archiveRevision = revision, sha256 = sha256,
    )

    private fun pack(
        format: TextureCatalog.ArchiveFormat,
        revision: Long = 1L,
        sha256: String = digest,
        version: String = "v1",
    ) = TextureCatalog.Pack(
        id = "pack-1", name = "Pack", gameTitle = "Game", serials = listOf("SCUS-00000"),
        version = version, authors = listOf("a"), credits = "", description = "", license = "",
        downloadUrl = "https://cdn.example.com/p.tar.zst", sourceUrl = "https://example.com",
        sizeBytes = 1000, sha256 = sha256, fileCount = 1,
        format = format, archiveRevision = revision, decompressedSizeBytes = 2000,
    )

    private fun b2(revision: Long = 1L, sha256: String = digest) =
        installed(schema = 2, format = "tar+zstd", revision = revision, sha256 = sha256)

    // ---- nothing installed ----------------------------------------------------------------------

    @Test
    fun nothingInstalledOffersInstall() {
        assertEquals(
            TexturePackInstallState.InstallAction.INSTALL,
            TexturePackInstallState.actionFor(null, pack(TextureCatalog.ArchiveFormat.TAR_ZSTD)),
        )
        assertEquals(
            TexturePackInstallState.InstallAction.INSTALL,
            TexturePackInstallState.actionFor(null, pack(TextureCatalog.ArchiveFormat.ZIP)),
        )
    }

    // ---- legacy ZIP install ----------------------------------------------------------------------

    @Test
    fun legacyZipStateUpdatesToB2() {
        assertEquals(
            TexturePackInstallState.InstallAction.UPDATE,
            TexturePackInstallState.actionFor(installed(), pack(TextureCatalog.ArchiveFormat.TAR_ZSTD)),
        )
    }

    @Test
    fun legacyZipVersionCompareUnchanged() {
        assertEquals(
            TexturePackInstallState.InstallAction.INSTALLED,
            TexturePackInstallState.actionFor(installed(), pack(TextureCatalog.ArchiveFormat.ZIP, version = "v1")),
        )
        assertEquals(
            TexturePackInstallState.InstallAction.UPDATE,
            TexturePackInstallState.actionFor(installed(), pack(TextureCatalog.ArchiveFormat.ZIP, version = "v2")),
        )
    }

    // ---- B2 tar+zstd install ----------------------------------------------------------------------

    @Test
    fun greaterB2RevisionUpdates() {
        assertEquals(
            TexturePackInstallState.InstallAction.UPDATE,
            TexturePackInstallState.actionFor(b2(revision = 1), pack(TextureCatalog.ArchiveFormat.TAR_ZSTD, revision = 2)),
        )
    }

    @Test
    fun equalRevisionWithMatchingDigestIsInstalled() {
        assertEquals(
            TexturePackInstallState.InstallAction.INSTALLED,
            TexturePackInstallState.actionFor(b2(revision = 3), pack(TextureCatalog.ArchiveFormat.TAR_ZSTD, revision = 3)),
        )
    }

    @Test
    fun equalRevisionWithChangedDigestConflicts() {
        assertEquals(
            TexturePackInstallState.InstallAction.CONFLICT,
            TexturePackInstallState.actionFor(b2(revision = 3), pack(TextureCatalog.ArchiveFormat.TAR_ZSTD, revision = 3, sha256 = otherDigest)),
        )
    }

    @Test
    fun lowerB2RevisionSuppressed() {
        // A lagging CDN or stale mirror must not walk a device backwards.
        assertEquals(
            TexturePackInstallState.InstallAction.INSTALLED,
            TexturePackInstallState.actionFor(b2(revision = 5), pack(TextureCatalog.ArchiveFormat.TAR_ZSTD, revision = 4)),
        )
    }

    @Test
    fun zipFallbackCannotDowngradeB2State() {
        assertEquals(
            TexturePackInstallState.InstallAction.INSTALLED,
            TexturePackInstallState.actionFor(b2(), pack(TextureCatalog.ArchiveFormat.ZIP, version = "v999")),
        )
    }

    @Test
    fun digestComparisonIsCaseInsensitive() {
        assertEquals(
            TexturePackInstallState.InstallAction.INSTALLED,
            TexturePackInstallState.actionFor(
                b2(revision = 2, sha256 = digest),
                pack(TextureCatalog.ArchiveFormat.TAR_ZSTD, revision = 2, sha256 = digest.lowercase()),
            ),
        )
    }
}
