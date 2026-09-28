// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/model/TimelineModel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <QVariantMap>

#include "project/Project.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"
#include "project/model/Track.h"

namespace {

// ClipKind -> the short token the QML maps to a colour. The kind, not the
// colour, crosses the bridge: colour is a presentation decision that belongs
// to the QML layer.
const char *kind_token(genesis::project::ClipKind kind)
{
    using genesis::project::ClipKind;
    switch (kind) {
    case ClipKind::Video:
        return "video";
    case ClipKind::Audio:
        return "audio";
    case ClipKind::Image:
        return "image";
    case ClipKind::Text:
        return "text";
    case ClipKind::Layer:
        return "layer";
    }
    return "video";
}

// MediaKind -> the short token the bin card maps to its thumbnail treatment.
// A sibling of kind_token: the kind crosses the bridge, the treatment is the
// QML layer's call.
const char *media_kind_token(genesis::project::MediaKind kind)
{
    using genesis::project::MediaKind;
    switch (kind) {
    case MediaKind::Video:
        return "video";
    case MediaKind::Audio:
        return "audio";
    case MediaKind::Image:
        return "image";
    }
    return "video";
}

} // namespace

TimelineModel::TimelineModel(const genesis::project::Project &project, QObject *parent)
    : QObject(parent)
{
    refresh(project);
}

void TimelineModel::rebuild(const genesis::project::Project &project)
{
    refresh(project);
    emit rebuilt();
}

void TimelineModel::refresh(const genesis::project::Project &project)
{
    const genesis::project::Timeline &timeline = project.active();

    name_ = QString::fromStdString(timeline.name);
    // Rational -> double happens here, at the presentation boundary, and
    // nowhere else. The host keeps exact rational time; the UI works in
    // seconds. Converting earlier would reintroduce frame drift.
    duration_ = timeline.duration().as_double();
    frameRate_ = timeline.video.rate();

    // A timeline's clips are stored in insertion order, not time order; the
    // strip draws left to right, so each lane is sorted by start here.
    QVariantList track_list;
    for (const genesis::project::Track &track : timeline.tracks) {
        std::vector<const genesis::project::Clip *> lane;
        for (const genesis::project::Clip &clip : timeline.clips) {
            if (clip.track_id == track.id) {
                lane.push_back(&clip);
            }
        }
        // These pointers are used and deep-copied below, within this call
        // only: nothing outlives the project reference this method was handed.
        std::sort(lane.begin(), lane.end(),
                  [](const genesis::project::Clip *a, const genesis::project::Clip *b) {
                      return a->start < b->start;
                  });

        QVariantList clip_list;
        for (const genesis::project::Clip *clip : lane) {
            QVariantMap c;
            c.insert(QStringLiteral("id"), QString::fromStdString(clip->id));
            c.insert(QStringLiteral("start"), clip->start.as_double());
            c.insert(QStringLiteral("duration"), clip->duration.as_double());
            // The clip's in-point, so a brush stroke can turn the playhead
            // into the source instant a smart brush reads pixels from.
            c.insert(QStringLiteral("source_start"), clip->source_start.as_double());
            c.insert(QStringLiteral("name"), QString::fromStdString(clip->name));
            c.insert(QStringLiteral("kind"), QString::fromLatin1(kind_token(clip->kind)));
            // The bin item the clip cuts from, so the strip can light a clip
            // when its media is selected in the bin (empty for text/layer).
            c.insert(QStringLiteral("media_id"), QString::fromStdString(clip->media_id));
            // The inspector's fields, exposed read-only so the pane displays
            // what the model holds and writes back through the EditController.
            c.insert(QStringLiteral("scale"), clip->scale);
            c.insert(QStringLiteral("offset_x"), clip->offset_x);
            c.insert(QStringLiteral("offset_y"), clip->offset_y);
            c.insert(QStringLiteral("rotation"), clip->rotation);
            c.insert(QStringLiteral("stretch_x"), clip->stretch_x);
            c.insert(QStringLiteral("stretch_y"), clip->stretch_y);
            c.insert(QStringLiteral("opacity"), clip->opacity);
            c.insert(QStringLiteral("speed"), clip->speed);
            c.insert(QStringLiteral("reverse"), clip->reverse);
            c.insert(QStringLiteral("fade_in"), clip->fade_in.as_double());
            c.insert(QStringLiteral("fade_out"), clip->fade_out.as_double());
            c.insert(QStringLiteral("pan"), clip->pan);
            clip_list.append(c);
        }

        QVariantMap t;
        t.insert(QStringLiteral("id"), QString::fromStdString(track.id));
        t.insert(QStringLiteral("visible"), track.visible);
        t.insert(QStringLiteral("muted"), track.muted);
        t.insert(QStringLiteral("clips"), clip_list);
        track_list.append(t);
    }
    tracks_ = track_list;

    // The bin's contents, one map per imported file. Missing-ness is derived
    // once here from the host's filesystem check, not re-statted per card.
    const std::set<std::string> missing_ids = [&project]() {
        std::set<std::string> ids;
        for (const genesis::project::MissingMedia &missing : project.missing_media()) {
            ids.insert(missing.id);
        }
        return ids;
    }();
    QVariantList media_list;
    for (const genesis::project::MediaItem &item : project.media) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), QString::fromStdString(item.id));
        m.insert(QStringLiteral("path"), QString::fromStdString(item.path));
        m.insert(QStringLiteral("name"), QString::fromStdString(item.name));
        m.insert(QStringLiteral("kind"), QString::fromLatin1(media_kind_token(item.kind)));
        m.insert(QStringLiteral("duration"), item.duration ? item.duration->as_double() : -1.0);
        m.insert(QStringLiteral("width"), item.width ? static_cast<double>(*item.width) : -1.0);
        m.insert(QStringLiteral("height"), item.height ? static_cast<double>(*item.height) : -1.0);
        m.insert(QStringLiteral("frame_rate"), item.frame_rate ? *item.frame_rate : -1.0);
        m.insert(QStringLiteral("frame_rate_fraction"),
                 item.frame_rate_fraction ? QString::fromStdString(*item.frame_rate_fraction)
                                          : QString());
        m.insert(QStringLiteral("video_codec"),
                 item.video_codec ? QString::fromStdString(*item.video_codec) : QString());
        m.insert(QStringLiteral("audio_codec"),
                 item.audio_codec ? QString::fromStdString(*item.audio_codec) : QString());
        m.insert(QStringLiteral("has_audio"), item.has_audio);
        m.insert(QStringLiteral("missing"), missing_ids.count(item.id) != 0);
        media_list.append(m);
    }
    media_ = media_list;
}

QString TimelineModel::fmt(double seconds) const
{
    if (!std::isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }
    const double rate = frameRate_ > 0.0 ? frameRate_ : 30.0;
    const std::int64_t whole = static_cast<std::int64_t>(std::floor(seconds));
    std::int64_t minutes = whole / 60;
    std::int64_t secs = whole % 60;
    std::int64_t frames = static_cast<std::int64_t>(
            std::floor((seconds - static_cast<double>(whole)) * rate + 0.5));
    if (frames >= static_cast<std::int64_t>(rate)) {
        frames = 0;
        if (++secs >= 60) {
            secs = 0;
            ++minutes;
        }
    }
    return QStringLiteral("%1:%2.%3")
            .arg(static_cast<qlonglong>(minutes), 2, 10, QLatin1Char('0'))
            .arg(static_cast<qlonglong>(secs), 2, 10, QLatin1Char('0'))
            .arg(static_cast<qlonglong>(frames), 2, 10, QLatin1Char('0'));
}
