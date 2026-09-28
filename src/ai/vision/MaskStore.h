// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "jobs/Job.h"
#include "project/model/Cutout.h"

namespace genesis::ai::vision {

// What produced one segmentation mask: the source frame it was cut from, and
// how. Mirrors render::CacheKey (src/render/MediaCache.h): a mask is per-frame
// and per-model, so the source, the subject looked for, the model that ran, and
// the frame index together name one mask. Staleness is a comparison, never a
// guess - a mask whose recorded key does not match the one asked for is
// ignored and regenerated.
struct MaskKey
{
    // The media file the frame was decoded from.
    std::filesystem::path source;
    // Its byte size at mask time - the cheap proxy for "the file changed".
    std::uintmax_t source_size = 0;
    // Which subject the model looked for (jobs::Subject).
    jobs::Subject subject = jobs::Subject::Auto;
    // Which model ran, e.g. "rvm" (person matting) or "isnet" (object).
    std::string model_id;
    // The frame index within the source, 0-based.
    std::uint32_t frame = 0;
    // The stroke revision the mask was generated for: empty for an automatic
    // cutout (the model's base mask), else the mask_revision() stem of the
    // cutout's strokes. Two cutouts that differ only in their strokes must
    // name different masks, so the revision rides into the key exactly like
    // the subject and model.
    std::string revision;

    bool operator==(const MaskKey &) const = default;
};

// The mask file for one frame under `mask_root`. Pure naming: no I/O. The name
// is the FNV-1a 64 stem of the source path plus the subject, the model id and
// the frame index, folded as bytes exactly like a reverse cache folds its span
// (src/render/ReverseCache.cpp), so the same frame maps to the same file every
// run and two frames never collide in practice.
std::filesystem::path mask_cache_path(const std::filesystem::path &mask_root, const MaskKey &key);

// What a project cutout is generated and rendered as: the jobs::Subject a mask
// is keyed by and the model that actually ran. This is the single source of
// truth the driver (MaskDriver) and the render path both read, so a mask cut
// for a subject is found by the same subject later and the two can never
// disagree about a model id. The catalogue has eight entries, but only "rvm"
// (person matting) and "isnet" (object segmentation) are segmentation models;
// the `auto` subject has no real model among them, so it records the
// placeholder "auto" until an analysis step decides which model a clip really
// needs - generation and render agree on the placeholder in the meantime.
struct MaskTarget
{
    jobs::Subject subject = jobs::Subject::Auto;
    std::string model_id;
};

MaskTarget mask_target(const genesis::project::Cutout &cutout);

// The stroke revision a cutout's masks are keyed by: empty for an automatic
// cutout or one with no strokes (the model's base mask), else the FNV-1a stem
// of the strokes folded as bytes. Two cutouts that differ in any stroke - its
// tool, size, points or the instant it was painted at - name a different
// revision, so a re-painted stroke regenerates rather than reusing a stale
// mask. Pure naming, no I/O.
std::string mask_revision(const genesis::project::Cutout &cutout);

// Everything the render path needs to find and step a clip's masks. Carried
// beside a clip's cutout (the flatten -> resolve -> engine seam) so the engine
// looks masks up exactly as the driver wrote them, without reopening the store
// or re-deriving the keying.
struct MaskSpec
{
    // The directory the .mask files live under (the caller's mask root).
    std::filesystem::path root;
    // The media file the masks were keyed by - the original source, not a
    // reversed copy or a proxy the engine might substitute.
    std::filesystem::path source;
    // Its byte size at generation time, the staleness proxy folded into each
    // key.
    std::uintmax_t source_size = 0;
    // Which subject the model looked for.
    jobs::Subject subject = jobs::Subject::Auto;
    // Which model ran.
    std::string model_id;
    // The mask frame size, in pixels: masks are cut at the source's native
    // size, so this is the media's dimensions. The engine forces its mask
    // chain to this size so a mask and its picture sample the same
    // normalised coordinate space.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The decimation stride the masks were generated at: masks exist at frames
    // 0, stride, 2*stride, ... 1 means every frame.
    std::uint32_t stride = 1;
    // The last generated source frame (inclusive), so a frame sampled past it
    // clamps to it rather than reading absent. Absent means no clamp: a frame
    // whose mask was never generated reads absent.
    std::optional<std::uint32_t> last_frame;
    // The stroke revision the masks were keyed by (mask_revision()); empty for
    // an automatic cutout. Folds into the key so the render path finds exactly
    // the masks the driver generated for this cutout's strokes.
    std::string revision;
};

// The mask frame a source frame samples: floor(source_frame / stride), clamped
// to the last generated frame when the spec records one. Pure arithmetic, no
// I/O - the documented decimation mapping the engine applies per frame.
std::uint32_t mask_frame_for(const MaskSpec &spec, std::uint32_t source_frame);

// The key naming the mask for one source frame under `spec`, through the
// stride mapping above - the one lookup generation and render share.
MaskKey mask_key_for(const MaskSpec &spec, std::uint32_t source_frame);

// The mask file for one source frame under `spec`. Pure naming; a caller still
// checks freshness/reads the bytes itself.
std::filesystem::path mask_path_for(const MaskSpec &spec, std::uint32_t source_frame);

// A decoded alpha mask: one 8-bit value per pixel, 0 = background (drop),
// 255 = subject (keep), row-major, top row first.
struct MaskData
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> alpha; // width * height bytes
};

// --- Mask file format -------------------------------------------------------
//
// A mask file is a small self-describing binary: a header, then the raw
// grayscale alpha plane. No image codec is involved, so a reader needs no
// dependency and the bytes are unambiguous. Every integer is little-endian.
//
//   offset  size  field
//   0       4     magic "GKMA"
//   4       1     version = 1
//   5       4     width  (u32)
//   9       4     height (u32)
//   13      n     alpha  (n = width * height bytes, row-major)
//
// A decode that sees the wrong magic, the wrong version, or a byte count that
// does not equal 13 + width * height refuses - a truncated or hand-edited mask
// reads absent rather than crashing the reader.

// Writes a mask file (creating parent directories). False when the file cannot
// be written; never a partial write into a reader's path (writes go to a
// temporary sibling and rename into place).
bool write_mask(const std::filesystem::path &path, const MaskData &mask);

// Reads and validates a mask file, or nullopt for a missing, corrupt or
// truncated file.
std::optional<MaskData> read_mask(const std::filesystem::path &path);

// Records the key beside `mask_file` (a ".key" sidecar), capturing the mask
// file's own size so a later truncation reads stale. Only the key is recorded;
// producing the mask bytes is the provider's business. False when the mask
// file is absent or the sidecar cannot be written.
bool mask_record(const std::filesystem::path &mask_file, const MaskKey &key);

// Whether `mask_file` is usable for `key`: the file exists, its sidecar
// records a key equal to `key`, and the file's size still matches the size
// recorded with it. A missing, corrupt, truncated or key-mismatched mask is
// stale - reported as false, never a crash. Mirrors
// render::MediaCache::cache_is_fresh.
bool mask_is_fresh(const std::filesystem::path &mask_file, const MaskKey &key);

} // namespace genesis::ai::vision
