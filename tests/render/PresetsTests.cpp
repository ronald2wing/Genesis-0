// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Clip.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/Catalogue.h"
#include "render/Presets.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

using namespace genesis::project;
namespace render = genesis::render;

bool finite(double value)
{
    return std::isfinite(value);
}

// A project with one video media item and one clip of it, built through the
// real command layer.
struct Fixture
{
    Editor editor;
    std::string clip_id;
};

Fixture fixture()
{
    Fixture f;
    NewMedia item;
    item.path = "/footage/a.mp4";
    item.name = "a.mp4";
    item.duration = Rational{ 10, 1 };
    item.kind = MediaKind::Video;
    item.width = 1920;
    item.height = 1080;
    const auto add_media = f.editor.apply(Command{ AddMedia{ std::move(item) } });
    check(add_media.has_value() && add_media->created_id.has_value(), "adds media");
    const std::string media_id = *add_media->created_id;
    const std::string track_id = f.editor.project().timelines[0].tracks[0].id;
    const auto add_clip =
            f.editor.apply(Command{ AddClip{ media_id, track_id, Rational{ 1, 1 }, false } });
    check(add_clip.has_value() && add_clip->created_id.has_value(), "adds clip");
    f.clip_id = *add_clip->created_id;
    return f;
}

void test_every_animation_preset_has_finite_in_range_keys()
{
    check(!render::animation_presets().empty(), "some animation presets exist");
    for (const render::AnimationPreset &preset : render::animation_presets()) {
        check(!preset.keys.empty(), "preset " + std::string(preset.name) + " has at least one key");
        Clip clip;
        clip.id = "probe";
        clip.keys = preset.keys;
        for (const ClipKey &key : preset.keys) {
            check(finite(key.at) && key.at >= 0.0 && key.at <= 1.0,
                  "preset " + std::string(preset.name) + " key at is finite and in [0,1]");
            check(finite(key.value), "preset " + std::string(preset.name) + " key value is finite");
        }
        for (KeyProperty property : key_property_all) {
            if (!clip.is_keyed(property)) {
                continue;
            }
            check(!clip.track_on(property).is_empty(),
                  "preset " + std::string(preset.name) + " claims non-empty track data on "
                          + std::string(name(property)));
        }
    }
}

void test_applying_a_preset_via_set_clip_key_yields_exactly_those_keys()
{
    const std::optional<render::AnimationPreset> preset = render::animation_preset("fadeInTopLeft");
    check(preset.has_value(), "fadeInTopLeft resolves");

    Fixture f = fixture();
    for (const ClipKey &key : preset->keys) {
        const auto applied = f.editor.apply(
                Command{ SetClipKey{ f.clip_id, key.property, key.at, key.value, key.ease } });
        check(applied.has_value(), "applies one key through SetClipKey");
    }

    const Clip *clip = f.editor.project().active().clip(f.clip_id);
    check(clip != nullptr, "the clip is present");

    // The clip's keys, in the model's property-then-at order, must be exactly
    // the preset's. fadeInTopLeft is: opacity 0->1, x -1->0, y -1->0.
    const std::vector<ClipKey> expected = {
        { KeyProperty::OffsetX, 0.0, -1.0, KeyEase::linear() },
        { KeyProperty::OffsetX, 1.0, 0.0, KeyEase::linear() },
        { KeyProperty::OffsetY, 0.0, -1.0, KeyEase::linear() },
        { KeyProperty::OffsetY, 1.0, 0.0, KeyEase::linear() },
        { KeyProperty::Opacity, 0.0, 0.0, KeyEase::linear() },
        { KeyProperty::Opacity, 1.0, 1.0, KeyEase::linear() },
    };
    check(clip->keys == expected, "SetClipKey yields exactly the preset's keys");
    check(clip->keys.size() == preset->keys.size(), "no keys were dropped or added");
}

void test_camera_motion_is_pure_arithmetic()
{
    // push_pull: a centred zoom, no drift, exact by construction.
    const render::CameraMotion push = render::push_pull(true, 1.5);
    check(push.scale_start == 1.0 && push.scale_end == 1.5, "push_pull zooms in from 1.0 to 1.5");
    check(push.offset_x_start == 0.0 && push.offset_x_end == 0.0 && push.offset_y_start == 0.0
                  && push.offset_y_end == 0.0,
          "push_pull does not drift");
    const render::CameraMotion pull = render::push_pull(false, 1.5);
    check(pull.scale_start == 1.5 && pull.scale_end == 1.0, "push_pull zooms out from 1.5 to 1.0");

    // camera_pan across a 3200x1080 source on a 1920x1080 frame: the crop
    // base is 3200 wide, natural pan room is (3200-1920)/(3200+1920) = 0.25,
    // which already reaches the 0.18 target, so no zoom and a 0.995 margin.
    const render::CameraMotion pan =
            render::camera_pan(render::MotionDirection::LeftToRight, 1920, 1080, 3200, 1080);
    check(pan.scale_start == 1.0 && pan.scale_end == 1.0,
          "pan needs no zoom when the crop already has room");
    check(pan.offset_x_start == 0.25 * 0.995, "pan starts at the natural room with margin");
    check(pan.offset_x_end == -(0.25 * 0.995), "pan ends at the mirrored magnitude");
    check(pan.offset_y_start == 0.0 && pan.offset_y_end == 0.0,
          "a horizontal pan does not drift vertically");

    // ken_burns zooming in, left-to-right, same source: zoom stays at the
    // requested 1.22 (the pan target needs only 0.73 scale), so the drift is
    // bounded by the 0.82 room factor and the 0.24 max pan.
    const render::CameraMotion kb =
            render::ken_burns(true, render::MotionDirection::LeftToRight, 1920, 1080, 3200, 1080);
    check(kb.scale_start == 1.0 && kb.scale_end == 1.22,
          "ken_burns zooms from 1.0 to the requested 1.22");
    check(kb.offset_x_start == 0.25 * 0.82,
          "ken_burns starts at the resting room with the 0.82 factor");
    check(kb.offset_x_end == -0.24, "ken_burns ends bounded by max_pan");
    check(kb.offset_y_start == 0.0 && kb.offset_y_end == 0.0,
          "a horizontal ken_burns does not drift vertically");
}

// The source packs/ directory (the builtin genesis.packs extension's packs),
// found by walking up from the working directory.
std::optional<std::filesystem::path> locate_packs()
{
    std::filesystem::path dir = std::filesystem::current_path();
    while (true) {
        const std::filesystem::path candidate =
                dir / "extensions" / "builtin" / "genesis.packs" / "packs";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) && !ec) {
            return candidate;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            return std::nullopt;
        }
        dir = parent;
    }
}

void test_every_colour_and_grain_look_references_only_installed_packs()
{
    const std::optional<std::filesystem::path> root = locate_packs();
    if (!root) {
        check(false, "packs/ found");
        return;
    }
    const render::Catalogue catalogue = render::Catalogue::from_directories({ *root });

    check(!render::colour_looks().empty(), "some colour looks exist");
    for (const render::ColourLook &look : render::colour_looks()) {
        for (const AppliedFilter &filter : look.effects) {
            check(catalogue.pack(filter.id) != nullptr,
                  "colour look " + std::string(look.name) + " references installed pack "
                          + filter.id);
        }
    }

    check(!render::film_grain_looks().empty(), "some film-grain looks exist");
    for (const render::FilmGrainLook &look : render::film_grain_looks()) {
        check(catalogue.pack(look.effect.id) != nullptr,
              "film-grain look " + std::string(look.name) + " references installed pack "
                      + look.effect.id);
    }
}

void test_unknown_names_return_nullopt_not_a_default()
{
    check(!render::animation_preset("no.such.preset").has_value(),
          "an unknown animation preset is nullopt");
    check(render::animation_preset("fadeIn").has_value(), "a known animation preset is present");

    check(!render::colour_look("no.such.look").has_value(), "an unknown colour look is nullopt");
    check(render::colour_look("warm_up").has_value(), "a known colour look is present");

    check(!render::film_grain_look("no.such.grain").has_value(),
          "an unknown film-grain look is nullopt");
    check(render::film_grain_look("super_8").has_value(), "a known film-grain look is present");
}

} // namespace

int main()
{
    test_every_animation_preset_has_finite_in_range_keys();
    test_applying_a_preset_via_set_clip_key_yields_exactly_those_keys();
    test_camera_motion_is_pure_arithmetic();
    test_every_colour_and_grain_look_references_only_installed_packs();
    test_unknown_names_return_nullopt_not_a_default();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
