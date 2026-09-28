// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The edit controller's suite: drives the UI-intent -> host-command mapping
// headlessly, so the drag/trim/delete/undo semantics of the studio shell are
// exercised without a GUI. The fixture is built through the real command layer
// (AddMedia/AddClip), so it cannot drift from the model; the controller is
// driven through its Q_INVOKABLE surface exactly as QML calls it.

#include <cstdio>
#include <string>

#include <QCoreApplication>
#include <QString>
#include <QVariantList>

#include "app/controllers/EditController.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Clip.h"
#include "project/model/Cutout.h"
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

void test_move_gesture_coalesces_and_undoes()
{
    Fixture f = fixture();
    EditController edit(&f.editor);

    // One drag, two live updates: the gesture folds into one undo step and
    // the clip lands at the last update's absolute start.
    edit.beginMove(QString::fromStdString(f.clip_id));
    edit.updateMove(QString::fromStdString(f.clip_id), 5.0);
    edit.updateMove(QString::fromStdString(f.clip_id), 8.0);
    edit.endMove();

    const Clip *clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->start == Rational{ 8, 1 }, "move lands at the final start");
    check(edit.canUndo(), "can undo after a move");
    check(!edit.canRedo(), "nothing to redo right after a move");

    edit.undo();
    check(clip_at(f.editor, f.clip_id)->start == Rational{ 1, 1 },
          "undo returns the clip to its original start");
    check(edit.canRedo(), "can redo after an undo");

    edit.redo();
    check(clip_at(f.editor, f.clip_id)->start == Rational{ 8, 1 },
          "redo replays the whole drag, not its last increment");
}

void test_trim_gesture_folds_cumulative_delta()
{
    Fixture f = fixture();
    EditController edit(&f.editor);

    // Tail trims: each update hands the cumulative delta, and the controller
    // steps back to the gesture's start before re-applying, so redo replays
    // the whole drag rather than only its last increment.
    edit.beginTrim(QString::fromStdString(f.clip_id), 1);
    edit.updateTrim(QString::fromStdString(f.clip_id), 1, 2.0);
    edit.updateTrim(QString::fromStdString(f.clip_id), 1, 3.0);
    edit.endTrim();

    const Clip *clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->duration == Rational{ 13, 1 },
          "trim folds to the cumulative delta");
    check(clip->start == Rational{ 1, 1 }, "a tail trim leaves the start put");

    edit.undo();
    check(clip_at(f.editor, f.clip_id)->duration == Rational{ 10, 1 },
          "undo restores the original length");
    edit.redo();
    check(clip_at(f.editor, f.clip_id)->duration == Rational{ 13, 1 },
          "redo replays the full trim");
}

void test_remove_clips_and_undo()
{
    Fixture f = fixture();
    const std::string track_id = f.editor.project().timelines[0].tracks[0].id;
    const auto second =
            f.editor.apply(Command{ AddClip{ f.media_id, track_id, Rational{ 20, 1 }, false } });
    check(second.has_value() && second->created_id.has_value(), "adds a second clip");
    const std::string second_id = *second->created_id;

    EditController edit(&f.editor);
    QVariantList ids;
    ids.append(QString::fromStdString(f.clip_id));
    edit.removeClips(ids);

    check(clip_at(f.editor, f.clip_id) == nullptr, "the named clip is gone");
    check(clip_at(f.editor, second_id) != nullptr, "the unnamed clip stays");

    edit.undo();
    const Clip *restored = clip_at(f.editor, f.clip_id);
    check(restored != nullptr && restored->start == Rational{ 1, 1 },
          "undo restores the removed clip");
}

void test_selection_and_dirty()
{
    Fixture f = fixture();
    // The fixture's own AddMedia/AddClip already made the project dirty and
    // left undo steps; mark it saved so `dirty` reflects only what the
    // controller does.
    f.editor.mark_saved();
    EditController edit(&f.editor);

    check(!edit.dirty(), "clean at start");
    check(edit.selectedClipId().isEmpty(), "no selection at start");

    edit.select(QString::fromStdString(f.clip_id));
    check(edit.selectedClipId() == QString::fromStdString(f.clip_id), "selection sticks");
    check(!edit.dirty(), "selection alone is not an edit");

    edit.removeSelected();
    check(clip_at(f.editor, f.clip_id) == nullptr, "removeSelected deletes the selected clip");
    check(edit.dirty(), "dirty after a delete");
    check(edit.canUndo(), "can undo after a delete");
    check(edit.selectedClipId().isEmpty(), "selection clears after delete");

    edit.undo();
    check(clip_at(f.editor, f.clip_id) != nullptr, "undo restores the deleted clip");
}

void test_snap_quantises_move_to_frame_grid()
{
    Fixture f = fixture();
    EditController edit(&f.editor);

    edit.setSnapEnabled(true);
    edit.beginMove(QString::fromStdString(f.clip_id));
    edit.updateMove(QString::fromStdString(f.clip_id), 5.02);
    edit.endMove();

    // 5.02 s at 30 fps is 150.6 frames; snapping half-up lands the clip on
    // frame 151, i.e. 151/30 s, not wherever the pointer stopped.
    check(clip_at(f.editor, f.clip_id)->start == Rational{ 151, 30 },
          "snap quantises the move to the frame grid");
    check(edit.snapEnabled(), "snap toggle stays on");

    // With snap off the same value lands exactly where asked.
    edit.setSnapEnabled(false);
    edit.beginMove(QString::fromStdString(f.clip_id));
    edit.updateMove(QString::fromStdString(f.clip_id), 5.02);
    edit.endMove();
    check(clip_at(f.editor, f.clip_id)->start == Rational{ 251, 50 },
          "without snap the move is exact");
}

void test_snap_pulls_clip_edges_to_neighbours_and_playhead()
{
    Fixture f = fixture();
    const std::string track_id = f.editor.project().timelines[0].tracks[0].id;
    const auto neighbour =
            f.editor.apply(Command{ AddClip{ f.media_id, track_id, Rational{ 20, 1 }, false } });
    check(neighbour.has_value(), "adds a neighbour clip");

    EditController edit(&f.editor);
    edit.setSnapEnabled(true);
    const QString id = QString::fromStdString(f.clip_id);
    // 0.1 s per pixel makes the 8 px threshold 0.8 s.
    const double secsPerPx = 0.1;

    // The clip's tail (start + 10) catches the neighbour's start at 20 s.
    edit.beginMove(id);
    edit.updateMove(id, 10.05, 0.0, secsPerPx);
    edit.endMove();
    check(clip_at(f.editor, f.clip_id)->start == Rational{ 10, 1 },
          "the tail snaps to the neighbour's start");

    // The head catches the playhead.
    edit.beginMove(id);
    edit.updateMove(id, 4.9, 5.0, secsPerPx);
    edit.endMove();
    check(clip_at(f.editor, f.clip_id)->start == Rational{ 5, 1 },
          "the head snaps to the playhead");

    // The head catches the timeline start.
    edit.beginMove(id);
    edit.updateMove(id, 0.2, 0.0, secsPerPx);
    edit.endMove();
    check(clip_at(f.editor, f.clip_id)->start == Rational::ZERO,
          "the head snaps to the timeline start");

    // Outside the threshold nothing catches, so the frame grid takes over.
    edit.beginMove(id);
    edit.updateMove(id, 10.05, 0.0, 0.001);
    edit.endMove();
    check(clip_at(f.editor, f.clip_id)->start == Rational{ 302, 30 },
          "outside the threshold the frame grid still applies");
}

void test_ripple_tail_trim_moves_later_clips()
{
    Fixture f = fixture();
    const std::string track_id = f.editor.project().timelines[0].tracks[0].id;
    const auto second =
            f.editor.apply(Command{ AddClip{ f.media_id, track_id, Rational{ 20, 1 }, false } });
    check(second.has_value() && second->created_id.has_value(), "adds a later clip");
    const std::string second_id = *second->created_id;

    EditController edit(&f.editor);
    edit.setRippleEnabled(true);
    edit.beginTrim(QString::fromStdString(f.clip_id), 1);
    edit.updateTrim(QString::fromStdString(f.clip_id), 1, 3.0);
    edit.endTrim();

    check(clip_at(f.editor, f.clip_id)->duration == Rational{ 13, 1 },
          "the trimmed clip lengthens by the delta");
    check(clip_at(f.editor, second_id)->start == Rational{ 23, 1 },
          "ripple moves the later clip by the length change");
}

void test_track_flag_apply_and_undo()
{
    Fixture f = fixture();
    const std::string track_id = f.editor.project().active().tracks[0].id;
    EditController edit(&f.editor);

    edit.setTrackFlag(QString::fromStdString(track_id), 1, true);
    check(f.editor.project().active().tracks[0].muted, "the mute flag is set");
    edit.setTrackFlag(QString::fromStdString(track_id), 0, false);
    check(!f.editor.project().active().tracks[0].visible, "the visible flag is cleared");

    edit.undo();
    edit.undo();
    check(!f.editor.project().active().tracks[0].muted
                  && f.editor.project().active().tracks[0].visible,
          "two undos restore both toggles");
}

void test_inspector_edits_round_trip()
{
    Fixture f = fixture();
    EditController edit(&f.editor);
    const QString id = QString::fromStdString(f.clip_id);

    edit.renameClip(id, "renamed");
    const Clip *clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->name == "renamed", "renameClip updates the display name");

    edit.setClipTransform(id, 1.5, 0.25, -0.5, 90.0, 1.1, 0.9);
    clip = clip_at(f.editor, f.clip_id);
    check(clip->scale == 1.5 && clip->offset_x == 0.25 && clip->offset_y == -0.5
                  && clip->rotation == 90.0 && clip->stretch_x == 1.1 && clip->stretch_y == 0.9,
          "transform lands on all six fields");

    edit.setClipOpacity(id, 0.5);
    edit.setClipSpeed(id, 2.0);
    edit.setClipFades(id, 0.5, 1.0);
    clip = clip_at(f.editor, f.clip_id);
    check(clip->opacity == 0.5, "opacity lands");
    check(clip->speed == 2.0, "speed lands");
    check(clip->fade_in == Rational{ 1, 2 } && clip->fade_out == Rational{ 1, 1 },
          "fades land as rationals");

    // Rename, transform, opacity, speed, fades: five undo steps back to the
    // fixture's defaults.
    for (int i = 0; i < 5; i++) {
        edit.undo();
    }
    clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->name == "a.mp4" && clip->scale == 1.0 && clip->opacity == 1.0
                  && clip->speed == 1.0 && clip->fade_in.is_zero() && clip->fade_out.is_zero(),
          "undo walks every inspector edit back");
}

void test_reverse_toggle_round_trips()
{
    Fixture f = fixture();
    EditController edit(&f.editor);
    const QString id = QString::fromStdString(f.clip_id);

    check(clip_at(f.editor, f.clip_id) != nullptr && !clip_at(f.editor, f.clip_id)->reverse,
          "a clip starts forwards");

    edit.setClipReverse(id, true);
    const Clip *reversed = clip_at(f.editor, f.clip_id);
    check(reversed != nullptr && reversed->reverse, "the toggle lands");

    edit.undo();
    const Clip *restored = clip_at(f.editor, f.clip_id);
    check(restored != nullptr && !restored->reverse, "undo plays forwards again");
}

void test_add_cutout_stroke()
{
    Fixture f = fixture();
    EditController edit(&f.editor);
    const QString id = QString::fromStdString(f.clip_id);

    // A fresh clip has no cutout; the first stroke creates one, custom rather
    // than automatic, since a stroke is a correction to the mask.
    check(!clip_at(f.editor, f.clip_id)->cutout.has_value(), "a clip starts with no cutout");

    QVariantList points;
    points.append(0.1);
    points.append(0.2);
    points.append(0.3);
    points.append(0.4);
    edit.addCutoutStroke(id, 0, 0.05, points, 2.5);

    const Clip *clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->cutout.has_value(), "the stroke creates a cutout");
    check(clip->cutout->mode == CutoutMode::Custom, "the cutout is custom, not automatic");
    check(clip->cutout->strokes.size() == 1, "one stroke lands");
    const Stroke &stroke = clip->cutout->strokes[0];
    check(stroke.tool == BrushTool::SmartBrush, "the smart brush lands");
    check(stroke.size == 0.05, "the size lands");
    check(stroke.points.size() == 2 && stroke.points[0][0] == 0.1 && stroke.points[0][1] == 0.2
                  && stroke.points[1][0] == 0.3 && stroke.points[1][1] == 0.4,
          "the points land as fractions");
    check(stroke.at.has_value() && *stroke.at == Rational{ 5, 2 },
          "the instant lands as a rational");

    // The stroke is one undo step, and undo takes the cutout back off.
    edit.undo();
    check(!clip_at(f.editor, f.clip_id)->cutout.has_value(), "undo removes the cutout");

    // A plain brush with a negative instant paints a disc: the stroke lands
    // but carries no source instant.
    edit.addCutoutStroke(id, 1, 0.1, points, -1.0);
    clip = clip_at(f.editor, f.clip_id);
    check(clip != nullptr && clip->cutout.has_value() && clip->cutout->strokes.size() == 1,
          "the plain brush recreates the cutout");
    check(clip->cutout->strokes[0].tool == BrushTool::Brush, "the plain brush lands");
    check(!clip->cutout->strokes[0].at.has_value(), "a plain brush records no instant");
}

void test_cutout_stroke_added_signal()
{
    Fixture f = fixture();
    EditController edit(&f.editor);
    const QString id = QString::fromStdString(f.clip_id);

    int emitted = 0;
    QString last_id;
    QObject::connect(&edit, &EditController::cutoutStrokeAdded, [&](const QString &clip_id) {
        ++emitted;
        last_id = clip_id;
    });

    QVariantList points;
    points.append(0.1);
    points.append(0.2);
    points.append(0.3);
    points.append(0.4);

    // A valid stroke commits and signals once, naming its clip.
    edit.addCutoutStroke(id, 0, 0.05, points, 2.5);
    check(emitted == 1, "one committed stroke emits once");
    check(last_id == id, "the signal names the clip");

    // A malformed gesture (a half-point) is dropped and signals nothing.
    QVariantList odd;
    odd.append(0.1);
    edit.addCutoutStroke(id, 0, 0.05, odd, 2.5);
    check(emitted == 1, "a dropped stroke signals nothing");
}

void test_pan_round_trips()
{
    Fixture f = fixture();
    EditController edit(&f.editor);
    const QString id = QString::fromStdString(f.clip_id);

    check(clip_at(f.editor, f.clip_id) != nullptr && clip_at(f.editor, f.clip_id)->pan == 0.0,
          "a clip starts centred");

    edit.setClipPan(id, -0.75);
    const Clip *panned = clip_at(f.editor, f.clip_id);
    check(panned != nullptr && panned->pan == -0.75, "the pan lands");

    edit.undo();
    const Clip *restored = clip_at(f.editor, f.clip_id);
    check(restored != nullptr && restored->pan == 0.0, "undo returns to centred");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_move_gesture_coalesces_and_undoes();
    test_trim_gesture_folds_cumulative_delta();
    test_remove_clips_and_undo();
    test_selection_and_dirty();
    test_snap_quantises_move_to_frame_grid();
    test_snap_pulls_clip_edges_to_neighbours_and_playhead();
    test_ripple_tail_trim_moves_later_clips();
    test_track_flag_apply_and_undo();
    test_inspector_edits_round_trip();
    test_reverse_toggle_round_trips();
    test_add_cutout_stroke();
    test_cutout_stroke_added_signal();
    test_pan_round_trips();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
