// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/vision/MaskStore.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "render/MediaCache.h"

namespace genesis::ai::vision {

namespace {

// The FNV-1a stem for one mask: the source path, the subject name, the model
// id, the stroke revision and the frame index, folded as bytes and hashed with
// the same pinned FNV-1a the render caches use (render::MediaCache::cache_stem).
// The frame rides in as its exact 32-bit little-endian pattern, so two frames
// of one source hash apart while the same frame maps to the same file every
// run. The revision (empty for an automatic cutout) folds between the model id
// and the frame, so a re-painted stroke renames the mask without disturbing the
// rest of the key.
std::string mask_stem(const MaskKey &key)
{
    const std::string source = key.source.string();
    const std::string_view subject = jobs::subject_name(key.subject);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(source.size() + subject.size() + key.model_id.size() + key.revision.size() + 4);
    bytes.insert(bytes.end(), source.begin(), source.end());
    bytes.insert(bytes.end(), subject.begin(), subject.end());
    bytes.insert(bytes.end(), key.model_id.begin(), key.model_id.end());
    bytes.insert(bytes.end(), key.revision.begin(), key.revision.end());
    for (std::size_t i = 0; i < sizeof(key.frame); ++i) {
        bytes.push_back(static_cast<std::uint8_t>((key.frame >> (8 * i)) & 0xff));
    }
    return render::cache_stem(bytes);
}

// The sidecar path: the mask file's path with ".key" appended, so the mask
// file stays a plain blob a reader can decode without knowing the key format.
std::filesystem::path sidecar_path(const std::filesystem::path &mask_file)
{
    return std::filesystem::path(mask_file.string() + ".key");
}

// ---- On-disk key format ----------------------------------------------------
//
// A small binary sidecar, not an embedded header: the mask file's byte layout
// is frozen above, so the key cannot live inside it. Every integer is
// little-endian. The shape mirrors render::MediaCache's ".key" sidecar, but
// the magic and fields differ so a mask key is never confused with a media
// cache key. Version 2 appended the stroke revision so a re-painted custom
// cutout regenerates instead of reusing a stale mask; a version-1 sidecar
// reads stale and is rewritten.
//
//   offset  size  field
//   0       4     magic "GMK1"
//   4       1     version = 2
//   5       8     source_size (u64)
//   13      8     mask_size   (u64) - the data file's size when recorded
//   21      4     frame       (u32)
//   25      1     subject     (u8)  - jobs::Subject as 0/1/2
//   26      4     source_len  (u32)
//   30      n1    source      (UTF-8 path bytes)
//   30+n1   4     model_len   (u32)
//   34+n1   n2    model       (UTF-8 bytes)
//   34+n1+n2  4   revision_len (u32)
//   38+n1+n2  n3  revision    (UTF-8 bytes)
//
// A decode that sees the wrong magic, the wrong version, an out-of-range
// subject, or a byte count that does not equal 38 + source_len + model_len +
// revision_len refuses - so a truncated or hand-edited key reads stale rather
// than crashing.

constexpr std::array<std::uint8_t, 4> kKeyMagic = { 'G', 'M', 'K', '1' };
constexpr std::uint8_t kKeyVersion = 2;

constexpr std::array<std::uint8_t, 4> kMaskMagic = { 'G', 'K', 'M', 'A' };
constexpr std::uint8_t kMaskVersion = 1;

template <typename T>
void put_le(std::vector<std::uint8_t> &out, T value)
{
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
    }
}

template <typename T>
T read_le(std::span<const std::uint8_t> bytes, std::size_t at)
{
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(bytes[at + i]) << (8 * i);
    }
    return value;
}

std::uint8_t subject_to_u8(jobs::Subject subject)
{
    switch (subject) {
    case jobs::Subject::Auto:
        return 0;
    case jobs::Subject::Person:
        return 1;
    case jobs::Subject::Object:
        return 2;
    }
    return 0;
}

bool subject_from_u8(std::uint8_t value, jobs::Subject &out)
{
    switch (value) {
    case 0:
        out = jobs::Subject::Auto;
        return true;
    case 1:
        out = jobs::Subject::Person;
        return true;
    case 2:
        out = jobs::Subject::Object;
        return true;
    }
    return false;
}

// The stroke's tool folded as one byte, so the revision distinguishes a smart
// brush from a smart eraser from the plain discs.
std::uint8_t brush_tool_u8(genesis::project::BrushTool tool)
{
    switch (tool) {
    case genesis::project::BrushTool::SmartBrush:
        return 0;
    case genesis::project::BrushTool::Brush:
        return 1;
    case genesis::project::BrushTool::SmartEraser:
        return 2;
    case genesis::project::BrushTool::Eraser:
        return 3;
    }
    return 0;
}

// Appends a double's exact bit pattern little-endian. A double cannot be
// shifted like an integer, so its bits are copied into a uint64 first; the
// revision is a digest, so only determinism matters, not the byte order's
// meaning.
void put_f64(std::vector<std::uint8_t> &out, double value)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    for (std::size_t i = 0; i < sizeof(bits); ++i) {
        out.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xff));
    }
}

std::vector<std::uint8_t> encode_key(const MaskKey &key, std::uintmax_t mask_size)
{
    const std::string source = key.source.string();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(38 + source.size() + key.model_id.size() + key.revision.size());
    bytes.insert(bytes.end(), kKeyMagic.begin(), kKeyMagic.end());
    put_le(bytes, kKeyVersion);
    put_le(bytes, static_cast<std::uint64_t>(key.source_size));
    put_le(bytes, static_cast<std::uint64_t>(mask_size));
    put_le(bytes, key.frame);
    bytes.push_back(subject_to_u8(key.subject));
    put_le(bytes, static_cast<std::uint32_t>(source.size()));
    bytes.insert(bytes.end(), source.begin(), source.end());
    put_le(bytes, static_cast<std::uint32_t>(key.model_id.size()));
    bytes.insert(bytes.end(), key.model_id.begin(), key.model_id.end());
    put_le(bytes, static_cast<std::uint32_t>(key.revision.size()));
    bytes.insert(bytes.end(), key.revision.begin(), key.revision.end());
    return bytes;
}

// The decoded sidecar: the key it recorded plus the data file's size at the
// time, which is how a later truncation is detected.
struct RecordedKey
{
    MaskKey key;
    std::uintmax_t mask_size = 0;
};

std::optional<RecordedKey> decode_key(std::span<const std::uint8_t> bytes)
{
    // The fixed prefix before the first variable-length field (magic + version
    // + source_size + mask_size + frame + subject + source_len) is 30 bytes.
    // Each variable field is a u32 length followed by its bytes; the source
    // sits at 30, the model length after it, and the revision length after the
    // model, so the fixed bytes independent of all three are 38.
    constexpr std::size_t kPrefix = 30;
    constexpr std::size_t kFixed = 38;
    if (bytes.size() < kPrefix) {
        return std::nullopt;
    }
    if (!std::equal(kKeyMagic.begin(), kKeyMagic.end(), bytes.begin()) || bytes[4] != kKeyVersion) {
        return std::nullopt;
    }
    const std::uint32_t source_len = read_le<std::uint32_t>(bytes, 26);
    const std::size_t n1 = static_cast<std::size_t>(source_len);
    const std::size_t model_len_at = kPrefix + n1;
    if (bytes.size() < model_len_at + 4) {
        return std::nullopt;
    }
    const std::uint32_t model_len = read_le<std::uint32_t>(bytes, model_len_at);
    const std::size_t n2 = static_cast<std::size_t>(model_len);
    const std::size_t revision_len_at = model_len_at + 4 + n2;
    if (bytes.size() < revision_len_at + 4) {
        return std::nullopt;
    }
    const std::uint32_t revision_len = read_le<std::uint32_t>(bytes, revision_len_at);
    const std::size_t n3 = static_cast<std::size_t>(revision_len);
    if (bytes.size() != kFixed + n1 + n2 + n3) {
        return std::nullopt;
    }

    RecordedKey out;
    out.key.source_size = read_le<std::uint64_t>(bytes, 5);
    out.mask_size = read_le<std::uint64_t>(bytes, 13);
    out.key.frame = read_le<std::uint32_t>(bytes, 21);
    if (!subject_from_u8(bytes[25], out.key.subject)) {
        return std::nullopt;
    }
    out.key.source = std::filesystem::path(
            std::string(bytes.begin() + static_cast<std::ptrdiff_t>(kPrefix),
                        bytes.begin() + static_cast<std::ptrdiff_t>(kPrefix + n1)));
    out.key.model_id =
            std::string(bytes.begin() + static_cast<std::ptrdiff_t>(model_len_at + 4),
                        bytes.begin() + static_cast<std::ptrdiff_t>(model_len_at + 4 + n2));
    out.key.revision =
            std::string(bytes.begin() + static_cast<std::ptrdiff_t>(revision_len_at + 4),
                        bytes.begin() + static_cast<std::ptrdiff_t>(revision_len_at + 4 + n3));
    return out;
}

bool read_file(const std::filesystem::path &path, std::vector<std::uint8_t> &bytes)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (size == 0) {
        return true;
    }
    in.read(reinterpret_cast<char *>(bytes.data()), size);
    return in.good() || in.eof();
}

} // namespace

std::filesystem::path mask_cache_path(const std::filesystem::path &mask_root, const MaskKey &key)
{
    return mask_root / (mask_stem(key) + ".mask");
}

MaskTarget mask_target(const genesis::project::Cutout &cutout)
{
    switch (cutout.subject) {
    case genesis::project::Subject::Person:
        return { jobs::Subject::Person, "rvm" };
    case genesis::project::Subject::Object:
        return { jobs::Subject::Object, "isnet" };
    case genesis::project::Subject::Auto:
        return { jobs::Subject::Auto, "auto" };
    }
    return { jobs::Subject::Auto, "auto" };
}

std::string mask_revision(const genesis::project::Cutout &cutout)
{
    // An automatic cutout (or a custom one with nothing painted) keys the
    // model's base mask, so it carries no revision. Every stroke of a custom
    // cutout - its tool, size, points and the instant it was painted at - is
    // folded so a re-painted stroke renames the mask.
    if (cutout.mode != genesis::project::CutoutMode::Custom || cutout.strokes.empty()) {
        return { };
    }
    std::vector<std::uint8_t> bytes;
    for (const genesis::project::Stroke &stroke : cutout.strokes) {
        bytes.push_back(brush_tool_u8(stroke.tool));
        put_f64(bytes, stroke.size);
        bytes.push_back(stroke.at.has_value() ? 1 : 0);
        if (stroke.at) {
            put_le(bytes, stroke.at->numerator());
            put_le(bytes, stroke.at->denominator());
        }
        put_le(bytes, static_cast<std::uint32_t>(stroke.points.size()));
        for (const std::array<double, 2> &point : stroke.points) {
            put_f64(bytes, point[0]);
            put_f64(bytes, point[1]);
        }
    }
    return render::cache_stem(bytes);
}

std::uint32_t mask_frame_for(const MaskSpec &spec, std::uint32_t source_frame)
{
    // A zero stride would divide by zero; the spec's default is 1 and callers
    // never set it lower, but guard anyway so a malformed spec cannot crash.
    const std::uint32_t stride = spec.stride == 0 ? 1 : spec.stride;
    const std::uint32_t sampled = source_frame / stride;
    if (spec.last_frame && sampled > *spec.last_frame) {
        return *spec.last_frame;
    }
    return sampled;
}

MaskKey mask_key_for(const MaskSpec &spec, std::uint32_t source_frame)
{
    return MaskKey{ spec.source,
                    spec.source_size,
                    spec.subject,
                    spec.model_id,
                    mask_frame_for(spec, source_frame),
                    spec.revision };
}

std::filesystem::path mask_path_for(const MaskSpec &spec, std::uint32_t source_frame)
{
    return mask_cache_path(spec.root, mask_key_for(spec, source_frame));
}

bool write_mask(const std::filesystem::path &path, const MaskData &mask)
{
    if (mask.width == 0 || mask.height == 0
        || mask.alpha.size() != static_cast<std::size_t>(mask.width) * mask.height) {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    // Stage beside the destination, then rename, so a reader never observes a
    // half-written mask - the same atomic-write shape the model store uses.
    const std::filesystem::path staging = std::filesystem::path(path.string() + ".tmp");
    std::ofstream out(staging, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    const auto write_le = [&out](auto value) {
        for (std::size_t i = 0; i < sizeof(value); ++i) {
            const std::uint8_t byte = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
            out.write(reinterpret_cast<const char *>(&byte), 1);
        }
    };
    out.write(reinterpret_cast<const char *>(kMaskMagic.data()),
              static_cast<std::streamsize>(kMaskMagic.size()));
    write_le(kMaskVersion);
    write_le(mask.width);
    write_le(mask.height);
    out.write(reinterpret_cast<const char *>(mask.alpha.data()),
              static_cast<std::streamsize>(mask.alpha.size()));
    out.close();
    if (!out) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return false;
    }

    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(staging, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return false;
    }
    return true;
}

std::optional<MaskData> read_mask(const std::filesystem::path &path)
{
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes)) {
        return std::nullopt;
    }
    constexpr std::size_t kHeader = 13;
    if (bytes.size() < kHeader || !std::equal(kMaskMagic.begin(), kMaskMagic.end(), bytes.begin())
        || bytes[4] != kMaskVersion) {
        return std::nullopt;
    }
    const std::uint32_t width = read_le<std::uint32_t>(bytes, 5);
    const std::uint32_t height = read_le<std::uint32_t>(bytes, 9);
    const std::size_t n = static_cast<std::size_t>(width) * height;
    if (bytes.size() != kHeader + n) {
        return std::nullopt;
    }
    MaskData mask;
    mask.width = width;
    mask.height = height;
    mask.alpha.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kHeader), bytes.end());
    return mask;
}

bool mask_record(const std::filesystem::path &mask_file, const MaskKey &key)
{
    std::error_code ec;
    if (!std::filesystem::exists(mask_file, ec)) {
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(mask_file, ec);
    if (ec) {
        return false;
    }

    const std::vector<std::uint8_t> bytes = encode_key(key, size);
    std::ofstream out(sidecar_path(mask_file), std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool mask_is_fresh(const std::filesystem::path &mask_file, const MaskKey &key)
{
    std::error_code ec;
    if (!std::filesystem::exists(mask_file, ec)) {
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(mask_file, ec);
    if (ec) {
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!read_file(sidecar_path(mask_file), bytes)) {
        return false;
    }
    const std::optional<RecordedKey> recorded = decode_key(bytes);
    if (!recorded) {
        return false;
    }
    return recorded->key == key && recorded->mask_size == size;
}

} // namespace genesis::ai::vision
