// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A fake QObject TimelineModel for QML interaction tests. Supplies a single
// track with a single clip and one media entry so the surfaces have enough
// data to render their delegates without linking the real project/model.

#pragma once

#include <QVariantList>
#include <QVariantMap>
#include <QObject>
#include <QString>

class FakeTimelineModel : public QObject
{
    Q_OBJECT

public:
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(double duration READ duration WRITE setDuration NOTIFY durationChanged)
    Q_PROPERTY(QVariantList tracks READ tracks CONSTANT)
    Q_PROPERTY(QVariantList media READ media CONSTANT)

    QString name() const { return QStringLiteral("Timeline 1"); }
    double duration() const { return duration_; }
    // Tests set this to 0 to model an empty timeline (the first-clip drop).
    void setDuration(double seconds)
    {
        if (duration_ == seconds)
            return;
        duration_ = seconds;
        emit durationChanged();
    }

signals:
    void durationChanged();

public:
    double duration_ = 10.0;

    QVariantList tracks() const
    {
        QVariantList clips;
        QVariantMap clip;
        clip[QStringLiteral("id")] = QStringLiteral("c1");
        clip[QStringLiteral("start")] = 1.0;
        clip[QStringLiteral("duration")] = 4.0;
        clip[QStringLiteral("name")] = QStringLiteral("a.mp4");
        clip[QStringLiteral("kind")] = QStringLiteral("video");
        clip[QStringLiteral("media_id")] = QStringLiteral("m1");
        clips.append(clip);

        QVariantMap track;
        track[QStringLiteral("id")] = QStringLiteral("V1");
        track[QStringLiteral("visible")] = true;
        track[QStringLiteral("muted")] = false;
        track[QStringLiteral("clips")] = clips;

        QVariantList tracks;
        tracks.append(track);
        return tracks;
    }

    QVariantList media() const
    {
        QVariantMap m;
        m[QStringLiteral("id")] = QStringLiteral("m1");
        m[QStringLiteral("path")] = QStringLiteral("/a.mp4");
        m[QStringLiteral("name")] = QStringLiteral("a.mp4");
        m[QStringLiteral("kind")] = QStringLiteral("video");
        m[QStringLiteral("duration")] = 10.0;
        m[QStringLiteral("width")] = 1920;
        m[QStringLiteral("height")] = 1080;
        m[QStringLiteral("frame_rate")] = 30.0;
        m[QStringLiteral("video_codec")] = QStringLiteral("h264");
        m[QStringLiteral("audio_codec")] = QStringLiteral("");
        m[QStringLiteral("has_audio")] = false;
        m[QStringLiteral("missing")] = false;

        QVariantList media;
        media.append(m);
        return media;
    }

    Q_INVOKABLE QString fmt(double /*seconds*/) const { return QStringLiteral("00:00.00"); }
};