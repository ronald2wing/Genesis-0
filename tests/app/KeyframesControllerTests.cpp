// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The keyframes controller's suite: drives the pane's projection and edit
// gestures headlessly, so the key view, the curve sampler, the command routing
// and the drag-is-one-undo-step semantics are exercised without a GUI. The
// fixture is built through the real command layer (AddMedia/AddClip), so it
// cannot drift from the model; the controller is driven through its
// Q_INVOKABLE surface exactly as QML calls it.

#include <cstdio>
#include <string>

#include <QCoreApplication>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "app/controllers/KeyframesController.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Clip.h"
#include "project/model/Keyframe.h"
#include "project/model/Media.h"
#include "project/Editor.h"

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

// A project with one video media item and one clip of it on the bottom lane at
// 1s, ten seconds long.
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
    return f;
}

const Clip *clip_at(const Editor &editor, const std::string &clip_id)
{
    return editor.project().active().clip(clip_id);
}

void test_unkeyed_then_keyed_projection()
{
    Fixture f = fixture();
    KeyframesController keys(&f.editor);
    keys.setClipId(QString::fromStdString(f.clip_id));

    check(keys.keyedProperties().isEmpty(), "an unkeyed clip reports no keyed properties");
    check(keys.keysFor(QStringLiteral("scale")).isEmpty(), "an unkeyed property has no keys");

    check(keys.addKey(QStringLiteral("scale"), 0.5, 1.5, QStringLiteral("linear")),
          "adding a key succeeds");

    const QVariantList properties = keys.keyedProperties();
    check(properties.size() == 1, "one keyed property after adding a key");
    check(properties[0].toMap().value(QStringLiteral("property")).toString()
                  == QStringLiteral("scale"),
          "the keyed property is the one that was keyed");

    const QVariantList held = keys.keysFor(QStringLiteral("scale"));
    check(held.size() == 1, "the property reports one key");
    check(held[0].toMap().value(QStringLiteral("at")).toDouble() == 0.5,
          "the key's time is reported");
    check(held[0].toMap().value(QStringLiteral("value")).toDouble() == 1.5,
          "the key's value is reported");
    check(held[0].toMap().value(QStringLiteral("ease")).toString() == QStringLiteral("linear"),
          "the key's ease is reported");
}

void test_add_key_routes_through_command_layer_and_undoes()
{
    Fixture f = fixture();
    KeyframesController keys(&f.editor);
    keys.setClipId(QString::fromStdString(f.clip_id));

    check(keys.addKey(QStringLiteral("scale"), 0.5, 1.5, QStringLiteral("linear")),
          "adding a key applies");
    const Clip *clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->keys.size() == 1, "the key lands in the model");
    check(keys.canUndo(), "undo is available after adding a key");

    keys.undo();
    clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->keys.empty(), "undo restores the clip exactly (no keys)");
}

void test_remove_key_and_undo_restores_it()
{
    Fixture f = fixture();
    KeyframesController keys(&f.editor);
    keys.setClipId(QString::fromStdString(f.clip_id));
    keys.addKey(QStringLiteral("scale"), 0.5, 1.5, QStringLiteral("linear"));

    check(keys.removeKey(QStringLiteral("scale"), 0.5), "removing the key applies");
    check(clip_at(f.editor, f.clip_id)->keys.empty(), "the key is gone");

    keys.undo();
    const Clip *clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->keys.size() == 1 && clip->keys[0].value == 1.5,
          "undo restores the removed key");
}

void test_sample_linear_ramp_and_endpoints()
{
    Fixture f = fixture();
    KeyframesController keys(&f.editor);
    keys.setClipId(QString::fromStdString(f.clip_id));
    // Opacity clamps into 0..=1, so a 0 -> 1 ramp keeps its endpoints.
    keys.addKey(QStringLiteral("opacity"), 0.0, 0.0, QStringLiteral("linear"));
    keys.addKey(QStringLiteral("opacity"), 1.0, 1.0, QStringLiteral("linear"));

    const QVariantList samples = keys.sample(QStringLiteral("opacity"), 0.0, 1.0, 5);
    check(samples.size() == 5, "sample returns the requested count");
    check(samples[0].toMap().value(QStringLiteral("at")).toDouble() == 0.0
                  && samples[0].toMap().value(QStringLiteral("value")).toDouble() == 0.0,
          "the first sample is exactly at `from`");
    check(samples[4].toMap().value(QStringLiteral("at")).toDouble() == 1.0
                  && samples[4].toMap().value(QStringLiteral("value")).toDouble() == 1.0,
          "the last sample is exactly at `to`");

    bool monotone = true;
    for (int i = 1; i < samples.size(); ++i) {
        monotone = monotone
                && samples[i - 1].toMap().value(QStringLiteral("value")).toDouble()
                        <= samples[i].toMap().value(QStringLiteral("value")).toDouble();
    }
    check(monotone, "a linear two-key track samples as a monotone ramp");
}

void test_sample_eased_key_differs_at_midpoint()
{
    Fixture f = fixture();
    KeyframesController keys(&f.editor);
    keys.setClipId(QString::fromStdString(f.clip_id));
    // offsetX and offsetY hold 0..=1 exactly (their clamp is ±3), so the
    // eased midpoints compare against the linear 0.5 directly.
    keys.addKey(QStringLiteral("offsetX"), 0.0, 0.0, QStringLiteral("linear"));
    keys.addKey(QStringLiteral("offsetX"), 1.0, 1.0, QStringLiteral("in"));
    keys.addKey(QStringLiteral("offsetY"), 0.0, 0.0, QStringLiteral("linear"));
    keys.addKey(QStringLiteral("offsetY"), 1.0, 1.0, QStringLiteral("out"));

    // count 3 puts the middle sample exactly at t = 0.5.
    const QVariantList eased_in = keys.sample(QStringLiteral("offsetX"), 0.0, 1.0, 3);
    const QVariantList eased_out = keys.sample(QStringLiteral("offsetY"), 0.0, 1.0, 3);
    const double in_mid = eased_in[1].toMap().value(QStringLiteral("value")).toDouble();
    const double out_mid = eased_out[1].toMap().value(QStringLiteral("value")).toDouble();

    check(in_mid < 0.5, "Ease::in lags the linear ramp at the midpoint");
    check(out_mid > 0.5, "Ease::out leads the linear ramp at the midpoint");
}

void test_drag_gesture_is_one_undo_step()
{
    Fixture f = fixture();
    KeyframesController keys(&f.editor);
    keys.setClipId(QString::fromStdString(f.clip_id));
    keys.addKey(QStringLiteral("scale"), 0.5, 1.0, QStringLiteral("linear"));

    // One drag, three live updates: the gesture folds into one undo step and
    // the key lands at the last update's position with no duplicates.
    check(keys.beginKeyMove(QStringLiteral("scale"), 0.5), "the gesture opens on the key");
    keys.updateKeyMove(QStringLiteral("scale"), 0.6, 1.5);
    keys.updateKeyMove(QStringLiteral("scale"), 0.7, 2.0);
    keys.endKeyMove();

    const Clip *clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->keys.size() == 1,
          "a drag leaves exactly one key (no duplicates)");
    check(clip != nullptr && clip->keys[0].at == 0.7 && clip->keys[0].value == 2.0,
          "the key lands at the final position and value");

    // One undo returns to the pre-drag value: if the gesture had recorded one
    // step per update, this would land at the first update instead.
    keys.undo();
    clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->keys.size() == 1 && clip->keys[0].at == 0.5
                  && clip->keys[0].value == 1.0,
          "one undo returns the key to its pre-drag value");
}

void test_refused_edit_reports_reason_and_leaves_project_unchanged()
{
    Fixture f = fixture();
    KeyframesController keys(&f.editor);
    keys.setClipId(QStringLiteral("nope"));

    check(!keys.addKey(QStringLiteral("scale"), 0.5, 1.0, QStringLiteral("linear")),
          "an edit on an unknown clip is refused");
    check(!keys.lastError().isEmpty(), "the refusal surfaces a reason");

    // The project is untouched: the real clip still has no keys.
    check(clip_at(f.editor, f.clip_id)->keys.empty(),
          "a refused edit leaves the project unchanged");
}

void test_set_key_ease_keeps_position_and_value()
{
    Fixture f = fixture();
    KeyframesController keys(&f.editor);
    keys.setClipId(QString::fromStdString(f.clip_id));
    keys.addKey(QStringLiteral("scale"), 0.5, 1.5, QStringLiteral("linear"));

    check(keys.setKeyEase(QStringLiteral("scale"), 0.5, QStringLiteral("in")),
          "setting the ease applies");
    const Clip *clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->keys.size() == 1 && clip->keys[0].at == 0.5
                  && clip->keys[0].value == 1.5,
          "re-easing keeps the key's position and value");
    check(clip != nullptr && clip->keys[0].ease.is(KeyEase::in()), "the ease lands as Ease::in");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_unkeyed_then_keyed_projection();
    test_add_key_routes_through_command_layer_and_undoes();
    test_remove_key_and_undo_restores_it();
    test_sample_linear_ramp_and_endpoints();
    test_sample_eased_key_differs_at_midpoint();
    test_drag_gesture_is_one_undo_step();
    test_refused_edit_reports_reason_and_leaves_project_unchanged();
    test_set_key_ease_keeps_position_and_value();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
