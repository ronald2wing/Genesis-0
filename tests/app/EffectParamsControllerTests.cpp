// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The effect-parameter controller's suite: drives the projection and the
// setter headlessly, so the pack-schema projection (including the enum labels
// that name each LUT look), the static-edit-as-key-at-start mapping, the
// command routing and the single-undo-step semantics are exercised without a
// GUI. The fixture is built through the real command layer (AddMedia/AddClip/
// UpdateClip), so it cannot drift from the model; the controller is driven
// through its Q_INVOKABLE surface exactly as QML calls it. The projection
// resolves the real shipped `genesis.lut-grade` pack through the builtin
// catalogue, so the suite runs with the source dir as cwd (packs/).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include <QCoreApplication>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "app/controllers/EffectParamsController.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/Catalogue.h"

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

// A project with one video media item and one clip of it, wearing a single
// `genesis.lut-grade` link on its picture chain.
struct Fixture
{
    Editor editor;
    std::string media_id;
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
    const auto add_media = f.editor.apply(Command{ AddMedia{ std::move(item) } });
    check(add_media.has_value(), "adds media");
    check(add_media->created_id.has_value(), "mints a media id");
    f.media_id = *add_media->created_id;
    const std::string track_id = f.editor.project().timelines[0].tracks[0].id;
    const auto add_clip =
            f.editor.apply(Command{ AddClip{ f.media_id, track_id, Rational{ 1, 1 }, false } });
    check(add_clip.has_value(), "adds clip");
    check(add_clip->created_id.has_value(), "mints a clip id");
    f.clip_id = *add_clip->created_id;

    ClipPatch patch;
    std::vector<AppliedFilter> chain;
    chain.push_back(AppliedFilter::create("genesis.lut-grade"));
    patch.video_effects = std::move(chain);
    UpdateClip update;
    update.clip_id = f.clip_id;
    update.patch = std::move(patch);
    check(f.editor.apply(Command{ std::move(update) }).has_value(), "applies the lut-grade look");
    return f;
}

const Clip *clip_at(const Editor &editor, const std::string &clip_id)
{
    return editor.project().active().clip(clip_id);
}

// A synthetic pack declaring one of each compound parameter (color, point,
// wheel, curve) - types no shipped pack declares - so the projection getters
// and compound setters are exercised against the real loader. The ranges are
// chosen to satisfy validate (default within min..max).
constexpr const char *kCompoundManifest = R"(
format = 2

[effect]
id = "genesis.compound"
name = "Compound"
kind = "effect"

[[param]]
key = "tint"
label = "Tint"
type = "color"
min = 0
max = 4294967295
default = 0

[[param]]
key = "position"
label = "Position"
type = "point"
min = 0
max = 1
default = 0.5

[[param]]
key = "hue"
label = "Hue"
type = "wheel"
min = 0
max = 1
default = 0.5

[[param]]
key = "ramp"
label = "Ramp"
type = "curve"
min = 0
max = 1
default = 0
)";

// Loads the synthetic pack from a temp directory and returns a catalogue over
// it. The Pack is copied into the catalogue, so the directory is removed
// before the call returns.
genesis::render::Catalogue compound_catalogue()
{
    const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "genesis-compound-root";
    std::error_code ec;
    std::filesystem::create_directories(root / "genesis.compound", ec);
    {
        std::ofstream manifest(root / "genesis.compound" / "effect.toml");
        manifest << kCompoundManifest;
    }
    genesis::render::Catalogue catalogue = genesis::render::Catalogue::from_directories({ root });
    std::filesystem::remove_all(root, ec);
    return catalogue;
}

// An editor with one clip wearing one link of `pack_id`, built through the
// real command layer, and the clip's id. Mirrors `fixture()` but takes the
// pack id so the compound suite reuses the same shape.
Editor compound_editor(std::string &clip_id, const char *pack_id)
{
    Editor editor;
    NewMedia item;
    item.path = "/footage/a.mp4";
    item.name = "a.mp4";
    item.duration = Rational{ 10, 1 };
    item.kind = MediaKind::Video;
    const auto add_media = editor.apply(Command{ AddMedia{ std::move(item) } });
    check(add_media.has_value(), "adds media");
    const std::string media_id = *add_media->created_id;
    const std::string track_id = editor.project().timelines[0].tracks[0].id;
    const auto add_clip =
            editor.apply(Command{ AddClip{ media_id, track_id, Rational{ 1, 1 }, false } });
    check(add_clip.has_value(), "adds clip");
    clip_id = *add_clip->created_id;

    ClipPatch patch;
    std::vector<AppliedFilter> chain;
    chain.push_back(AppliedFilter::create(pack_id));
    patch.video_effects = std::move(chain);
    UpdateClip update;
    update.clip_id = clip_id;
    update.patch = std::move(patch);
    check(editor.apply(Command{ std::move(update) }).has_value(), "applies the look");
    return editor;
}

// A double equality within `eps`, for the quantised color channels.
bool near(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) <= eps;
}

// The single link of the clip, for the compound assertions.
const AppliedFilter &link_at(const Editor &editor, const std::string &clip_id)
{
    return clip_at(editor, clip_id)->video_effects[0];
}

void test_projection_reports_schema_and_enum_labels()
{
    Fixture f = fixture();
    EffectParamsController params(&f.editor);
    params.setClipId(QString::fromStdString(f.clip_id));

    const QVariantList effects = params.effects();
    check(effects.size() == 1, "one applied effect is projected");
    const QVariantMap link = effects[0].toMap();
    check(link.value(QStringLiteral("entry")).toInt() == 0, "the link reports its entry index");
    check(link.value(QStringLiteral("id")).toString() == QStringLiteral("genesis.lut-grade"),
          "the link reports its pack id");
    check(link.value(QStringLiteral("name")).toString() == QStringLiteral("LUT Grade"),
          "the link reports the pack's display name");
    check(link.value(QStringLiteral("enabled")).toBool(), "the link is enabled");

    const QVariantList parameters = link.value(QStringLiteral("parameters")).toList();
    check(parameters.size() == 2, "the lut-grade pack declares two parameters");

    // The `lut` enum, first in manifest order.
    const QVariantMap lut = parameters[0].toMap();
    check(lut.value(QStringLiteral("key")).toString() == QStringLiteral("lut"),
          "the first parameter is `lut`");
    check(lut.value(QStringLiteral("type")).toString() == QStringLiteral("enum"),
          "the lut parameter is an enum");
    const QVariantList labels = lut.value(QStringLiteral("labels")).toList();
    check(labels.size() == 8, "the lut enum carries eight labels");
    check(labels[0].toString() == QStringLiteral("Bleach Bypass"),
          "the first label names the first look, not its index");
    check(labels[4].toString() == QStringLiteral("Teal Orange"), "a mid label names its look");
    const QVariantList values = lut.value(QStringLiteral("values")).toList();
    check(values.size() == 8 && values[0].toDouble() == 0.0 && values[7].toDouble() == 7.0,
          "the enum values ride beside the labels");
    check(lut.value(QStringLiteral("value")).toDouble() == 0.0,
          "an untouched enum reports its default index");

    // The `strength` float, second in manifest order.
    const QVariantMap strength = parameters[1].toMap();
    check(strength.value(QStringLiteral("key")).toString() == QStringLiteral("strength"),
          "the second parameter is `strength`");
    check(strength.value(QStringLiteral("type")).toString() == QStringLiteral("float"),
          "the strength parameter is a float");
    check(strength.value(QStringLiteral("value")).toDouble() == 100.0,
          "an untouched float reports its default");
}

void test_setter_round_trips_as_a_key_at_the_clip_start()
{
    Fixture f = fixture();
    EffectParamsController params(&f.editor);
    params.setClipId(QString::fromStdString(f.clip_id));

    check(params.setParameter(0, QStringLiteral("lut"), 3.0), "setting a parameter applies");

    const Clip *clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->video_effects.size() == 1, "the clip still wears one effect");
    const AppliedFilter &link = clip->video_effects[0];
    check(link.keys.size() == 1 && link.keys.count("lut") == 1, "the edit lands as a key on `lut`");
    const std::vector<ParamKey> &run = link.keys.at("lut");
    check(run.size() == 1 && run[0].at == 0.0 && run[0].value == 3.0,
          "the static edit is a single key at the clip start");

    // The projection re-reads the value from the model.
    const QVariantMap lut =
            params.effects()[0].toMap().value(QStringLiteral("parameters")).toList()[0].toMap();
    check(lut.value(QStringLiteral("value")).toDouble() == 3.0,
          "the projection reports the new value");
    check(params.canUndo(), "undo is available after setting a parameter");
}

void test_setter_undo_restores_the_default()
{
    Fixture f = fixture();
    EffectParamsController params(&f.editor);
    params.setClipId(QString::fromStdString(f.clip_id));
    params.setParameter(0, QStringLiteral("lut"), 5.0);

    params.undo();

    const Clip *clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->video_effects[0].keys.empty(),
          "undo removes the key, returning the parameter to its default");
    const QVariantMap lut =
            params.effects()[0].toMap().value(QStringLiteral("parameters")).toList()[0].toMap();
    check(lut.value(QStringLiteral("value")).toDouble() == 0.0,
          "the projection reports the default after undo");
}

void test_refused_edit_reports_reason_and_leaves_project_unchanged()
{
    Fixture f = fixture();
    EffectParamsController params(&f.editor);
    params.setClipId(QString::fromStdString(f.clip_id));

    // Out-of-range entry: the clip has one link (index 0), so 1 is refused.
    check(!params.setParameter(1, QStringLiteral("lut"), 2.0), "an out-of-range entry is refused");
    check(!params.lastError().isEmpty(), "the refusal surfaces a reason");
    check(clip_at(f.editor, f.clip_id)->video_effects[0].keys.empty(),
          "a refused edit leaves the project unchanged");

    // A non-finite value is refused the same way.
    check(!params.setParameter(0, QStringLiteral("lut"), std::numeric_limits<double>::quiet_NaN()),
          "a non-finite value is refused");
    check(clip_at(f.editor, f.clip_id)->video_effects[0].keys.empty(),
          "a non-finite edit leaves the project unchanged");
}

void test_unknown_clip_is_refused()
{
    Fixture f = fixture();
    EffectParamsController params(&f.editor);
    params.setClipId(QStringLiteral("nope"));

    check(params.effects().isEmpty(), "an unknown clip projects no effects");
    check(!params.setParameter(0, QStringLiteral("lut"), 1.0),
          "an edit on an unknown clip is refused");
    check(!params.lastError().isEmpty(), "the refusal surfaces a reason");
}

void test_compound_types_project_their_component_fields()
{
    std::string clip_id;
    Editor editor = compound_editor(clip_id, "genesis.compound");
    const genesis::render::Catalogue catalogue = compound_catalogue();
    EffectParamsController params(&editor, nullptr, &catalogue);
    params.setClipId(QString::fromStdString(clip_id));

    const QVariantList parameters =
            params.effects()[0].toMap().value(QStringLiteral("parameters")).toList();
    check(parameters.size() == 4, "the compound pack declares four parameters");

    // Color: packed RGBA 0 reports every channel as 0.
    const QVariantMap tint = parameters[0].toMap();
    check(tint.value(QStringLiteral("type")).toString() == QStringLiteral("color"),
          "the first parameter is a color");
    check(near(tint.value(QStringLiteral("r")).toDouble(), 0.0)
                  && near(tint.value(QStringLiteral("g")).toDouble(), 0.0)
                  && near(tint.value(QStringLiteral("b")).toDouble(), 0.0)
                  && near(tint.value(QStringLiteral("a")).toDouble(), 0.0),
          "an untouched color reports all-zero channels");

    // Point: untouched reports the centred default.
    const QVariantMap position = parameters[1].toMap();
    check(position.value(QStringLiteral("type")).toString() == QStringLiteral("point"),
          "the second parameter is a point");
    check(near(position.value(QStringLiteral("x")).toDouble(), 0.5)
                  && near(position.value(QStringLiteral("y")).toDouble(), 0.5),
          "an untouched point reports the centred default");

    // Wheel: x/y default 0, master defaults to the parameter's default.
    const QVariantMap hue = parameters[2].toMap();
    check(hue.value(QStringLiteral("type")).toString() == QStringLiteral("wheel"),
          "the third parameter is a wheel");
    check(near(hue.value(QStringLiteral("x")).toDouble(), 0.0)
                  && near(hue.value(QStringLiteral("y")).toDouble(), 0.0)
                  && near(hue.value(QStringLiteral("m")).toDouble(), 0.5),
          "an untouched wheel reports the puck at origin and the default master");

    // Curve: fewer than two points is the identity line.
    const QVariantMap ramp = parameters[3].toMap();
    check(ramp.value(QStringLiteral("type")).toString() == QStringLiteral("curve"),
          "the fourth parameter is a curve");
    const QVariantList points = ramp.value(QStringLiteral("points")).toList();
    check(points.size() == 2 && near(points[0].toMap().value(QStringLiteral("x")).toDouble(), 0.0)
                  && near(points[0].toMap().value(QStringLiteral("y")).toDouble(), 0.0)
                  && near(points[1].toMap().value(QStringLiteral("x")).toDouble(), 1.0)
                  && near(points[1].toMap().value(QStringLiteral("y")).toDouble(), 1.0),
          "an untouched curve reports the identity line");
}

void test_color_setter_packs_rgba_and_round_trips()
{
    std::string clip_id;
    Editor editor = compound_editor(clip_id, "genesis.compound");
    const genesis::render::Catalogue catalogue = compound_catalogue();
    EffectParamsController params(&editor, nullptr, &catalogue);
    params.setClipId(QString::fromStdString(clip_id));

    check(params.setColor(0, QStringLiteral("tint"), 1.0, 0.5, 0.25, 0.0),
          "setting a color applies");

    // 1.0->255, 0.5->128, 0.25->64, 0.0->0: 0xFF804000 packed.
    const AppliedFilter &link = link_at(editor, clip_id);
    check(link.keys.size() == 1 && link.keys.count("tint") == 1,
          "the color edit lands as a key on `tint`");
    const std::vector<ParamKey> &run = link.keys.at("tint");
    check(run.size() == 1 && run[0].value == 4286595072.0,
          "the channels pack to one 32-bit RGBA value");

    const QVariantMap tint =
            params.effects()[0].toMap().value(QStringLiteral("parameters")).toList()[0].toMap();
    check(near(tint.value(QStringLiteral("r")).toDouble(), 1.0), "red reads back as 1.0");
    check(near(tint.value(QStringLiteral("g")).toDouble(), 128.0 / 255.0),
          "green reads back quantised");
    check(near(tint.value(QStringLiteral("b")).toDouble(), 64.0 / 255.0),
          "blue reads back quantised");
    check(near(tint.value(QStringLiteral("a")).toDouble(), 0.0), "alpha reads back 0");
}

void test_point_setter_writes_dotted_keys_as_one_undo()
{
    std::string clip_id;
    Editor editor = compound_editor(clip_id, "genesis.compound");
    const genesis::render::Catalogue catalogue = compound_catalogue();
    EffectParamsController params(&editor, nullptr, &catalogue);
    params.setClipId(QString::fromStdString(clip_id));

    check(params.setPoint(0, QStringLiteral("position"), 0.25, 0.75), "setting a point applies");

    const AppliedFilter &link = link_at(editor, clip_id);
    check(link.keys.size() == 2 && link.keys.count("position.x") == 1
                  && link.keys.count("position.y") == 1,
          "the point edit lands as two dotted keys");
    check(link.keys.at("position.x")[0].value == 0.25
                  && link.keys.at("position.y")[0].value == 0.75,
          "the dotted keys hold x and y");

    params.undo();
    check(link_at(editor, clip_id).keys.empty(),
          "one undo removes both dotted keys (a single step)");
}

void test_wheel_setter_writes_three_keys_as_one_undo()
{
    std::string clip_id;
    Editor editor = compound_editor(clip_id, "genesis.compound");
    const genesis::render::Catalogue catalogue = compound_catalogue();
    EffectParamsController params(&editor, nullptr, &catalogue);
    params.setClipId(QString::fromStdString(clip_id));

    check(params.setWheel(0, QStringLiteral("hue"), 0.1, 0.2, 0.3), "setting a wheel applies");

    const AppliedFilter &link = link_at(editor, clip_id);
    check(link.keys.size() == 3 && link.keys.count("hue.x") == 1 && link.keys.count("hue.y") == 1
                  && link.keys.count("hue.m") == 1,
          "the wheel edit lands as three dotted keys");
    check(link.keys.at("hue.m")[0].value == 0.3, "the master key holds m");

    params.undo();
    check(link_at(editor, clip_id).keys.empty(), "one undo removes all three dotted keys");
}

void test_curve_setter_sorts_and_clears_stale_slots()
{
    std::string clip_id;
    Editor editor = compound_editor(clip_id, "genesis.compound");
    const genesis::render::Catalogue catalogue = compound_catalogue();
    EffectParamsController params(&editor, nullptr, &catalogue);
    params.setClipId(QString::fromStdString(clip_id));

    QVariantList points;
    const auto point = [&points](double x, double y) {
        QVariantMap map;
        map.insert(QStringLiteral("x"), x);
        map.insert(QStringLiteral("y"), y);
        points.append(map);
    };
    point(0.8, 0.7);
    point(0.2, 0.3);
    point(0.5, 0.1);
    check(params.setCurve(0, QStringLiteral("ramp"), points), "setting a curve applies");

    const AppliedFilter &link = link_at(editor, clip_id);
    check(link.keys.count("ramp.0.x") == 1 && link.keys.count("ramp.0.y") == 1
                  && link.keys.count("ramp.1.x") == 1 && link.keys.count("ramp.1.y") == 1
                  && link.keys.count("ramp.2.x") == 1 && link.keys.count("ramp.2.y") == 1,
          "the curve edit lands as six dotted keys");
    check(link.keys.at("ramp.0.x")[0].value == 0.2 && link.keys.at("ramp.1.x")[0].value == 0.5
                  && link.keys.at("ramp.2.x")[0].value == 0.8,
          "the points are x-sorted before being written");

    // The projection reports the sorted points back.
    const QVariantMap ramp =
            params.effects()[0].toMap().value(QStringLiteral("parameters")).toList()[3].toMap();
    const QVariantList reported = ramp.value(QStringLiteral("points")).toList();
    check(reported.size() == 3
                  && near(reported[0].toMap().value(QStringLiteral("x")).toDouble(), 0.2)
                  && near(reported[2].toMap().value(QStringLiteral("x")).toDouble(), 0.8),
          "the projection reports the sorted curve points");

    params.undo();
    check(link_at(editor, clip_id).keys.empty(), "one undo removes every curve point");
}

void test_curve_setter_clears_stale_slots_and_refuses_bad_input()
{
    std::string clip_id;
    Editor editor = compound_editor(clip_id, "genesis.compound");
    const genesis::render::Catalogue catalogue = compound_catalogue();
    EffectParamsController params(&editor, nullptr, &catalogue);
    params.setClipId(QString::fromStdString(clip_id));

    // Three points first, then one: the shrunken curve must drop slots 1 and 2.
    QVariantList three;
    const auto point = [&three](double x, double y) {
        QVariantMap map;
        map.insert(QStringLiteral("x"), x);
        map.insert(QStringLiteral("y"), y);
        three.append(map);
    };
    point(0.1, 0.1);
    point(0.5, 0.5);
    point(0.9, 0.9);
    check(params.setCurve(0, QStringLiteral("ramp"), three), "three points apply");

    QVariantList one;
    QVariantMap map;
    map.insert(QStringLiteral("x"), 0.3);
    map.insert(QStringLiteral("y"), 0.6);
    one.append(map);
    check(params.setCurve(0, QStringLiteral("ramp"), one), "a one-point curve applies");

    const AppliedFilter &link = link_at(editor, clip_id);
    check(link.keys.count("ramp.0.x") == 1 && link.keys.count("ramp.0.y") == 1,
          "slot 0 keeps the single point");
    check(link.keys.count("ramp.1.x") == 0 && link.keys.count("ramp.1.y") == 0
                  && link.keys.count("ramp.2.x") == 0 && link.keys.count("ramp.2.y") == 0,
          "the stale slots are cleared");

    // More than MAX_CURVE_POINTS is refused outright.
    QVariantList too_many;
    for (int n = 0; n < 9; ++n) {
        QVariantMap m;
        m.insert(QStringLiteral("x"), n / 8.0);
        m.insert(QStringLiteral("y"), n / 8.0);
        too_many.append(m);
    }
    check(!params.setCurve(0, QStringLiteral("ramp"), too_many), "a nine-point curve is refused");
    check(!params.lastError().isEmpty(), "the refusal surfaces a reason");

    // An element that is not an {x,y} map is refused.
    QVariantList bad;
    bad.append(QStringLiteral("not a point"));
    check(!params.setCurve(0, QStringLiteral("ramp"), bad), "a non-map curve element is refused");
    check(!params.lastError().isEmpty(), "the refusal surfaces a reason");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_projection_reports_schema_and_enum_labels();
    test_setter_round_trips_as_a_key_at_the_clip_start();
    test_setter_undo_restores_the_default();
    test_refused_edit_reports_reason_and_leaves_project_unchanged();
    test_unknown_clip_is_refused();
    test_compound_types_project_their_component_fields();
    test_color_setter_packs_rgba_and_round_trips();
    test_point_setter_writes_dotted_keys_as_one_undo();
    test_wheel_setter_writes_three_keys_as_one_undo();
    test_curve_setter_sorts_and_clears_stale_slots();
    test_curve_setter_clears_stale_slots_and_refuses_bad_input();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
