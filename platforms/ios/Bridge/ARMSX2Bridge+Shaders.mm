// ARMSX2Bridge+Shaders.mm — shader packs, librashader presets and MetalFX
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

// MetalFX is iOS 16+ and weak-linked (see PCSX2 CMake). The Simulator SDK has
// no MetalFX framework, so the import is gated off there and isMetalFXSupported
// returns NO without referencing MTLFXSpatialScalerDescriptor.
#import <Metal/Metal.h>
#if !TARGET_OS_SIMULATOR
	#import <MetalFX/MetalFX.h>
	#define ARMSX2_HAS_METALFX 1
#else
	#define ARMSX2_HAS_METALFX 0
#endif

// Only the runtime-independent preset API is used here, so no LIBRA_RUNTIME_* define is needed.
#ifdef ARMSX2_HAS_LIBRASHADER
#include "librashader.h"
#endif

#include "GS/Renderers/Common/GSDevice.h"
#include "common/ZipHelpers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

enum ARMSX2ShaderPackFailure {
    ARMSX2ShaderPackBadArgument = 1,
    ARMSX2ShaderPackUnreadable,
    ARMSX2ShaderPackTooLarge,
    ARMSX2ShaderPackEscapingEntry,
    ARMSX2ShaderPackWriteFailed,
};

static NSArray<NSURL*>* ARMSX2FailShaderPackExtraction(NSError** error, NSInteger code, NSString* message)
{
    if (error) {
        *error = [NSError errorWithDomain:@"ARMSX2ShaderPackExtraction"
                                     code:code
                                 userInfo:@{NSLocalizedDescriptionKey: message}];
    }
    NSLog(@"[ARMSX2 iOS Shaders] %@", message);
    return @[];
}

static BOOL ARMSX2IsShaderPackImportName(NSString* name)
{
    static NSSet<NSString*>* allowed;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        allowed = [[NSSet alloc] initWithArray:@[@"slangp", @"slang", @"glslp", @"glsl", @"cgp",
                                                 @"cg", @"inc", @"h", @"params", @"png", @"jpg",
                                                 @"jpeg", @"tga", @"bmp", @"txt", @"md"]];
    });
    return [allowed containsObject:name.pathExtension.lowercaseString];
}

static NSString* ARMSX2ContainedRelativePath(NSArray<NSString*>* components)
{
    NSMutableArray<NSString*>* kept = [NSMutableArray arrayWithCapacity:components.count];
    for (NSString* component in components) {
        if (component.length == 0 || [component isEqualToString:@"."])
            continue;
        if ([component isEqualToString:@".."] || [component isEqualToString:@"/"])
            return nil;
        [kept addObject:component];
    }
    return kept.count > 0 ? [NSString pathWithComponents:kept] : nil;
}

static NSString* ARMSX2CommonArchiveRoot(NSArray<NSString*>* names)
{
    NSString* root = names.firstObject.pathComponents.firstObject;
    if (names.firstObject.pathComponents.count < 2 || root.length == 0)
        return nil;

    for (NSString* name in names) {
        NSArray<NSString*>* components = name.pathComponents;
        if (components.count < 2 || ![components.firstObject isEqualToString:root])
            return nil;
    }
    return root;
}

static void ARMSX2RollBackShaderPack(NSArray<NSURL*>* files, NSArray<NSURL*>* directories)
{
    NSFileManager* manager = [NSFileManager defaultManager];
    for (NSURL* url in files)
        [manager removeItemAtURL:url error:nil];
    for (NSURL* url in directories.reverseObjectEnumerator)
        [manager removeItemAtURL:url error:nil];
}

@implementation ARMSX2Bridge (Shaders)

+ (nonnull NSArray<NSURL *> *)extractShaderPackArchiveAtURL:(nonnull NSURL *)archiveURL toDirectory:(nonnull NSURL *)destinationDirectory error:(NSError * _Nullable * _Nullable)error
{
    // Sized for the stock RetroArch pack of thousands of stages; the skin caps would truncate it.
    static const zip_uint64_t kMaxShaderPackEntryBytes = 8 * 1024 * 1024;
    static const zip_uint64_t kMaxShaderPackTotalBytes = 512 * 1024 * 1024;
    static const zip_int64_t kMaxShaderPackEntries = 32768;

    if (error)
        *error = nil;

    if (!archiveURL.isFileURL || !destinationDirectory.isFileURL)
        return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackBadArgument, @"Shader pack extraction needs file URLs.");

    NSFileManager *manager = [NSFileManager defaultManager];
    NSError *directoryError = nil;
    if (![manager createDirectoryAtURL:destinationDirectory
           withIntermediateDirectories:YES
                            attributes:nil
                                 error:&directoryError]) {
        return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackWriteFailed,
            [NSString stringWithFormat:@"Could not create %@: %@", destinationDirectory.path, directoryError.localizedDescription]);
    }

    char rootBuffer[PATH_MAX] = {};
    if (!realpath(destinationDirectory.path.fileSystemRepresentation, rootBuffer))
        return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackWriteFailed, @"Could not resolve the shader pack destination.");

    NSString *resolvedRoot = [manager stringWithFileSystemRepresentation:rootBuffer length:strlen(rootBuffer)];
    NSString *guardPrefix = [resolvedRoot stringByAppendingString:@"/"];

    zip_error_t ze = {};
    auto zf = zip_open_managed(archiveURL.path.UTF8String, ZIP_RDONLY, &ze);
    if (!zf) {
        return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackUnreadable,
            [NSString stringWithFormat:@"Could not open %@: %s", archiveURL.lastPathComponent, zip_error_strerror(&ze)]);
    }

    const zip_int64_t count = zip_get_num_entries(zf.get(), 0);
    if (count > kMaxShaderPackEntries) {
        return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackTooLarge,
            [NSString stringWithFormat:@"%@ has %lld entries, more than a shader pack should.", archiveURL.lastPathComponent, static_cast<long long>(count)]);
    }

    NSMutableArray<NSString *> *names = [NSMutableArray array];
    NSMutableArray<NSNumber *> *indices = [NSMutableArray array];
    BOOL hasStages = NO;
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(std::max<zip_int64_t>(count, 0)); i++) {
        zip_stat_t stat = {};
        if (zip_stat_index(zf.get(), i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
            continue;

        NSString *entryName = [NSString stringWithUTF8String:stat.name];
        if (entryName.length == 0 || [entryName hasSuffix:@"/"])
            continue;
        // Junk is dropped before the common-root check, or one __MACOSX/ entry would keep the top
        // folder from being stripped.
        if (ARMSX2IsArchiveJunkName(entryName))
            continue;

        [names addObject:entryName];
        [indices addObject:@(i)];
        hasStages = hasStages || [entryName.pathExtension.lowercaseString isEqualToString:@"slang"];
    }

    // A pack with no .slang files reaches other packs through ../ paths, so it keeps its top folder.
    NSString *commonRoot = hasStages ? ARMSX2CommonArchiveRoot(names) : nil;
    NSMutableArray<NSURL *> *extracted = [NSMutableArray array];
    NSMutableArray<NSURL *> *createdDirectories = [NSMutableArray array];
    zip_uint64_t totalBytes = 0;

    for (NSUInteger n = 0; n < names.count; n++) {
        NSString *entryName = names[n];
        NSArray<NSString *> *components = entryName.pathComponents;
        if (commonRoot && [components.firstObject isEqualToString:commonRoot])
            components = [components subarrayWithRange:NSMakeRange(1, components.count - 1)];

        NSString *relative = ARMSX2ContainedRelativePath(components);
        if (!relative) {
            ARMSX2RollBackShaderPack(extracted, createdDirectories);
            return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackEscapingEntry,
                [NSString stringWithFormat:@"%@ contains an entry that escapes its directory: %@", archiveURL.lastPathComponent, entryName]);
        }
        if (!ARMSX2IsShaderPackImportName(relative))
            continue;

        const zip_uint64_t index = indices[n].unsignedLongLongValue;
        zip_uint8_t opsys = 0;
        zip_uint32_t attributes = 0;
        if (zip_file_get_external_attributes(zf.get(), index, 0, &opsys, &attributes) == 0 &&
            opsys == ZIP_OPSYS_UNIX && ((attributes >> 16) & S_IFMT) == S_IFLNK) {
            ARMSX2RollBackShaderPack(extracted, createdDirectories);
            return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackEscapingEntry,
                [NSString stringWithFormat:@"%@ contains a symlink entry: %@", archiveURL.lastPathComponent, entryName]);
        }

        zip_stat_t stat = {};
        if (zip_stat_index(zf.get(), index, ZIP_FL_ENC_GUESS, &stat) != 0)
            continue;
        if ((stat.valid & ZIP_STAT_SIZE) && stat.size > kMaxShaderPackEntryBytes)
            continue;

        NSArray<NSString *> *relativeComponents = relative.pathComponents;
        NSURL *parentURL = destinationDirectory;
        for (NSUInteger c = 0; c + 1 < relativeComponents.count; c++) {
            parentURL = [parentURL URLByAppendingPathComponent:relativeComponents[c] isDirectory:YES];
            if ([manager fileExistsAtPath:parentURL.path])
                continue;
            if (![manager createDirectoryAtURL:parentURL
                   withIntermediateDirectories:NO
                                    attributes:nil
                                         error:&directoryError]) {
                ARMSX2RollBackShaderPack(extracted, createdDirectories);
                return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackWriteFailed,
                    [NSString stringWithFormat:@"Could not create %@: %@", parentURL.path, directoryError.localizedDescription]);
            }
            [createdDirectories addObject:parentURL];
        }

        // The tree is kept, so containment is checked on the resolved path at a component
        // boundary: <dest>-evil shares a prefix with <dest> but is outside it.
        char parentBuffer[PATH_MAX] = {};
        NSString *resolvedParent = realpath(parentURL.path.fileSystemRepresentation, parentBuffer) ?
            [manager stringWithFileSystemRepresentation:parentBuffer length:strlen(parentBuffer)] : nil;
        if (![resolvedParent isEqualToString:resolvedRoot] && ![resolvedParent hasPrefix:guardPrefix]) {
            ARMSX2RollBackShaderPack(extracted, createdDirectories);
            return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackEscapingEntry,
                [NSString stringWithFormat:@"%@ contains an entry that escapes its directory: %@", archiveURL.lastPathComponent, entryName]);
        }

        auto file = zip_fopen_index_managed(zf.get(), index, ZIP_FL_ENC_GUESS);
        if (!file)
            continue;

        std::optional<std::vector<u8>> data = ReadBinaryFileInZip(file.get());
        if (!data.has_value() || data->size() > kMaxShaderPackEntryBytes)
            continue;

        totalBytes += data->size();
        if (totalBytes > kMaxShaderPackTotalBytes) {
            ARMSX2RollBackShaderPack(extracted, createdDirectories);
            return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackTooLarge,
                [NSString stringWithFormat:@"%@ unpacks to more than a shader pack should.", archiveURL.lastPathComponent]);
        }

        NSURL *destinationURL = [parentURL URLByAppendingPathComponent:relativeComponents.lastObject isDirectory:NO];
        // Per entry, so autoreleased bytes from thousands of entries don't pile up until the end.
        bool written = false;
        @autoreleasepool {
            NSData *bytes = [NSData dataWithBytes:data->data() length:data->size()];
            written = [bytes writeToURL:destinationURL atomically:YES];
        }
        if (!written) {
            ARMSX2RollBackShaderPack(extracted, createdDirectories);
            return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackWriteFailed,
                [NSString stringWithFormat:@"Could not write %@", destinationURL.path]);
        }
        [extracted addObject:destinationURL];
    }

    NSLog(@"[ARMSX2 iOS Shaders] Extracted %lu file(s) from %@",
          static_cast<unsigned long>(extracted.count), archiveURL.lastPathComponent);
    return extracted;
}

// Works from the main menu before any GS device exists, so the settings UI can
// decide whether to show the Upscaler section. NO before iOS 16, on the
// simulator (compiled out) and on any GPU that fails the capability probe.
+ (BOOL)isMetalFXSupported {
#if ARMSX2_HAS_METALFX
	if (@available(iOS 16.0, *)) {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (!device)
			return NO;
		return [MTLFXSpatialScalerDescriptor supportsDevice:device];
	}
	return NO;
#else
	// iOS Simulator build: MetalFX framework absent at compile time.
	return NO;
 #endif
 }

#pragma mark - Shader chain parameters

#ifdef ARMSX2_HAS_LIBRASHADER
static void ARMSX2ShaderPresetFailure(libra_error_t err, NSError** error)
{
    char* msg = nullptr;
    if (error && libra_error_write(err, &msg) == 0 && msg)
    {
        *error = [NSError errorWithDomain:@"librashader"
                                     code:static_cast<NSInteger>(libra_error_errno(err))
                                 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:msg] ?: @""}];
        libra_error_free_string(&msg);
    }
    libra_error_free(&err);
}
#endif

// Uses its own preset handle, since creating a chain consumes the renderer's. Parsing needs no
// Metal device and no running VM.
+ (nullable NSString *)shaderPresetParametersAtPath:(nonnull NSString *)path error:(NSError * _Nullable * _Nullable)error {
#ifndef ARMSX2_HAS_LIBRASHADER
    return nil;
#else
    const char* filename = path.UTF8String;
    if (!filename)
        return nil;

    libra_shader_preset_t preset = nullptr;
    libra_error_t err = libra_preset_create(filename, &preset);
    if (err)
    {
        ARMSX2ShaderPresetFailure(err, error);
        return nil;
    }

    libra_preset_param_list_t params = {};
    err = libra_preset_get_runtime_params(&preset, &params);
    if (err)
    {
        ARMSX2ShaderPresetFailure(err, error);
        libra_preset_free(&preset);
        return nil;
    }

    const auto escape = [](const char* s) {
        std::string out;
        if (!s)
            return out;
        for (const char* p = s; *p; ++p)
        {
            switch (*p)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(*p) >= 0x20)
                        out += *p;
                    break;
            }
        }
        return out;
    };

    // Non-finite values become null, since "%g" would print nan or inf and break the JSON.
    const auto number = [](float value) {
        if (!std::isfinite(value))
            return std::string("null");
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
        return std::string(buffer);
    };

    std::string json("[");
    for (uint64_t i = 0; i < params.length; i++)
    {
        const libra_preset_param_t& p = params.parameters[i];
        if (i)
            json += ',';
        json += "{\"name\":\"" + escape(p.name);
        json += "\",\"description\":\"" + escape(p.description);
        json += "\",\"initial\":" + number(p.initial);
        json += ",\"minimum\":" + number(p.minimum);
        json += ",\"maximum\":" + number(p.maximum);
        json += ",\"step\":" + number(p.step) + "}";
    }
    json += ']';

    // free_runtime_params takes the list by value, and reading parameters doesn't consume the preset.
    libra_preset_free_runtime_params(params);
    libra_preset_free(&preset);

    return [NSString stringWithUTF8String:json.c_str()];
#endif
}

// Not behind ARMSX2_HAS_LIBRASHADER: the store holds plain values, and its reader is compiled
// out without librashader.
+ (void)setShaderChainParameters:(nonnull NSDictionary<NSString *, NSNumber *> *)params forPreset:(nonnull NSString *)preset {
    std::vector<std::pair<std::string, float>> values;
    values.reserve(params.count);
    for (NSString* name in params)
    {
        const char* utf8 = name.UTF8String;
        if (!utf8)
            continue;
        values.emplace_back(utf8, params[name].floatValue);
    }

    const char* path = preset.UTF8String;
    GSDevice::SetShaderChainParams(path ? std::string(path) : std::string(), std::move(values));
}

+ (void)retryShaderChain {
    GSDevice::RetryShaderChain();
}

+ (nullable NSError *)shaderChainErrorForPreset:(nonnull NSString *)path {
    std::string message;
    const char* utf8 = path.UTF8String;
    if (!utf8 || !GSDevice::GetShaderChainError(utf8, &message))
        return nil;
    return [NSError errorWithDomain:@"librashader" code:0
                           userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:message.c_str()] ?: @""}];
}

@end
