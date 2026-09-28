// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's effect-parameter controller: the thin, testable layer
// that exposes the selected clip's applied effect chain (its pack schema and
// each parameter's current value) to QML and turns one parameter edit into a
// host command, applied through the project Editor so every edit is one
// undoable step. It owns no QML - only the selection of the clip being
// inspected, the read-only projection the inspector draws, and the mapping
// from a knob change to a command. The shell sets the clip id (it already
// tracks selection); the pane binds to the properties below and calls the
// Q_INVOKABLEs.
//
// Static-vs-keyframed mapping. The model stores a parameter two ways: a
// static value in AppliedFilter::params, and a keyframed ride in
// AppliedFilter::keys. The command layer, however, exposes no "set static
// parameter" verb - only SetEffectKey (a key at one point) and its clear
// siblings. Rather than add a command (which touches Command.h, the Apply
// dispatcher, Validate and the verb group together - a documented fragile
// contract) or rebuild a clip's whole video_effects chain through UpdateClip
// for one knob, this controller represents a static edit as the documented
// minimal equivalent: a single key at the clip start (`at = 0`), applied via
// SetEffectKey. A lone key holds its value across the whole clip
// (KeyTrack::value_at returns the first key's value before it and the last
// key's after it), and resolve_pass reads params_at(at), which substitutes
// the ride - so a key at the clip start renders byte-for-byte as a constant
// parameter. The projection reads the same point (AppliedFilter::value_at at
// 0) so what the inspector shows is what the renderer draws. When
// effect-parameter keyframing lands, only the read point (and a playhead the
// shell already owns) moves; the command path does not.
//
// Scope: the picture chain (Clip::video_effects) only, matching SetEffectKey.
// Audio filters are not editable through this path and are out of scope here.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <string>
#include <vector>

namespace genesis::project {
class Editor;
struct Command;
struct Clip;
} // namespace genesis::project

namespace genesis::render {
class Catalogue;
} // namespace genesis::render

class EffectParamsController : public QObject
{
    Q_OBJECT

    // The clip currently being inspected. Set by the shell, which already owns
    // selection; empty means nothing is selected and the pane is idle.
    Q_PROPERTY(QString clipId READ clipId WRITE setClipId NOTIFY clipIdChanged)
    // The reason the last edit was refused, empty when the last edit applied.
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)
    // Undo/redo/dirty, mirrored from the host Editor so the toolbar that
    // EditController also feeds stays in step with parameter edits.
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY historyChanged)

public:
    // `catalogue` is the pack catalogue the projection resolves against;
    // null falls back to the builtin catalogue (the builtin packs' roots).
    // Injectable so tests can project a synthetic pack declaring the
    // compound types without shipping one.
    explicit EffectParamsController(genesis::project::Editor *editor, QObject *parent = nullptr,
                                    const genesis::render::Catalogue *catalogue = nullptr);

    QString clipId() const { return clipId_; }
    void setClipId(const QString &clipId);

    QString lastError() const { return lastError_; }

    bool canUndo() const;
    bool canRedo() const;
    bool dirty() const;

    // ---- Read-only projection for the pane. ----

    // The selected clip's applied picture effects, one map per link of its
    // video_effects chain in chain order:
    //   { "entry": int, "id": QString, "name": QString, "enabled": bool,
    //     "parameters": QVariantList }
    // `entry` is the index the setter takes; `name` is the pack's display name
    // (empty for an id the catalogue no longer knows); `parameters` is the
    // pack's declared knobs in manifest order, each a map:
    //   { "key", "label", "group", "type", "min", "max", "step", "unit",
    //     "animate", "defaultValue", "value", "values", "labels" }
    // `type` is "float"/"int"/"bool"/"enum"/"color"/"point"/"wheel"/"curve";
    // `values`/`labels` are the enum's choices (empty for every other type);
    // `value` is the parameter's current value at the clip start, which is
    // also its whole-clip value under the static representation above. The
    // compound types add their own component fields beside `value`:
    //   color -> "r","g","b","a" (each 0..1, unpacked from the packed RGBA);
    //   point -> "x","y"; wheel -> "x","y","m";
    //   curve -> "points" (a list of {x,y} maps, x-sorted like the resolver).
    // Empty when nothing is selected; a link whose pack is unknown still
    // appears (id, enabled) but with no parameters, so the pane shows the
    // effect is there without pretending it can edit it.
    Q_INVOKABLE QVariantList effects() const;

    // ---- Edits, one undo step each. Returns true when applied, false when
    // ---- refused or a no-op; refusals land in `lastError`. ----

    // Sets one parameter of one link to `value`, recorded as a single key at
    // the clip start (see the header note). `entry` indexes the link in
    // video_effects; `key` is the parameter's manifest key; `value` is in the
    // parameter's own units (an enum's index, a float's number, and so on) and
    // is forwarded unclamped - the pack's range is the pane's to keep, exactly
    // as SetEffectKey documents.
    Q_INVOKABLE bool setParameter(int entry, const QString &key, double value);

    // Sets a color parameter's packed RGBA from four 0..1 channels. One key
    // under `key`; the channels are clamped into 0..1 and packed the way the
    // resolver unpacks them (R in the high byte), so it is a single undo step.
    Q_INVOKABLE bool setColor(int entry, const QString &key, double r, double g, double b,
                              double a);

    // Sets a point parameter's position: two keys, `<key>.x` and `<key>.y`,
    // in the 0..1 square, applied as one batch (one undo step). Forwarded
    // unclamped, like setParameter.
    Q_INVOKABLE bool setPoint(int entry, const QString &key, double x, double y);

    // Sets a wheel parameter's puck and master: `<key>.x`, `<key>.y` and
    // `<key>.m`, applied as one batch (one undo step). Forwarded unclamped.
    Q_INVOKABLE bool setWheel(int entry, const QString &key, double x, double y, double m);

    // Replaces a curve parameter's control points. `points` is a list of
    // {x, y} maps in the unit square; they are clamped, x-sorted and de-
    // duplicated (same x keeps the later, exactly as the resolver reads
    // them), then written to `<key>.<n>.x`/`.y` for n in order - clearing any
    // stale slot beyond the new count - as one batch (one undo step). An
    // empty list clears the curve back to its default.
    Q_INVOKABLE bool setCurve(int entry, const QString &key, const QVariantList &points);

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

signals:
    // clipId changed: the pane re-reads the clip's effects.
    void clipIdChanged();
    // lastError changed: the pane shows or hides the refusal reason.
    void errorChanged();
    // canUndo / canRedo / dirty changed.
    void historyChanged();
    // The clip's effect parameters changed (a set, or an undo/redo through
    // this controller): the pane re-reads effects().
    void effectsChanged();
    // The project changed and the shell should reload the engine/timeline.
    void projectEdited();

private:
    // The selected clip on the active timeline, or null when none/unknown.
    const genesis::project::Clip *clip() const;

    // The selected clip with `entry` in range, or null (the refusal is
    // already recorded). The shared front half of every setter.
    const genesis::project::Clip *target(int entry);

    // Records a refusal reason and emits errorChanged.
    void refuse(const QString &reason);
    // Clears a previously recorded refusal.
    void clearError();

    // Applies one discrete command (one undo step) and notifies; false when
    // refused or a tolerated no-op.
    bool applyEdit(const genesis::project::Command &command);

    // Applies several commands as one Batch - one undo step - and notifies.
    bool applyBatch(std::vector<genesis::project::Command> commands);

    genesis::project::Editor *editor_;
    const genesis::render::Catalogue *catalogue_;
    QString clipId_;
    QString lastError_;
};
