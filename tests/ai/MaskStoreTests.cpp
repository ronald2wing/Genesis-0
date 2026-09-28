// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "ai/vision/MaskStore.h"
#include "jobs/Job.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace vision = genesis::ai::vision;
namespace jobs = genesis::jobs;

// A per-run sandbox under the system temp dir, emptied first so a crashed run
// cannot leak a fresh-looking mask into the assertions.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-mask-store-tests";
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
        return p;
    }();
    return dir;
}

vision::MaskKey key(std::uint32_t frame = 7)
{
    vision::MaskKey k;
    k.source = "/media/clips/interview.mp4";
    k.source_size = 123456789;
    k.subject = jobs::Subject::Person;
    k.model_id = "rvm";
    k.frame = frame;
    return k;
}

vision::MaskData mask(std::uint32_t w, std::uint32_t h)
{
    vision::MaskData m;
    m.width = w;
    m.height = h;
    m.alpha.resize(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < m.alpha.size(); ++i) {
        m.alpha[i] = static_cast<std::uint8_t>((i * 13 + 3) & 0xff);
    }
    return m;
}

// A MaskSpec the render path would carry: keyed by one source at a known
// size, with a configurable stride and (optional) last generated frame.
vision::MaskSpec spec(std::uint32_t stride = 1,
                      std::optional<std::uint32_t> last_frame = std::nullopt)
{
    vision::MaskSpec s;
    s.root = sandbox() / "spec";
    s.source = "/media/clips/interview.mp4";
    s.source_size = 123456789;
    s.subject = jobs::Subject::Person;
    s.model_id = "rvm";
    s.width = 96;
    s.height = 64;
    s.stride = stride;
    s.last_frame = last_frame;
    return s;
}

void test_cache_path_deterministic_and_distinct()
{
    const std::filesystem::path root = sandbox() / "masks";
    const vision::MaskKey a = key();
    check(vision::mask_cache_path(root, a) == vision::mask_cache_path(root, a),
          "the same key maps to the same path");

    const std::filesystem::path path = vision::mask_cache_path(root, a);
    check(path.parent_path() == root, "the path lives under the mask root");
    check(path.extension() == ".mask", "the path is a .mask file");

    vision::MaskKey other_frame = a;
    other_frame.frame = 8;
    check(vision::mask_cache_path(root, a) != vision::mask_cache_path(root, other_frame),
          "two frames map apart");

    vision::MaskKey other_subject = a;
    other_subject.subject = jobs::Subject::Object;
    check(vision::mask_cache_path(root, a) != vision::mask_cache_path(root, other_subject),
          "two subjects map apart");

    vision::MaskKey other_model = a;
    other_model.model_id = "isnet";
    check(vision::mask_cache_path(root, a) != vision::mask_cache_path(root, other_model),
          "two models map apart");
}

void test_write_read_roundtrip()
{
    const std::filesystem::path path = vision::mask_cache_path(sandbox() / "roundtrip", key());
    const vision::MaskData written = mask(12, 8);
    check(vision::write_mask(path, written), "a well-formed mask writes");

    const std::optional<vision::MaskData> read = vision::read_mask(path);
    check(read.has_value(), "the written mask reads back");
    if (!read) {
        return;
    }
    check(read->width == written.width && read->height == written.height,
          "the dimensions survive the round trip");
    check(read->alpha == written.alpha, "the alpha plane survives byte-for-byte");
}

void test_write_refuses_mismatched_alpha()
{
    const std::filesystem::path path = vision::mask_cache_path(sandbox() / "mismatch", key());
    vision::MaskData bad;
    bad.width = 4;
    bad.height = 4;
    bad.alpha.resize(3); // not width * height
    check(!vision::write_mask(path, bad), "a mask whose alpha is not width*height is refused");
    check(!std::filesystem::exists(path), "and no file lands");
}

void test_read_refuses_corrupt()
{
    const std::filesystem::path path = vision::mask_cache_path(sandbox() / "corrupt", key());
    const std::filesystem::path missing = vision::mask_cache_path(sandbox() / "corrupt", key(99));
    check(!vision::read_mask(missing).has_value(), "a missing mask reads absent");
    check(vision::write_mask(path, mask(4, 4)), "a valid mask writes");

    // Truncate mid-plane: the byte count no longer matches the header.
    std::filesystem::resize_file(path, 13 + 8);
    check(!vision::read_mask(path).has_value(), "a truncated mask reads absent");
}

void test_record_and_freshness()
{
    const std::filesystem::path path = vision::mask_cache_path(sandbox() / "fresh", key());
    const vision::MaskKey k = key();
    check(!vision::mask_is_fresh(path, k), "a mask that was never written is stale");
    check(!vision::mask_record(path, k), "recording a key for an absent mask fails");

    check(vision::write_mask(path, mask(6, 6)), "the mask writes");
    check(!vision::mask_is_fresh(path, k), "a mask with no recorded key is stale");
    check(vision::mask_record(path, k), "recording the key succeeds");
    check(vision::mask_is_fresh(path, k), "the recorded mask reads fresh for its key");

    // A different key (frame, subject, model, source size) is stale.
    vision::MaskKey other_frame = k;
    other_frame.frame = 9;
    check(!vision::mask_is_fresh(path, other_frame), "a different frame is stale");

    vision::MaskKey other_size = k;
    other_size.source_size += 1;
    check(!vision::mask_is_fresh(path, other_size), "a changed source size is stale");

    // Truncate the data file: its size no longer matches the recorded size.
    std::filesystem::resize_file(path, 13 + 4);
    check(!vision::mask_is_fresh(path, k), "a truncated mask is stale");

    // Remove the sidecar: the key is gone, so the mask is stale.
    check(vision::write_mask(path, mask(6, 6)), "the mask rewrites");
    check(vision::mask_record(path, k), "the key re-records");
    std::filesystem::remove(std::filesystem::path(path.string() + ".key"));
    check(!vision::mask_is_fresh(path, k), "a mask whose sidecar is gone is stale");
}

// The mask target a cutout names: the subject's model and id. Generation and
// render both read this, so the subject a mask was cut for is the subject it
// is found by later.
void test_mask_target_maps_subjects()
{
    genesis::project::Cutout person = genesis::project::Cutout::automatic();
    person.subject = genesis::project::Subject::Person;
    const vision::MaskTarget pt = vision::mask_target(person);
    check(pt.subject == jobs::Subject::Person, "a person cutout keys the person model");
    check(pt.model_id == "rvm", "the person model id is rvm");

    genesis::project::Cutout object = genesis::project::Cutout::automatic();
    object.subject = genesis::project::Subject::Object;
    const vision::MaskTarget ot = vision::mask_target(object);
    check(ot.subject == jobs::Subject::Object, "an object cutout keys the object model");
    check(ot.model_id == "isnet", "the object model id is isnet");

    genesis::project::Cutout automatic = genesis::project::Cutout::automatic();
    const vision::MaskTarget at = vision::mask_target(automatic);
    check(at.subject == jobs::Subject::Auto, "an auto cutout keys the auto subject");
    check(at.model_id == "auto", "the auto subject records the auto placeholder");
}

// The decimation mapping: floor(frame / stride), clamped to the last generated
// frame when the spec records one. A zero stride degrades to one rather than
// dividing by zero.
void test_mask_frame_for_decimates_and_clamps()
{
    check(vision::mask_frame_for(spec(1), 7) == 7, "stride 1 samples the frame itself");
    check(vision::mask_frame_for(spec(2), 7) == 3, "stride 2 floors the frame to its mask");
    check(vision::mask_frame_for(spec(2), 8) == 4, "stride 2 hits the next mask exactly");
    check(vision::mask_frame_for(spec(0), 7) == 7, "a zero stride degrades to one");
    check(vision::mask_frame_for(spec(3, 10), 100) == 10, "a frame past the last clamps to it");
    check(vision::mask_frame_for(spec(1, 5), 3) == 3, "a frame under the last is untouched");
    check(vision::mask_frame_for(spec(2, 5), 5) == 2,
          "a frame at the last is not clamped below it");
}

// The key and path a source frame names are deterministic: the same frame
// always maps to the same key and path, two decimated frames map apart, and
// frames clamped to the same last mask share one path.
void test_mask_key_and_path_are_deterministic()
{
    const vision::MaskSpec s = spec(2, 20);

    const vision::MaskKey a = vision::mask_key_for(s, 9);
    const vision::MaskKey b = vision::mask_key_for(s, 9);
    check(a == b, "the same source frame keys the same");
    check(a.frame == 4, "the key's frame is the decimated mask frame");
    check(a.source == s.source && a.source_size == s.source_size,
          "the key carries the source facts");
    check(a.subject == s.subject && a.model_id == s.model_id,
          "the key carries the subject and model");

    check(vision::mask_path_for(s, 9) == vision::mask_path_for(s, 9),
          "the same frame maps to the same path");
    check(vision::mask_path_for(s, 9) == vision::mask_cache_path(s.root, a),
          "the path is the cache path for the key");
    check(vision::mask_path_for(s, 9) != vision::mask_path_for(s, 10),
          "two decimated frames map apart");
    check(vision::mask_path_for(s, 100) == vision::mask_path_for(s, 40),
          "frames clamped to the same last mask map together");
}

// The stroke revision a cutout names: empty for an automatic cutout (the
// model's base mask), non-empty for a custom cutout with strokes, and different
// whenever any stroke changes - a re-painted stroke renames the mask rather
// than reusing a stale one.
void test_mask_revision_names_strokes()
{
    genesis::project::Cutout automatic = genesis::project::Cutout::automatic();
    check(vision::mask_revision(automatic).empty(), "an automatic cutout carries no revision");

    genesis::project::Cutout custom = genesis::project::Cutout::automatic();
    custom.mode = genesis::project::CutoutMode::Custom;
    check(vision::mask_revision(custom).empty(),
          "a custom cutout with no strokes carries no revision");

    genesis::project::Stroke stroke;
    stroke.tool = genesis::project::BrushTool::SmartBrush;
    stroke.size = 0.05;
    stroke.points = { { { 0.1, 0.2 }, { 0.3, 0.4 } } };
    stroke.at = genesis::core::Rational{ 1, 2 };
    custom.strokes.push_back(stroke);
    const std::string revision = vision::mask_revision(custom);
    check(!revision.empty(), "a stroked cutout carries a revision");

    // The same cutout names the same revision; a different stroke does not.
    check(vision::mask_revision(custom) == revision, "the same strokes name the same revision");

    genesis::project::Cutout re_painted = custom;
    re_painted.strokes[0].points[0][0] = 0.11;
    check(vision::mask_revision(re_painted) != revision, "a moved point renames the revision");

    genesis::project::Cutout erased = custom;
    erased.strokes[0].tool = genesis::project::BrushTool::SmartEraser;
    check(vision::mask_revision(erased) != revision, "a changed tool renames the revision");
}

// A revision-bearing key maps apart from its base and round-trips through the
// sidecar, so the render path finds exactly the masks the driver wrote for a
// custom cutout's strokes.
void test_revision_keys_and_roundtrip()
{
    const vision::MaskKey base = key();
    vision::MaskKey revised = base;
    revised.revision = "abc123";

    check(vision::mask_cache_path(sandbox() / "rev", base)
                  != vision::mask_cache_path(sandbox() / "rev", revised),
          "two revisions map apart");

    const std::filesystem::path path =
            vision::mask_cache_path(sandbox() / "rev-roundtrip", revised);
    check(vision::write_mask(path, mask(4, 4)), "the mask writes");
    check(!vision::mask_is_fresh(path, base), "the base key is stale for a revised mask");
    check(vision::mask_record(path, revised), "the revised key records");
    check(vision::mask_is_fresh(path, revised), "the revised mask reads fresh for its own key");

    // A MaskSpec carrying the revision keys a source frame through it.
    vision::MaskSpec s = spec(1);
    s.revision = "abc123";
    const vision::MaskKey keyed = vision::mask_key_for(s, 7);
    check(keyed.revision == "abc123", "the spec's revision rides into the key");
    check(vision::mask_path_for(s, 7) == vision::mask_cache_path(s.root, keyed),
          "the path is the revised cache path");
}

} // namespace

int main()
{
    test_cache_path_deterministic_and_distinct();
    test_write_read_roundtrip();
    test_write_refuses_mismatched_alpha();
    test_read_refuses_corrupt();
    test_record_and_freshness();
    test_mask_target_maps_subjects();
    test_mask_frame_for_decimates_and_clamps();
    test_mask_key_and_path_are_deterministic();
    test_mask_revision_names_strokes();
    test_revision_keys_and_roundtrip();

    std::filesystem::remove_all(sandbox());

    return genesis::test::summary();
}
