// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "interchange/Cmx3600.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "interchange/ProjectModel.h"
#include "project/Document.h"
#include "project/Project.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"
#include "project/model/Track.h"
#include "project/model/VideoSettings.h"

namespace genesis::interchange {

namespace {

using genesis::project::Clip;
using genesis::project::ClipKind;
using genesis::project::MediaItem;
using genesis::project::MediaKind;
using genesis::project::Project;
using genesis::project::Rational;
using genesis::project::Timeline;
using genesis::project::Track;
using genesis::project::VideoSettings;

// Whole frames, non-drop: the only frame field a CMX-3600 record has. A
// fractional rate uses its rounded frame count.
std::string timecode(double seconds, double rate)
{
    const long long per_second = std::max<long long>(1, std::llround(rate));
    const long long whole = std::max<long long>(0, std::llround(seconds * per_second));
    const long long total_seconds = whole / per_second;
    char text[64];
    std::snprintf(text, sizeof text, "%02lld:%02lld:%02lld:%02lld", total_seconds / 3600,
                  (total_seconds / 60) % 60, total_seconds % 60, whole % per_second);
    return text;
}

// Splits `line` on runs of spaces and tabs. Consecutive separators collapse, so
// both the exporter's double-spaced padding and a single-space separator read.
std::vector<std::string_view> fields_of(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
            ++index;
        }
        const std::size_t begin = index;
        while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
            ++index;
        }
        if (index > begin) {
            fields.push_back(line.substr(begin, index - begin));
        }
    }
    return fields;
}

// Parses "HH:MM:SS:FF" into a whole-frame count at `per_second` frames, or
// nullopt when the four fields are not present or not unsigned integers.
std::optional<long long> parse_timecode(std::string_view text, long long per_second,
                                        bool drop_frame)
{
    std::size_t first = 0;
    long long parts[4] = { };
    // HH:MM:SS:FF has three separators; the frame field runs to the end.
    for (std::size_t index = 0; index < 3; ++index) {
        const std::size_t colon = text.find(':', first);
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view field = text.substr(first, colon - first);
        if (field.empty()) {
            return std::nullopt;
        }
        char *end = nullptr;
        const long long value = std::strtoll(std::string(field).c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || value < 0) {
            return std::nullopt;
        }
        parts[index] = value;
        first = colon + 1;
    }
    // The final field runs to the end of the token; it must itself be numeric.
    const std::string_view tail = text.substr(first);
    if (tail.empty() || tail.find(':') != std::string_view::npos) {
        return std::nullopt;
    }
    char *end = nullptr;
    const long long value = std::strtoll(std::string(tail).c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || value < 0) {
        return std::nullopt;
    }
    parts[3] = value;

    long long frames = ((parts[0] * 60 + parts[1]) * 60 + parts[2]) * per_second + parts[3];
    if (drop_frame) {
        // Drop-frame numbering skips `drop` frame labels at every minute except
        // each tenth, so the label count runs ahead of the real frame count.
        // Removing the skipped labels converts a drop-frame timecode to frames.
        const long long drop = std::llround(static_cast<double>(per_second) * 0.066666);
        if (drop > 0) {
            const long long total_minutes = parts[0] * 60 + parts[1];
            frames -= drop * (total_minutes - total_minutes / 10);
        }
    }
    return frames;
}

} // namespace

std::string to_cmx3600(const Project &project)
{
    const Timeline *timeline = active_timeline(project);
    if (timeline == nullptr) {
        return { };
    }
    const double rate = timeline->video.rate();

    // A CMX-3600 event is a picture or a sound: it names one reel's record
    // in and out. A title or a layer has no reel behind it, so it is not an
    // EDL event at all.
    std::vector<const Clip *> events;
    for (const Clip &clip : timeline->clips) {
        if (clip.kind == ClipKind::Video || clip.kind == ClipKind::Audio) {
            events.push_back(&clip);
        }
    }
    // The event number is assigned in record order, so the union of video and
    // audio must be sorted by its record in: the file only reads
    // chronologically when its numbers and its timecodes agree. A clip that
    // starts at the same instant as another keeps video before audio, so the
    // picture column leads wherever the cut is simultaneous.
    std::sort(events.begin(), events.end(), [](const Clip *left, const Clip *right) {
        if (left->start != right->start) {
            return left->start < right->start;
        }
        return left->kind < right->kind;
    });

    std::string text =
            "TITLE: " + (timeline->name.empty() ? std::string("Timeline") : timeline->name);
    text += "\nFCM: NON-DROP FRAME\n\n";

    int number = 0;
    for (const Clip *clip : events) {
        const double start = clip->start.as_double();
        const double duration = clip->duration.as_double();
        const double source = clip->source_start.as_double();
        char event[16];
        std::snprintf(event, sizeof event, "%03d", ++number);
        text += event;
        text += "  AX       ";
        // The track column: 'V' for a picture, 'A' for sound. The same four
        // timecodes follow either way - an audio event is just a video event
        // on the audio track column.
        text += clip->kind == ClipKind::Audio ? 'A' : 'V';
        text += "     C        ";
        text += timecode(source, rate);
        text += ' ';
        text += timecode(source + duration, rate);
        text += ' ';
        text += timecode(start, rate);
        text += ' ';
        text += timecode(start + duration, rate);
        text += '\n';
    }
    return text;
}

std::optional<Project> from_cmx3600(std::string_view source, const VideoSettings &video,
                                    std::string &error)
{
    // Strip a UTF-8 BOM and normalise CRLF so a Windows-authored file reads the
    // same as a Unix one; the byte order mark would otherwise become the first
    // line's first character and break both the TITLE and event parsing.
    std::string text(source);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            normalized += '\n';
            ++i;
        } else if (text[i] == '\r') {
            normalized += '\n';
        } else {
            normalized += text[i];
        }
    }

    // An EDL counts frames against the rate the caller says the file was cut
    // at. The frame field is whole frames even when that rate is fractional,
    // so each timecode becomes an exact Rational of the form frames / per_second
    // seconds with no floating-point round trip.
    const long long per_second = std::max<long long>(1, std::llround(video.rate()));
    bool drop_frame = false;
    // A drop-frame timecode names real time (that is what it exists for), so its
    // frame count divides by the true rate; a non-drop timecode is a label grid,
    // so it divides by the nominal frame count. Both stay exact Rationals.
    const Rational true_rate{ video.rate_num, video.rate_den };
    const Rational nominal_rate{ per_second, 1 };
    const auto frames_to_seconds = [&](long long frames) {
        return Rational::from_int(frames) / (drop_frame ? true_rate : nominal_rate);
    };

    Timeline timeline;
    timeline.id = "TL1";
    timeline.name = "Timeline";
    timeline.video = video;
    Project project(video);
    project.timelines.clear();
    project.media.clear();

    // Track column -> track id. The spec names V and A, but a W or K column is
    // tolerated: it lands on the same video track as V rather than being
    // dropped, so a dissolve or keyed event still carries its picture.
    auto track_for = [&timeline](ClipKind kind) -> std::string {
        const bool audio = kind == ClipKind::Audio;
        const std::string id = audio ? "T2" : "T1";
        const auto found = std::find_if(timeline.tracks.begin(), timeline.tracks.end(),
                                        [&id](const Track &track) { return track.id == id; });
        if (found == timeline.tracks.end()) {
            Track track;
            track.id = id;
            timeline.tracks.push_back(std::move(track));
        }
        return id;
    };

    // Reel+kind -> media id, so a reel shared by several events of the same
    // kind is one media item (and the user relinks once). The kind is part of
    // the key because a reel can legitimately appear on both the V and A
    // columns; keying on the name alone would point the sound at a picture
    // asset (or the reverse).
    std::map<std::pair<std::string, ClipKind>, std::string> reel_media;
    const auto media_for = [&](const std::string &reel, ClipKind kind) -> std::string {
        const auto existing = reel_media.find({ reel, kind });
        if (existing != reel_media.end()) {
            return existing->second;
        }
        MediaItem media;
        media.id = "m" + std::to_string(project.media.size() + 1);
        media.name = reel;
        media.path = reel;
        media.kind = kind == ClipKind::Audio ? MediaKind::Audio : MediaKind::Video;
        const std::string id = media.id;
        project.media.push_back(std::move(media));
        reel_media.emplace(std::pair{ reel, kind }, id);
        return id;
    };

    std::size_t line_begin = 0;
    int line_number = 0;
    int event_count = 0;
    while (line_begin <= normalized.size()) {
        const std::size_t newline = normalized.find('\n', line_begin);
        const std::size_t line_end = newline == std::string::npos ? normalized.size() : newline;
        const std::string_view line(normalized.data() + line_begin, line_end - line_begin);
        ++line_number;

        // A line whose first non-space character is '*' or ';' is a comment.
        const std::size_t first_nonspace = line.find_first_not_of(" \t");
        if (first_nonspace == std::string_view::npos) {
            line_begin = line_end + 1;
            continue;
        }
        const char lead = line[first_nonspace];
        if (lead == '*' || lead == ';') {
            line_begin = line_end + 1;
            continue;
        }

        // The exporter writes "TITLE: Name"; other editors write "TITLE Name",
        // so the colon is optional.
        if (line.substr(0, 5) == "TITLE") {
            std::string_view name = line.substr(5);
            const std::size_t name_begin = name.find_first_not_of(" :\t");
            if (name_begin != std::string_view::npos) {
                name = name.substr(name_begin);
                const std::size_t name_end = name.find_last_not_of(" \t");
                name = name.substr(0, name_end + 1);
                if (!name.empty()) {
                    timeline.name = std::string(name);
                }
            }
            line_begin = line_end + 1;
            continue;
        }

        // FCM is a format declaration. Drop-frame numbering skips labels at
        // every minute but the tenth, so the reader has to know which it is
        // before it can turn a timecode into frames. "NON-DROP" contains
        // "DROP", so it is tested first.
        if (line.substr(0, 3) == "FCM") {
            if (line.find("NON-DROP") == std::string_view::npos
                && line.find("DROP") != std::string_view::npos) {
                drop_frame = true;
            }
            line_begin = line_end + 1;
            continue;
        }

        const std::string trimmed(line.substr(first_nonspace, line_end - first_nonspace));
        const std::vector<std::string_view> fields = fields_of(trimmed);
        if (fields.empty()) {
            line_begin = line_end + 1;
            continue;
        }

        // An event has at least eight whitespace-separated fields:
        // NNN REEL TRACK TRANS SRCIN SRCOUT RECIN RECOUT. A dissolve inserts
        // its duration in frames between TRANS and SRCIN, which is why the
        // timecode columns are read through `shift` rather than fixed.
        if (fields.size() < 8) {
            error = "line " + std::to_string(line_number) + " is not an EDL event";
            return std::nullopt;
        }

        const std::string reel(fields[1]);
        const char track_column = fields[2].empty() ? ' ' : fields[2][0];
        const ClipKind kind = track_column == 'A' ? ClipKind::Audio : ClipKind::Video;

        std::string transition_column(fields[3]);
        for (char &character : transition_column) {
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        }
        std::size_t shift = 0;
        long long dissolve_frames = 0;
        if (!transition_column.empty() && transition_column.front() == 'D' && fields.size() >= 9) {
            char *end = nullptr;
            const long long value = std::strtoll(std::string(fields[4]).c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && value > 0) {
                dissolve_frames = value;
                shift = 1;
            }
        }
        if (fields.size() < 8 + shift) {
            error = "line " + std::to_string(line_number) + " is not an EDL event";
            return std::nullopt;
        }

        std::optional<long long> source_in =
                parse_timecode(fields[4 + shift], per_second, drop_frame);
        std::optional<long long> source_out =
                parse_timecode(fields[5 + shift], per_second, drop_frame);
        std::optional<long long> record_in =
                parse_timecode(fields[6 + shift], per_second, drop_frame);
        std::optional<long long> record_out =
                parse_timecode(fields[7 + shift], per_second, drop_frame);
        if (!source_in || !source_out || !record_in || !record_out) {
            error = "line " + std::to_string(line_number) + " has an unreadable timecode";
            return std::nullopt;
        }

        Clip clip;
        clip.id = "c" + std::to_string(++event_count);
        clip.track_id = track_for(kind);
        clip.media_id = media_for(reel, kind);
        clip.name = reel.empty() ? "Clip " + std::to_string(event_count) : reel;
        clip.kind = kind;
        clip.start = frames_to_seconds(*record_in);
        clip.duration = std::max(frames_to_seconds(*record_out - *record_in), Rational::ZERO);
        clip.source_start = frames_to_seconds(*source_in);
        // A dissolve column is the cut-in of this event, so it maps to the
        // clip's transition-in exactly like an OTIO Transition.1 does.
        if (dissolve_frames > 0) {
            clip.transition_in = genesis::project::Transition{ "cross-fade",
                                                               frames_to_seconds(dissolve_frames) };
        }

        timeline.clips.push_back(std::move(clip));

        if (newline == std::string::npos) {
            break;
        }
        line_begin = newline + 1;
    }

    if (event_count == 0) {
        error = "the file holds no EDL events";
        return std::nullopt;
    }

    project.timelines.push_back(std::move(timeline));
    project.active_timeline_id = "TL1";

    // Settle through the document layer, so an imported file is held to the
    // same invariants as a document read.
    std::optional<Project> readable = settle_project(std::move(project));
    if (!readable) {
        error = "the imported timeline does not settle into a project";
    }
    return readable;
}

} // namespace genesis::interchange
