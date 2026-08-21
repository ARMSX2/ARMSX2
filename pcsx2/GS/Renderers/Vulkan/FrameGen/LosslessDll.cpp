// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/frame_gen/lossless_dll.cpp.
// The PE walk, the resource tree traversal and the variant selection are verbatim — this parses
// a third-party binary format, so staying diffable against upstream is worth more than restyling
// it to PCSX2's brace conventions. Only three platform seams are remapped:
//   * paths are std::string rather than std::filesystem::path,
//   * Common::FS::IOFile becomes FileSystem::ReadBinaryFile / WriteBinaryFile,
//   * Common::CityHash64 becomes GSXXH3_64bits.
// Each is marked PORT below where it changes the shape of the surrounding code.

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "common/FileSystem.h"
#include "common/Path.h"

#include "GS/GSXXH.h"

#include "LosslessDll.h"
#include "LsfgTranslate.h"

namespace VideoCore::FrameGen {

namespace {

constexpr u16 DOS_MAGIC = 0x5A4D;
constexpr u32 PE_SIGNATURE = 0x00004550;
constexpr u16 PE32_MAGIC = 0x010B;
constexpr u16 PE32_PLUS_MAGIC = 0x020B;

constexpr size_t DOS_LFANEW_OFFSET = 0x3C;
constexpr size_t COFF_HEADER_SIZE = 20;
constexpr size_t OPTIONAL_HEADER_SIZE_OFFSET = 16;
constexpr size_t SECTION_HEADER_SIZE = 40;
constexpr size_t DATA_DIRECTORY_ENTRY_SIZE = 8;
constexpr size_t DATA_DIRECTORY_OFFSET_PE32 = 96;
constexpr size_t DATA_DIRECTORY_OFFSET_PE32_PLUS = 112;
constexpr size_t RESOURCE_DATA_DIRECTORY_INDEX = 2;

constexpr size_t RESOURCE_DIRECTORY_SIZE = 16;
constexpr size_t RESOURCE_NAMED_COUNT_OFFSET = 12;
constexpr size_t RESOURCE_ID_COUNT_OFFSET = 14;
constexpr size_t RESOURCE_ENTRY_SIZE = 8;
constexpr u32 RESOURCE_SUBDIRECTORY_FLAG = 0x80000000;
constexpr u32 RESOURCE_TYPE_RCDATA = 10;

constexpr u32 MIPMAPS_SHADER_ID = 255;
constexpr u32 GENERATE_SHADER_ID = 256;
constexpr u32 PERFORMANCE_SHADER_ID_FIRST = 280;
constexpr u32 PERFORMANCE_SHADER_ID_LAST = 302;

constexpr u32 CACHE_MAGIC = 0x4746534C;
constexpr u32 CACHE_VERSION = 3;

// PORT: Eden gets this from LOSSLESS_CACHE_FILE in common/fs/fs_paths.h. Same value, so a cache
// written by either project stays readable by the other.
constexpr const char* CACHE_FILE_NAME = "lsfg_spirv.cache";

struct CacheHeader {
    u32 magic;
    u32 version;
    u64 source_size;
    u64 source_hash;
    // PORT: mtime and flags are ours, and they are what make a cache hit CHEAP.
    //
    // Eden validates on size + content hash, which means it must read and hash the whole DLL
    // before it can even look at the cache — so a "hit" still paid for the multi-megabyte read,
    // the hash and the PE walk. Recording mtime lets the common case be answered from a stat(),
    // which is what GSLsfg.cpp already does in this tree.
    //
    // `flags` records the allow_fp16/prefer_fp16 the cached variant was selected under. Without
    // it, toggling the performance-shader setting would silently keep serving the old variant,
    // because the DLL itself has not changed.
    u64 source_mtime;
    u32 module_count;
    u32 variant;
    u32 flags;
    u32 reserved;
};

// PORT: Eden writes this struct through IOFile::Write, which is a raw object copy. The cache is
// serialized by hand below, so pin the layout it assumes — a padding hole here would silently
// change the on-disk format.
static_assert(sizeof(CacheHeader) == 48, "CacheHeader must be padding-free to serialize raw");

struct Section {
    u32 virtual_address;
    u32 virtual_size;
    u32 raw_address;
    u32 raw_size;
};

struct ResourceEntry {
    u32 id;
    u32 offset;
    bool is_directory;
    bool is_named;
};

class ImageReader {
public:
    explicit ImageReader(std::span<const u8> image_) : image{image_} {}

    template <typename T>
    [[nodiscard]] bool Read(size_t offset, T& out_value) const {
        if (offset > image.size() || image.size() - offset < sizeof(T)) {
            return false;
        }
        std::memcpy(&out_value, image.data() + offset, sizeof(T));
        return true;
    }

    [[nodiscard]] bool Slice(size_t offset, size_t size, std::span<const u8>& out_slice) const {
        if (offset > image.size() || image.size() - offset < size) {
            return false;
        }
        out_slice = image.subspan(offset, size);
        return true;
    }

private:
    std::span<const u8> image;
};

[[nodiscard]] std::optional<size_t> FindPeHeader(const ImageReader& reader) {
    u16 dos_magic{};
    if (!reader.Read(0, dos_magic) || dos_magic != DOS_MAGIC) {
        return std::nullopt;
    }

    u32 pe_offset{};
    if (!reader.Read(DOS_LFANEW_OFFSET, pe_offset)) {
        return std::nullopt;
    }

    u32 pe_signature{};
    if (!reader.Read(pe_offset, pe_signature) || pe_signature != PE_SIGNATURE) {
        return std::nullopt;
    }

    return static_cast<size_t>(pe_offset);
}

[[nodiscard]] std::optional<size_t> FindDataDirectory(const ImageReader& reader,
                                                      size_t optional_header_offset) {
    u16 optional_magic{};
    if (!reader.Read(optional_header_offset, optional_magic)) {
        return std::nullopt;
    }

    switch (optional_magic) {
    case PE32_MAGIC:
        return optional_header_offset + DATA_DIRECTORY_OFFSET_PE32;
    case PE32_PLUS_MAGIC:
        return optional_header_offset + DATA_DIRECTORY_OFFSET_PE32_PLUS;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool ReadSections(const ImageReader& reader, size_t pe_offset,
                                std::vector<Section>& out_sections) {
    u16 section_count{};
    u16 optional_header_size{};
    if (!reader.Read(pe_offset + 4 + 2, section_count) ||
        !reader.Read(pe_offset + 4 + OPTIONAL_HEADER_SIZE_OFFSET, optional_header_size)) {
        return false;
    }

    const size_t table_offset = pe_offset + 4 + COFF_HEADER_SIZE + optional_header_size;
    out_sections.reserve(section_count);
    for (size_t i = 0; i < section_count; ++i) {
        const size_t offset = table_offset + i * SECTION_HEADER_SIZE;
        Section section{};
        if (!reader.Read(offset + 8, section.virtual_size) ||
            !reader.Read(offset + 12, section.virtual_address) ||
            !reader.Read(offset + 16, section.raw_size) ||
            !reader.Read(offset + 20, section.raw_address)) {
            return false;
        }
        out_sections.push_back(section);
    }
    return true;
}

[[nodiscard]] std::optional<size_t> RvaToFileOffset(std::span<const Section> sections, u32 rva) {
    for (const Section& section : sections) {
        const u32 span = std::max(section.virtual_size, section.raw_size);
        if (span == 0 || rva < section.virtual_address) {
            continue;
        }
        const u32 relative = rva - section.virtual_address;
        if (relative < span) {
            return static_cast<size_t>(section.raw_address) + relative;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool ReadResourceEntries(const ImageReader& reader, size_t directory_offset,
                                       std::vector<ResourceEntry>& out_entries) {
    u16 named_count{};
    u16 id_count{};
    if (!reader.Read(directory_offset + RESOURCE_NAMED_COUNT_OFFSET, named_count) ||
        !reader.Read(directory_offset + RESOURCE_ID_COUNT_OFFSET, id_count)) {
        return false;
    }

    const size_t total = size_t{named_count} + size_t{id_count};
    out_entries.clear();
    out_entries.reserve(total);
    for (size_t i = 0; i < total; ++i) {
        const size_t offset = directory_offset + RESOURCE_DIRECTORY_SIZE + i * RESOURCE_ENTRY_SIZE;
        u32 name{};
        u32 data{};
        if (!reader.Read(offset, name) || !reader.Read(offset + 4, data)) {
            return false;
        }
        out_entries.push_back(ResourceEntry{
            .id = name & ~RESOURCE_SUBDIRECTORY_FLAG,
            .offset = data & ~RESOURCE_SUBDIRECTORY_FLAG,
            .is_directory = (data & RESOURCE_SUBDIRECTORY_FLAG) != 0,
            .is_named = (name & RESOURCE_SUBDIRECTORY_FLAG) != 0,
        });
    }
    return true;
}

[[nodiscard]] bool ReadResourceLeaf(const ImageReader& reader, std::span<const Section> sections,
                                    size_t leaf_offset, std::span<const u8>& out_data) {
    u32 data_rva{};
    u32 data_size{};
    if (!reader.Read(leaf_offset, data_rva) || !reader.Read(leaf_offset + 4, data_size) ||
        data_size == 0) {
        return false;
    }

    const std::optional<size_t> data_offset = RvaToFileOffset(sections, data_rva);
    if (!data_offset) {
        return false;
    }
    return reader.Slice(*data_offset, data_size, out_data);
}

using ResourceSpans = std::map<u32, std::span<const u8>>;

[[nodiscard]] bool CollectRcData(const ImageReader& reader, std::span<const Section> sections,
                                 size_t resource_base, ResourceSpans& out_resources) {
    std::vector<ResourceEntry> type_entries;
    if (!ReadResourceEntries(reader, resource_base, type_entries)) {
        return false;
    }

    for (const ResourceEntry& type_entry : type_entries) {
        if (type_entry.is_named || type_entry.id != RESOURCE_TYPE_RCDATA ||
            !type_entry.is_directory) {
            continue;
        }

        std::vector<ResourceEntry> name_entries;
        if (!ReadResourceEntries(reader, resource_base + type_entry.offset, name_entries)) {
            return false;
        }

        for (const ResourceEntry& name_entry : name_entries) {
            if (name_entry.is_named || !name_entry.is_directory) {
                continue;
            }

            std::vector<ResourceEntry> language_entries;
            if (!ReadResourceEntries(reader, resource_base + name_entry.offset, language_entries)) {
                return false;
            }

            for (const ResourceEntry& language_entry : language_entries) {
                if (language_entry.is_directory) {
                    continue;
                }
                std::span<const u8> data;
                if (!ReadResourceLeaf(reader, sections, resource_base + language_entry.offset,
                                      data)) {
                    continue;
                }
                out_resources.insert_or_assign(name_entry.id, data);
                break;
            }
        }
    }

    return true;
}

[[nodiscard]] std::vector<u32> PerformanceShaderIds() {
    std::vector<u32> ids{MIPMAPS_SHADER_ID, GENERATE_SHADER_ID};
    for (u32 id = PERFORMANCE_SHADER_ID_FIRST; id <= PERFORMANCE_SHADER_ID_LAST; ++id) {
        ids.push_back(id);
    }
    return ids;
}

template <typename Map>
[[nodiscard]] bool HasPerformanceShaders(const Map& resources) {
    const std::vector<u32> ids = PerformanceShaderIds();
    return std::ranges::all_of(ids, [&](u32 id) { return resources.contains(id); });
}

[[nodiscard]] u32 VariantOffset(ShaderVariant variant) {
    return variant == ShaderVariant::NativeFp16 ? PerformanceShader::NATIVE_FP16_OFFSET
                                                : PerformanceShader::NATIVE_FP32_OFFSET;
}

template <typename Map>
[[nodiscard]] bool HasNativeVariant(const Map& resources, ShaderVariant variant) {
    const u32 offset = VariantOffset(variant);
    return std::ranges::all_of(PerformanceShaderIds(), [&](u32 id) {
        const auto hit = resources.find(id + offset);
        return hit != resources.end() && IsSpirvModule(hit->second);
    });
}

[[nodiscard]] std::optional<ShaderVariant> SelectVariant(const ResourceSpans& resources,
                                                        bool allow_fp16, bool prefer_fp16) {
    if (prefer_fp16 && HasNativeVariant(resources, ShaderVariant::NativeFp16)) {
        return ShaderVariant::NativeFp16;
    }
    if (HasNativeVariant(resources, ShaderVariant::NativeFp32)) {
        return ShaderVariant::NativeFp32;
    }
    if (allow_fp16 && HasNativeVariant(resources, ShaderVariant::NativeFp16)) {
        return ShaderVariant::NativeFp16;
    }
    return std::nullopt;
}

[[nodiscard]] LosslessStatus TranslateAll(const ResourceSpans& resources,
                                          ShaderModules& out_modules,
                                          ShaderVariant variant) {
    const u32 offset = VariantOffset(variant);
    out_modules.clear();
    for (const u32 id : PerformanceShaderIds()) {
        const auto hit = resources.find(id + offset);
        if (hit == resources.end()) {
            return LosslessStatus::MissingShaders;
        }
        std::vector<u32> adopted = AdoptSpirvModule(hit->second);
        if (adopted.empty()) {
            return LosslessStatus::TranslationFailed;
        }
        out_modules.emplace(id, std::move(adopted));
    }
    return LosslessStatus::Ok;
}

void AppendRaw(std::vector<u8>& out, const void* data, size_t size) {
    const u8* const bytes = static_cast<const u8*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

// PORT: Eden streams the cache out through Common::FS::IOFile. PCSX2's FileSystem has no
// incremental writer, so the identical byte layout is assembled in memory and written in one
// call. That also makes the write all-or-nothing rather than leaving a half-written cache behind.
[[nodiscard]] bool WriteShaderCache(const std::string& path, const CacheHeader& header,
                                    const ShaderModules& modules) {
    std::vector<u8> blob;
    AppendRaw(blob, &header, sizeof(header));

    for (const auto& [id, words] : modules) {
        const u32 word_count = static_cast<u32>(words.size());
        AppendRaw(blob, &id, sizeof(id));
        AppendRaw(blob, &word_count, sizeof(word_count));
        AppendRaw(blob, words.data(), words.size() * sizeof(u32));
    }

    return FileSystem::WriteBinaryFile(path.c_str(), blob.data(), blob.size());
}

// PORT: likewise the read side — the whole file is pulled in at once and walked with a cursor.
// The layout and every validation below are unchanged from Eden.
/// The two runtime bools folded into one field, so the cache can tell whether the variant it
/// holds is still the one that would be selected now.
[[nodiscard]] constexpr u32 VariantFlags(bool allow_fp16, bool prefer_fp16) {
    return (allow_fp16 ? 1u : 0u) | (prefer_fp16 ? 2u : 0u);
}

/// Size + mtime of the source DLL without reading it. Same idiom as GSLsfg.cpp's StatSourceDll.
[[nodiscard]] bool StatSource(const std::string& path, u64& out_size, u64& out_mtime) {
    FILESYSTEM_STAT_DATA sd = {};
    if (path.empty() || !FileSystem::StatFile(path.c_str(), &sd)) {
        return false;
    }
    out_size = static_cast<u64>(sd.Size);
    out_mtime = static_cast<u64>(sd.ModificationTime);
    return true;
}

/// [source_hash] of 0 means "not computed" — the fast path has not read the DLL, so it validates
/// on size + mtime alone. That is a weaker guarantee than a content hash and it is a deliberate
/// trade: it is exactly what GSLsfg.cpp in this tree already relies on, and the alternative is
/// paying a full read + hash on every launch to catch a file that changed while keeping both its
/// size and its timestamp. [variant] of ~0u means "whatever was cached", for the same reason.
[[nodiscard]] bool ReadShaderCache(const std::string& path, u64 source_size, u64 source_hash,
                                   u64 source_mtime, u32 variant, u32 flags,
                                   ShaderModules& out_modules) {
    if (!FileSystem::FileExists(path.c_str())) {
        return false;
    }

    const std::optional<std::vector<u8>> blob = FileSystem::ReadBinaryFile(path.c_str());
    if (!blob) {
        return false;
    }

    size_t cursor = 0;
    const auto read_raw = [&](void* out_value, size_t size) {
        if (blob->size() - cursor < size) {
            return false;
        }
        std::memcpy(out_value, blob->data() + cursor, size);
        cursor += size;
        return true;
    };

    CacheHeader header{};
    if (!read_raw(&header, sizeof(header))) {
        return false;
    }
    if (header.magic != CACHE_MAGIC || header.version != CACHE_VERSION ||
        header.source_size != source_size || header.source_mtime != source_mtime ||
        header.flags != flags) {
        return false;
    }
    if (source_hash != 0 && header.source_hash != source_hash) {
        return false;
    }
    if (variant != ~0u && header.variant != variant) {
        return false;
    }

    out_modules.clear();
    for (u32 i = 0; i < header.module_count; ++i) {
        u32 id{};
        u32 word_count{};
        if (!read_raw(&id, sizeof(id)) || !read_raw(&word_count, sizeof(word_count)) ||
            word_count == 0) {
            return false;
        }

        // PORT: bounds-check before allocating. Eden reaches the same verdict one step later, when
        // IOFile::Read comes up short — but only after word_count, which is untrusted and up to
        // 4G, has already been turned into an allocation.
        if (word_count > (blob->size() - cursor) / sizeof(u32)) {
            return false;
        }

        std::vector<u32> words(word_count);
        if (!read_raw(words.data(), words.size() * sizeof(u32))) {
            return false;
        }
        out_modules.emplace(id, std::move(words));
    }

    return HasPerformanceShaders(out_modules);
}

[[nodiscard]] LosslessStatus ReadImageFile(const std::string& path,
                                          std::vector<u8>& out_image) {
    // PORT: the path is user-configured and may simply be unset, which reads as "not installed".
    if (path.empty() || !FileSystem::FileExists(path.c_str())) {
        return LosslessStatus::NotInstalled;
    }

    std::optional<std::vector<u8>> image = FileSystem::ReadBinaryFile(path.c_str());
    if (!image) {
        return LosslessStatus::UnreadableFile;
    }

    out_image = std::move(*image);
    if (out_image.empty()) {
        return LosslessStatus::UnreadableFile;
    }
    return LosslessStatus::Ok;
}

[[nodiscard]] LosslessStatus ParseShaderSpans(std::span<const u8> image,
                                              ResourceSpans& out_resources) {
    const ImageReader reader{image};
    const std::optional<size_t> pe_offset = FindPeHeader(reader);
    if (!pe_offset) {
        return LosslessStatus::NotPortableExecutable;
    }

    const std::optional<size_t> data_directory =
        FindDataDirectory(reader, *pe_offset + 4 + COFF_HEADER_SIZE);
    if (!data_directory) {
        return LosslessStatus::NotPortableExecutable;
    }

    std::vector<Section> sections;
    if (!ReadSections(reader, *pe_offset, sections)) {
        return LosslessStatus::NotPortableExecutable;
    }

    u32 resource_rva{};
    if (!reader.Read(*data_directory + RESOURCE_DATA_DIRECTORY_INDEX * DATA_DIRECTORY_ENTRY_SIZE,
                     resource_rva) ||
        resource_rva == 0) {
        return LosslessStatus::MissingShaders;
    }

    const std::optional<size_t> resource_base = RvaToFileOffset(sections, resource_rva);
    if (!resource_base) {
        return LosslessStatus::NotPortableExecutable;
    }

    out_resources.clear();
    if (!CollectRcData(reader, sections, *resource_base, out_resources)) {
        return LosslessStatus::MissingShaders;
    }

    return HasPerformanceShaders(out_resources) ? LosslessStatus::Ok
                                                : LosslessStatus::MissingShaders;
}

} // Anonymous namespace

// PORT: Eden installs the DLL into a folder it manages and builds the path from that. PCSX2 lets
// the user point at the copy they already own, so the configured path IS the answer.
std::string GetLosslessDllPath() {
    return GSConfig.LsfgDllPath;
}

std::string GetShaderCachePath() {
    return Path::Combine(EmuFolders::Cache, CACHE_FILE_NAME);
}

LosslessStatus ReadShaderResources(const std::string& path, ShaderResources& out_resources) {
    std::vector<u8> image;
    const LosslessStatus read_status = ReadImageFile(path, image);
    if (read_status != LosslessStatus::Ok) {
        return read_status;
    }

    ResourceSpans spans;
    const LosslessStatus parse_status = ParseShaderSpans(image, spans);
    if (parse_status != LosslessStatus::Ok) {
        return parse_status;
    }

    out_resources.clear();
    for (const auto& [id, data] : spans) {
        out_resources.emplace(id, std::vector<u8>{data.begin(), data.end()});
    }
    return LosslessStatus::Ok;
}

LosslessStatus ValidateLosslessDll(const std::string& path) {
    std::vector<u8> image;
    const LosslessStatus read_status = ReadImageFile(path, image);
    if (read_status != LosslessStatus::Ok) {
        return read_status;
    }

    ResourceSpans spans;
    return ParseShaderSpans(image, spans);
}

LosslessStatus GetInstalledLosslessStatus() {
    return ValidateLosslessDll(GetLosslessDllPath());
}

LosslessStatus LoadShaderModules(ShaderModules& out_modules, bool allow_fp16, bool prefer_fp16) {
    const std::string dll_path = GetLosslessDllPath();
    const std::string cache_path = GetShaderCachePath();
    const u32 flags = VariantFlags(allow_fp16, prefer_fp16);

    // ── fast path ────────────────────────────────────────────────────────────────────────────
    // Answer from the cache using only a stat() of the DLL. Eden cannot do this because it keys
    // the cache on a content hash, so it reads and hashes the whole file and walks its PE
    // resource tree BEFORE it is allowed to look at the cache — every launch, hit or miss. On a
    // phone that is several megabytes of IO and a full resource-tree walk to load ~52 SPIR-V
    // blobs we already had on disk.
    //
    // A hit here still has to read the cache file itself, but that is the shaders and nothing
    // else. The slow path below is unchanged and remains the authority whenever this misses.
    u64 stat_size = 0;
    u64 stat_mtime = 0;
    if (StatSource(dll_path, stat_size, stat_mtime) &&
        ReadShaderCache(cache_path, stat_size, /*source_hash=*/0, stat_mtime, /*variant=*/~0u,
                        flags, out_modules)) {
        return LosslessStatus::Ok;
    }

    std::vector<u8> image;
    const LosslessStatus read_status = ReadImageFile(dll_path, image);
    if (read_status != LosslessStatus::Ok) {
        return read_status;
    }

    const u64 source_size = image.size();
    // PORT: Common::CityHash64 -> the GS-wide XXH3 helper. Only used to detect that the source
    // DLL changed, so the exact function does not matter as long as it is stable across runs.
    const u64 source_hash = GSXXH3_64bits(image.data(), image.size());

    ResourceSpans spans;
    const LosslessStatus parse_status = ParseShaderSpans(image, spans);
    if (parse_status != LosslessStatus::Ok) {
        return parse_status;
    }

    const std::optional<ShaderVariant> variant = SelectVariant(spans, allow_fp16, prefer_fp16);
    if (!variant) {
        return LosslessStatus::MissingShaders;
    }

    // Still worth a second look: the fast path misses when the stat failed or the cache was
    // written by an older build, and in both cases the hash may still match what is on disk.
    if (ReadShaderCache(cache_path, source_size, source_hash, stat_mtime,
                        static_cast<u32>(*variant), flags, out_modules)) {
        return LosslessStatus::Ok;
    }

    const LosslessStatus translate_status = TranslateAll(spans, out_modules, *variant);
    if (translate_status != LosslessStatus::Ok) {
        return translate_status;
    }

    const CacheHeader header{
        .magic = CACHE_MAGIC,
        .version = CACHE_VERSION,
        .source_size = source_size,
        .source_hash = source_hash,
        .source_mtime = stat_mtime,
        .module_count = static_cast<u32>(out_modules.size()),
        .variant = static_cast<u32>(*variant),
        .flags = flags,
        .reserved = 0,
    };
    if (!WriteShaderCache(cache_path, header, out_modules)) {
        void(FileSystem::DeleteFilePath(cache_path.c_str()));
        return LosslessStatus::CacheUnusable;
    }

    return LosslessStatus::Ok;
}

LosslessStatus BuildShaderCache() {
    ShaderModules modules;
    return LoadShaderModules(modules, true);
}

// PORT: Eden's RemoveInstalledLosslessDll() also deleted the DLL itself. That is safe there
// because Eden owns the file — it lives in an app-managed folder. Here the path is whatever
// GSConfig.LsfgDllPath says, and nothing validates that it points inside our own storage, so a
// config carried from elsewhere could aim it at the user's actual Lossless Scaling install.
//
// The DLL half is also simply unused: the Android UI removes its copy itself. So only the cache
// half is kept, and it is named for what it does rather than for what Eden called it.
void ClearShaderCache()
{
    const std::string cache_path = GetShaderCachePath();
    if (FileSystem::FileExists(cache_path.c_str()))
        void(FileSystem::DeleteFilePath(cache_path.c_str()));
}

} // namespace VideoCore::FrameGen
