// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The host->UI bridge for the studio shell: a projection of the active
// timeline (name, duration, frame rate, tracks and their clips) that the QML
// timeline view and clip list draw. It owns its copies - QString and double,
// never a reference into the host project - so it cannot outlive the project
// it was built from (R4/R5: the host owns the model; the UI reads a snapshot
// and never touches host memory). Rational time is converted to double here,
// at the presentation boundary, and nowhere earlier.
//
// The projection is rebuilt after every edit (TimelineModel::rebuild): the
// shell holds one of these and re-points it at the host project whenever the
// EditController reports a change. It is still read-only - no path from QML
// mutates it; edits flow through the EditController into the host Editor.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

namespace genesis::project {
struct Project;
}

class TimelineModel : public QObject
{
    Q_OBJECT

    // The active timeline's tab label.
    Q_PROPERTY(QString name READ name NOTIFY rebuilt)
    // Timeline length in seconds (double: the presentation form of Rational).
    Q_PROPERTY(double duration READ duration NOTIFY rebuilt)
    // Frames per second, for the mm:ss.ff readout.
    Q_PROPERTY(double frameRate READ frameRate NOTIFY rebuilt)
    // Tracks, top to bottom. Each is a map { id, visible, muted, clips }, and
    // each clip a map { id, start, duration, name, kind, media_id, ... } with
    // seconds as double. `id` lets the QML name a clip back to the
    // EditController; `media_id` names its bin item back to the media bin.
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY rebuilt)
    // The media bin: every imported file, as a map { id, path, name, kind,
    // duration, width, height, frame_rate, frame_rate_fraction, video_codec,
    // audio_codec, has_audio, missing }. Duration is seconds as double (-1 when
    // the probe reported none); width/height/frame_rate are -1 when absent.
    Q_PROPERTY(QVariantList media READ media NOTIFY rebuilt)

public:
    explicit TimelineModel(const genesis::project::Project &project, QObject *parent = nullptr);

    QString name() const { return name_; }
    double duration() const { return duration_; }
    double frameRate() const { return frameRate_; }
    QVariantList tracks() const { return tracks_; }
    QVariantList media() const { return media_; }

    // Re-derives every property from the host project and notifies QML. One
    // signal covers all four properties: the shell re-reads them together.
    void rebuild(const genesis::project::Project &project);

    // Seconds -> "mm:ss.ff" against frameRate, the readout every pane shows.
    Q_INVOKABLE QString fmt(double seconds) const;

signals:
    void rebuilt();

private:
    void refresh(const genesis::project::Project &project);

    QString name_;
    double duration_ = 0.0;
    double frameRate_ = 30.0;
    QVariantList tracks_;
    QVariantList media_;
};
