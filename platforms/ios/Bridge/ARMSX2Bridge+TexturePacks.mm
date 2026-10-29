// ARMSX2Bridge+TexturePacks.mm — disc identity, texture reload and texture pack install
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "VMManager.h"
#include "pcsx2/MTGS.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "pcsx2/Host.h"
#include "common/FileSystem.h"
#include "common/ZipHelpers.h"
#include "TexturePackPaths.h"
#include "TexturePackTar.h"
#include "TexturePackZstd.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <unordered_set>
#include <vector>
#include <zstd.h>

@implementation ARMSX2Bridge (TexturePacks)

+ (nonnull NSString *)currentDiscIdentity {
    if (!VMManager::HasValidVM())
        return @"";
    const std::string serial = VMManager::GetDiscSerial();
    if (!serial.empty()) {
        NSString* s = [NSString stringWithUTF8String:serial.c_str()];
        return [[s stringByReplacingOccurrencesOfString:@"_" withString:@"-"] uppercaseString];
    }
    const u32 crc = VMManager::GetDiscCRC();
    if (crc != 0)
        return [NSString stringWithFormat:@"CRC-%08X", crc];
    return @"";
}

+ (nonnull NSString *)currentTextureSerial {
    if (!VMManager::HasValidVM())
        return @"";
    return [NSString stringWithUTF8String:VMManager::GetDiscSerial().c_str()];
}

+ (void)reloadTextureReplacements {
    if (!MTGS::IsOpen())
        return;
    Host::RunOnGSThread([]() {
        if (!g_gs_renderer)
            return;
        GSTextureReplacements::ReloadReplacementMap();
        g_gs_renderer->PurgeTextureCache(true, false, true);
    });
}

static NSString* ARMSX2FailTexturePack(NSError** error, NSString* message)
{
    if (error)
        *error = [NSError errorWithDomain:@"ARMSX2TexturePackInstall" code:1 userInfo:@{NSLocalizedDescriptionKey: message}];
    NSLog(@"[ARMSX2 iOS Textures] %@", message);
    return nil;
}

// Android's limits: a bigger single file is malformed rather than large, and so is a pack of more entries.
static const zip_uint64_t kMaxTextureBytes = 512ull * 1024 * 1024;
static const unsigned long long kMaxEntries = 100000;
static const unsigned long long kSpareBytes = 256ull * 1024 * 1024;

static NSString* ARMSX2NoSpaceForTexturePack(NSString* name, unsigned long long needed, unsigned long long available)
{
    return [NSString stringWithFormat:@"%@ needs %@ of free space and %@ is available.", name,
        [NSByteCountFormatter stringFromByteCount:static_cast<long long>(needed) countStyle:NSByteCountFormatterCountStyleFile],
        [NSByteCountFormatter stringFromByteCount:static_cast<long long>(available) countStyle:NSByteCountFormatterCountStyleFile]];
}

// Every entry is checked before anything is written, so a bad zip leaves nothing behind.
static NSString* ARMSX2UnpackTextureZip(NSURL* archiveURL, NSURL* staging, unsigned long long available,
    std::vector<std::string>& paths, std::optional<std::string>& serial)
{
    NSString* name = archiveURL.lastPathComponent;
    zip_error_t ze = {};
    auto zf = zip_open_managed(archiveURL.path.fileSystemRepresentation, ZIP_RDONLY, &ze);
    if (!zf)
        return [NSString stringWithFormat:@"Could not open %@: %s", name, zip_error_strerror(&ze)];

    std::vector<zip_uint64_t> indices;
    std::unordered_set<std::string> destinations;
    unsigned long long total = 0;
    const zip_int64_t count = zip_get_num_entries(zf.get(), 0);
    if (static_cast<unsigned long long>(std::max<zip_int64_t>(count, 0)) > kMaxEntries)
        return [NSString stringWithFormat:@"%@ has more than %llu entries.", name, kMaxEntries];
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(std::max<zip_int64_t>(count, 0)); i++) {
        zip_stat_t stat = {};
        if (zip_stat_index(zf.get(), i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
            continue;

        const TexturePackPaths::Entry entry = TexturePackPaths::Classify(stat.name);
        zip_uint8_t opsys = 0;
        zip_uint32_t attributes = 0;
        const bool symlink = zip_file_get_external_attributes(zf.get(), i, 0, &opsys, &attributes) == 0 &&
            opsys == ZIP_OPSYS_UNIX && ((attributes >> 16) & S_IFMT) == S_IFLNK;
        if (entry.kind == TexturePackPaths::Kind::Unsafe || symlink)
            return [NSString stringWithFormat:@"%@ contains an unsafe entry: %s", name, stat.name];
        if (!serial)
            serial = TexturePackPaths::SerialFolderIn(stat.name);
        if (entry.kind != TexturePackPaths::Kind::Texture)
            continue;
        if ((stat.valid & ZIP_STAT_SIZE) && stat.size > kMaxTextureBytes)
            return [NSString stringWithFormat:@"%s in %@ is too large to be a texture.", stat.name, name];
        if (!destinations.insert(TexturePackPaths::Lower(entry.path)).second)
            return [NSString stringWithFormat:@"%@ holds two files for %s.", name, entry.path.c_str()];

        total += stat.size;
        indices.push_back(i);
        paths.push_back(entry.path);
    }
    if (available < total + kSpareBytes)
        return ARMSX2NoSpaceForTexturePack(name, total + kSpareBytes, available);

    // The sizes above are only what the zip claims, so the real bytes are counted again as they land.
    NSFileManager* manager = [NSFileManager defaultManager];
    std::vector<char> chunk(256 * 1024);
    unsigned long long written = 0;
    for (size_t n = 0; n < paths.size(); n++) {
        NSURL* destination = [staging URLByAppendingPathComponent:@(paths[n].c_str()) isDirectory:NO];
        auto file = zip_fopen_index_managed(zf.get(), indices[n], 0);
        [manager createDirectoryAtURL:destination.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
        std::FILE* out = file ? std::fopen(destination.path.fileSystemRepresentation, "wb") : nullptr;
        bool ok = out != nullptr;
        zip_uint64_t size = 0;
        zip_int64_t got = 0;
        while (ok && (got = zip_fread(file.get(), chunk.data(), chunk.size())) > 0) {
            size += static_cast<zip_uint64_t>(got);
            written += static_cast<unsigned long long>(got);
            if (written + kSpareBytes > available) {
                std::fclose(out);
                return [NSString stringWithFormat:@"%@ does not fit in the free space.", name];
            }
            ok = size <= kMaxTextureBytes && std::fwrite(chunk.data(), 1, static_cast<size_t>(got), out) == static_cast<size_t>(got);
        }
        ok = (!out || std::fclose(out) == 0) && ok && got == 0;
        if (!ok)
            return [NSString stringWithFormat:@"Could not unpack %s from %@.", paths[n].c_str(), name];
    }
    return nil;
}

// Streams straight from the decoder into files, so no uncompressed tar ever exists on disk.
static NSString* ARMSX2UnpackTextureTarZstd(NSURL* archiveURL, NSURL* staging, unsigned long long available,
    std::vector<std::string>& paths, std::optional<std::string>& serial)
{
    NSString* name = archiveURL.lastPathComponent;
    auto fp = FileSystem::OpenManagedCFile(archiveURL.path.fileSystemRepresentation, "rb");
    if (!fp)
        return [NSString stringWithFormat:@"Could not open %@.", name];

    unsigned char head[32]; // more than any zstd frame header
    const unsigned long long content = ZSTD_getFrameContentSize(head, std::fread(head, 1, sizeof(head), fp.get()));
    if (content != ZSTD_CONTENTSIZE_UNKNOWN && content != ZSTD_CONTENTSIZE_ERROR && available < content + kSpareBytes)
        return ARMSX2NoSpaceForTexturePack(name, content + kSpareBytes, available);
    std::rewind(fp.get());

    TexturePackZstd zstd(fp.get());
    if (!zstd.Valid())
        return [NSString stringWithFormat:@"Could not open %@.", name];
    const auto read = [&zstd](void* buffer, size_t n) { return zstd.Read(buffer, n); };

    NSFileManager* manager = [NSFileManager defaultManager];
    std::unordered_set<std::string> destinations;
    std::vector<char> chunk(256 * 1024);
    std::string error;
    // The frame header's size is optional and can lie, so the real bytes are counted as they land.
    unsigned long long entries = 0, written = 0;
    const bool ok = TexturePackTar::Read(read, [&](const std::string& entry_name, uint64_t size) {
        if (++entries > kMaxEntries) {
            error = "it has more than " + std::to_string(kMaxEntries) + " entries";
            return false;
        }
        const TexturePackPaths::Entry entry = TexturePackPaths::Classify(entry_name);
        if (entry.kind == TexturePackPaths::Kind::Unsafe) {
            error = "it contains an unsafe entry: " + entry_name;
            return false;
        }
        if (!serial)
            serial = TexturePackPaths::SerialFolderIn(entry_name);

        std::FILE* file = nullptr;
        if (entry.kind == TexturePackPaths::Kind::Texture) {
            if (size > kMaxTextureBytes || !destinations.insert(TexturePackPaths::Lower(entry.path)).second) {
                error = "it holds an oversized or repeated file: " + entry.path;
                return false;
            }
            NSURL* destination = [staging URLByAppendingPathComponent:@(entry.path.c_str()) isDirectory:NO];
            [manager createDirectoryAtURL:destination.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
            file = std::fopen(destination.path.fileSystemRepresentation, "wb");
            if (!file) {
                error = "could not write " + entry.path;
                return false;
            }
            paths.push_back(entry.path);
        }

        uint64_t left = size;
        bool wrote = true;
        while (left > 0) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(left, chunk.size()));
            if (!read(chunk.data(), n))
                break;
            if (file && (written += n) + kSpareBytes > available) {
                std::fclose(file);
                error = "it does not fit in the free space";
                return false;
            }
            wrote = wrote && (!file || std::fwrite(chunk.data(), 1, n, file) == n);
            left -= n;
        }
        wrote = (!file || std::fclose(file) == 0) && wrote;
        if (!wrote)
            error = "could not write " + entry.path;
        return wrote && left == 0;
    }, error);
    if (!ok)
        return [NSString stringWithFormat:@"Could not unpack %@: %s", name, error.c_str()];
    return nil;
}

+ (nullable NSString *)installTexturePackAtURL:(nonnull NSURL *)archiveURL serial:(nonnull NSString *)forcedSerial fallbackSerial:(nonnull NSString *)fallbackSerial error:(NSError * _Nullable * _Nullable)error
{
    NSString* name = archiveURL.lastPathComponent;
    NSFileManager* manager = [NSFileManager defaultManager];
    NSURL* root = [NSURL fileURLWithPath:[[self documentsDirectory] stringByAppendingPathComponent:@"textures"] isDirectory:YES];
    NSNumber* capacity = nil;
    [root getResourceValue:&capacity forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil];
    const unsigned long long available = capacity ? capacity.unsignedLongLongValue : ULLONG_MAX;

    NSURL* staging = [root URLByAppendingPathComponent:[@".import-" stringByAppendingString:NSUUID.UUID.UUIDString] isDirectory:YES];
    std::vector<std::string> paths;
    std::optional<std::string> serial;
    if (forcedSerial.length > 0)
        serial = forcedSerial.UTF8String;
    const BOOL tar = [name.lowercaseString hasSuffix:@".zst"] || [name.lowercaseString hasSuffix:@".tzst"];
    NSString* failure = tar ? ARMSX2UnpackTextureTarZstd(archiveURL, staging, available, paths, serial) :
                              ARMSX2UnpackTextureZip(archiveURL, staging, available, paths, serial);
    if (!failure && paths.empty())
        failure = [NSString stringWithFormat:@"%@ has no PNG, DDS, ASTC or KTX textures.", name];

    if (!serial)
        serial = TexturePackPaths::FindSerial(name.UTF8String);
    if (!serial && fallbackSerial.length > 0)
        serial = fallbackSerial.UTF8String;
    if (!failure && !serial)
        failure = [NSString stringWithFormat:@"Could not tell which game %@ is for. Put the game's serial, like SLUS-21137, in its name, or start the game first.", name];
    if (failure) {
        [manager removeItemAtURL:staging error:nil];
        return ARMSX2FailTexturePack(error, failure);
    }

    // Merges over an existing pack, as Android's folder import does, so a mod keeps its base pack.
    NSURL* target = [[root URLByAppendingPathComponent:@(serial->c_str()) isDirectory:YES] URLByAppendingPathComponent:@"replacements" isDirectory:YES];
    NSError* moveError = nil;
    bool moved = true;
    for (const std::string& path : paths) {
        NSURL* from = [staging URLByAppendingPathComponent:@(path.c_str()) isDirectory:NO];
        NSURL* to = [target URLByAppendingPathComponent:@(path.c_str()) isDirectory:NO];
        [manager removeItemAtURL:to error:nil];
        if (![manager createDirectoryAtURL:to.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:&moveError] ||
            ![manager moveItemAtURL:from toURL:to error:&moveError]) {
            moved = false;
            break;
        }
    }
    [manager removeItemAtURL:staging error:nil];
    if (!moved)
        return ARMSX2FailTexturePack(error, [NSString stringWithFormat:@"Could not install %@: %@", name, moveError.localizedDescription]);
    return @(serial->c_str());
}

@end
