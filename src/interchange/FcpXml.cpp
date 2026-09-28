// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "interchange/FcpXml.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/ClipTimeline.h"
#include "core/XmlRead.h"
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

using genesis::core::Rational;
using genesis::project::Clip;
using genesis::project::ClipKind;
using genesis::project::MediaItem;
using genesis::project::MediaKind;
using genesis::project::Project;
using genesis::project::Timeline;
using genesis::project::Track;
using genesis::project::VideoSettings;

std::string xml_escape(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += character;
            break;
        }
    }
    return out;
}

// FCPXML writes durations as rational seconds: "1/30s", and a whole second as
// "2s". Keeping the Rational means a rate like 30000/1001 stays exact.
std::string fcpx_time(Rational value)
{
    if (value.denominator() == 1) {
        return std::to_string(value.numerator()) + "s";
    }
    return std::to_string(value.numerator()) + "/" + std::to_string(value.denominator()) + "s";
}

// A clip can be written as a spine item only when a file backs it: FCPXML has
// no element for "a clip with no media", and inventing one would be a lie.
bool has_media(const Project &project, const Clip &clip)
{
    const MediaItem *media = media_by_id(project, clip.media_id);
    return media != nullptr && !media->path.empty();
}

std::vector<const Clip *> pictures(const Timeline &timeline)
{
    std::vector<const Clip *> items;
    for (const Clip &clip : timeline.clips) {
        if (clip.kind != ClipKind::Audio) {
            items.push_back(&clip);
        }
    }
    std::sort(items.begin(), items.end(), [](const Clip *left, const Clip *right) {
        return left->start.as_double() < right->start.as_double();
    });
    return items;
}

// Audio clips ride above the picture spine rather than inside it, so they are
// collected separately and written after the pictures as connected clips.
std::vector<const Clip *> sounds(const Timeline &timeline)
{
    std::vector<const Clip *> items;
    for (const Clip &clip : timeline.clips) {
        if (clip.kind == ClipKind::Audio) {
            items.push_back(&clip);
        }
    }
    std::sort(items.begin(), items.end(), [](const Clip *left, const Clip *right) {
        return left->start.as_double() < right->start.as_double();
    });
    return items;
}

// The asset's length: the probe's duration when it has one, else the furthest
// point any clip reads from it.
Rational asset_duration(const Timeline &timeline, const MediaItem &media)
{
    if (media.duration) {
        return *media.duration;
    }
    Rational longest = Rational::ZERO;
    for (const Clip &clip : timeline.clips) {
        if (clip.media_id != media.id) {
            continue;
        }
        const Rational end = clip.source_start + clip.duration;
        if (end.as_double() > longest.as_double()) {
            longest = end;
        }
    }
    return longest;
}

} // namespace

std::string to_fcpxml(const Project &project)
{
    const Timeline *timeline = active_timeline(project);
    if (timeline == nullptr) {
        return { };
    }
    const VideoSettings &video = timeline->video;
    const std::vector<const Clip *> spine = pictures(*timeline);
    const std::vector<const Clip *> audio = sounds(*timeline);

    // The assets the spine actually references, in first-use order, so the ids
    // are stable and no unused resource is declared. The pictures come first
    // and the audio media after them, so the picture ids never shift when an
    // audio clip is added or removed.
    std::vector<const MediaItem *> assets;
    const auto collect = [&](const std::vector<const Clip *> &clips) {
        for (const Clip *clip : clips) {
            const MediaItem *media = media_by_id(project, clip->media_id);
            if (media == nullptr || media->path.empty()) {
                continue;
            }
            if (std::find(assets.begin(), assets.end(), media) == assets.end()) {
                assets.push_back(media);
            }
        }
    };
    collect(spine);
    collect(audio);

    Rational duration = Rational::ZERO;
    for (const Clip *clip : spine) {
        const Rational end = clip->start + clip->duration;
        if (end.as_double() > duration.as_double()) {
            duration = end;
        }
    }

    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE fcpxml>\n";
    out += "<fcpxml version=\"1.9\">\n";
    out += "  <resources>\n";
    out += "    <format id=\"r1\" frameDuration=\""
            + fcpx_time(Rational(video.rate_den, video.rate_num)) + "\" width=\""
            + std::to_string(video.width) + "\" height=\"" + std::to_string(video.height)
            + "\"/>\n";
    for (std::size_t index = 0; index < assets.size(); ++index) {
        const MediaItem &media = *assets[index];
        const bool has_audio = media.kind == genesis::project::MediaKind::Audio || media.has_audio;
        out += "    <asset id=\"r" + std::to_string(index + 2) + "\" name=\""
                + xml_escape(media.name.empty() ? media.path : media.name)
                + "\" start=\"0s\" duration=\"" + fcpx_time(asset_duration(*timeline, media))
                + "\" hasVideo=\"" + (media.kind == genesis::project::MediaKind::Audio ? "0" : "1")
                + "\" hasAudio=\"" + (has_audio ? "1" : "0") + "\" format=\"r1\">\n";
        out += "      <media-rep kind=\"original-media\" src=\"file://" + xml_escape(media.path)
                + "\"/>\n";
        out += "    </asset>\n";
    }
    out += "  </resources>\n";
    out += "  <library>\n";
    out += "    <event name=\"Genesis\">\n";
    out += "      <project name=\"" + xml_escape(timeline->name) + "\">\n";
    out += "        <sequence format=\"r1\" duration=\"" + fcpx_time(duration)
            + "\" tcStart=\"0s\" tcFormat=\"NDF\">\n";
    out += "          <spine>\n";

    Rational cursor = Rational::ZERO;
    for (const Clip *clip : spine) {
        // Anything the model cannot express as a spine item keeps its time as
        // a gap, so the cut lands at the right place even when the element is
        // dropped.
        if (!has_media(project, *clip)) {
            continue;
        }
        if (clip->start.as_double() > cursor.as_double()) {
            const Rational gap = clip->start - cursor;
            out += "            <gap offset=\"" + fcpx_time(cursor) + "\" duration=\""
                    + fcpx_time(gap) + "\" start=\"0s\"/>\n";
        }
        const MediaItem *media = media_by_id(project, clip->media_id);
        const auto reference = std::find(assets.begin(), assets.end(), media) - assets.begin();
        const std::string id = "r" + std::to_string(reference + 2);
        out += "            <clip offset=\"" + fcpx_time(clip->start) + "\" duration=\""
                + fcpx_time(clip->duration) + "\" start=\"" + fcpx_time(clip->source_start)
                + "\" name=\"" + xml_escape(clip->name) + "\">\n";
        out += "              <video ref=\"" + id + "\" offset=\"0s\" duration=\""
                + fcpx_time(clip->duration) + "\" start=\"" + fcpx_time(clip->source_start)
                + "\"/>\n";
        out += "            </clip>\n";
        cursor = clip->start + clip->duration;
    }

    // Audio is written as connected clips after the pictures. `lane="1"` is
    // FCPXML's signal that a clip sits above (connected to) the primary
    // storyline, so the sound runs parallel to the picture instead of being
    // appended after it on the storyline itself.
    for (const Clip *clip : audio) {
        if (!has_media(project, *clip)) {
            continue;
        }
        const MediaItem *media = media_by_id(project, clip->media_id);
        const auto reference = std::find(assets.begin(), assets.end(), media) - assets.begin();
        const std::string id = "r" + std::to_string(reference + 2);
        out += "            <asset-clip ref=\"" + id + "\" lane=\"1\" offset=\""
                + fcpx_time(clip->start) + "\" duration=\"" + fcpx_time(clip->duration)
                + "\" start=\"" + fcpx_time(clip->source_start) + "\" name=\""
                + xml_escape(clip->name) + "\"/>\n";
    }

    out += "          </spine>\n";
    out += "        </sequence>\n";
    out += "      </project>\n";
    out += "    </event>\n";
    out += "  </library>\n";
    out += "</fcpxml>\n";
    return out;
}

namespace {

using genesis::core::Rational;
using genesis::core::XmlNode;

// FCPXML writes times as a rational number of seconds: "2s", "1/30s", or a
// bare decimal "1.5s". `Rational::parse` handles a bare integer/decimal but not
// the trailing 's', so the suffix is stripped first; a "num/den" pair is read
// exactly rather than through a double.
std::optional<Rational> parse_fcpx_time(std::string_view text)
{
    if (!text.empty() && text.back() == 's') {
        text.remove_suffix(1);
    }
    const std::size_t slash = text.find('/');
    if (slash != std::string_view::npos) {
        const std::string_view num = text.substr(0, slash);
        const std::string_view den = text.substr(slash + 1);
        const std::optional<Rational> numerator = Rational::parse(num);
        const std::optional<Rational> denominator = Rational::parse(den);
        if (!numerator || !denominator || denominator->is_zero() || numerator->numerator() <= 0
            || denominator->numerator() <= 0) {
            return std::nullopt;
        }
        return *numerator / *denominator;
    }
    // A bare integer/decimal number of seconds becomes a rational via the
    // nearest-microsecond seam, so "1.5s" is 3/2 exactly.
    if (const std::optional<Rational> exact = Rational::parse(text)) {
        return exact;
    }
    return std::nullopt;
}

// An unsigned decimal integer, or nullopt on anything else. Width and height
// come from a foreign file, so a non-numeric value is skipped rather than
// throwing (std::stoul throws on garbage).
std::optional<std::uint64_t> parse_unsigned(std::string_view text)
{
    std::uint64_t value = 0;
    if (text.empty()) {
        return std::nullopt;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

// The frame rate a frameDuration value declares: "1001/30000s" names one frame
// of 1001/30000 seconds, i.e. 30000/1001 frames per second.
bool rate_from_frame_duration(std::string_view text, std::int64_t *num, std::int64_t *den)
{
    const std::optional<Rational> frame = parse_fcpx_time(text);
    if (!frame || frame->is_zero()) {
        return false;
    }
    const Rational rate = frame->recip();
    *num = rate.numerator();
    *den = rate.denominator();
    return true;
}

// Percent-decodes "%XX" escapes in place, returning false on a malformed
// escape. file:// URLs escape spaces and non-ASCII bytes this way.
bool percent_decode(std::string &path)
{
    std::string out;
    out.reserve(path.size());
    for (std::size_t index = 0; index < path.size(); ++index) {
        const char c = path[index];
        if (c != '%') {
            out += c;
            continue;
        }
        if (index + 2 >= path.size()) {
            return false;
        }
        const auto hex = [](char digit) -> int {
            if (digit >= '0' && digit <= '9') {
                return digit - '0';
            }
            if (digit >= 'a' && digit <= 'f') {
                return digit - 'a' + 10;
            }
            if (digit >= 'A' && digit <= 'F') {
                return digit - 'A' + 10;
            }
            return -1;
        };
        const int high = hex(path[index + 1]);
        const int low = hex(path[index + 2]);
        if (high < 0 || low < 0) {
            return false;
        }
        out += static_cast<char>((high << 4) | low);
        index += 2;
    }
    path = std::move(out);
    return true;
}

// The absolute path a media-rep's src names: strips the file:// scheme,
// percent-decodes, and resolves a relative path against the document's
// directory.
std::string resolve_src(std::string src, const std::filesystem::path &source_path)
{
    if (src.rfind("file://", 0) == 0) {
        src = src.substr(7);
    }
    percent_decode(src);
    const std::filesystem::path path(src);
    if (src.empty() || src.front() == '/' || path.is_absolute() || source_path.empty()) {
        return path.string();
    }
    return (source_path.parent_path() / path).string();
}

} // namespace

std::optional<project::Project> from_fcpxml(std::string_view xml,
                                            const std::filesystem::path &source_path,
                                            const project::VideoSettings &video, std::string &error)
{
    std::optional<XmlNode> tree = genesis::core::parse_xml(xml, error);
    if (!tree) {
        return std::nullopt;
    }
    const XmlNode *root = tree->child("fcpxml");
    if (root == nullptr) {
        error = "the document is not an FCPXML file";
        return std::nullopt;
    }

    // The frame the timeline renders to: start from the caller's (a sequence
    // that declares no rate has to come from somewhere), then take the
    // frameDuration, width and height the format resource carries.
    VideoSettings output = video;
    std::unordered_map<std::string, const XmlNode *> formats;
    // The asset id -> index into `media`. Indices, not pointers, because
    // `media` is moved into the project before the spine is read.
    std::unordered_map<std::string, std::size_t> assets;
    std::vector<MediaItem> media;

    if (const XmlNode *resources = root->child("resources")) {
        for (const XmlNode &format : resources->children) {
            if (format.name != "format") {
                continue;
            }
            const std::string *id = format.attribute("id");
            if (id != nullptr) {
                formats[*id] = &format;
            }
        }
    }

    // The sequence names its format; the first format is the one to read the
    // frame from when the sequence does not. Read it before the assets so the
    // rate is known while they are built, even though only the frame is used.
    const XmlNode *sequence = nullptr;
    std::string project_name;
    if (const XmlNode *library = root->child("library")) {
        for (const XmlNode &event : library->children) {
            if (event.name != "event") {
                continue;
            }
            for (const XmlNode &project : event.children) {
                if (project.name != "project") {
                    continue;
                }
                if (const std::string *name = project.attribute("name")) {
                    project_name = *name;
                }
                for (const XmlNode &candidate : project.children) {
                    if (candidate.name == "sequence") {
                        sequence = &candidate;
                    }
                }
            }
        }
    }

    const XmlNode *format_node = nullptr;
    if (sequence != nullptr) {
        if (const std::string *ref = sequence->attribute("format")) {
            if (const auto found = formats.find(*ref); found != formats.end()) {
                format_node = found->second;
            }
        }
    }
    if (format_node == nullptr && !formats.empty()) {
        format_node = formats.begin()->second;
    }
    if (format_node != nullptr) {
        if (const std::string *frame_duration = format_node->attribute("frameDuration")) {
            std::int64_t num = 0;
            std::int64_t den = 0;
            if (rate_from_frame_duration(*frame_duration, &num, &den)) {
                output.rate_num = num;
                output.rate_den = den;
            }
        }
        if (const std::string *width = format_node->attribute("width")) {
            if (const std::optional<std::uint64_t> value = parse_unsigned(*width)) {
                output.width = static_cast<std::uint32_t>(*value);
            }
        }
        if (const std::string *height = format_node->attribute("height")) {
            if (const std::optional<std::uint64_t> value = parse_unsigned(*height)) {
                output.height = static_cast<std::uint32_t>(*value);
            }
        }
    }

    // Assets become the bin. The map is by the asset's id, so a clip's
    // `<video ref="...">` or `<asset-clip ref="...">` names a media item
    // directly.
    if (const XmlNode *resources = root->child("resources")) {
        for (const XmlNode &asset : resources->children) {
            if (asset.name != "asset") {
                continue;
            }
            const std::string *id = asset.attribute("id");
            const std::string *has_video = asset.attribute("hasVideo");
            const std::string *has_audio = asset.attribute("hasAudio");
            const bool video_asset = has_video != nullptr && *has_video == "1";
            const bool audio_asset = has_audio != nullptr && *has_audio == "1";

            MediaItem item;
            item.id = "m" + std::to_string(media.size() + 1);
            if (const std::string *name = asset.attribute("name")) {
                item.name = *name;
            }
            if (const std::string *duration = asset.attribute("duration")) {
                item.duration = parse_fcpx_time(*duration);
            }
            if (const XmlNode *rep = asset.child("media-rep")) {
                if (const std::string *src = rep->attribute("src")) {
                    item.path = resolve_src(*src, source_path);
                }
            }
            item.has_audio = audio_asset;
            // FCPXML flags an audio-only asset as hasVideo="0" hasAudio="1";
            // anything else is treated as video.
            item.kind = audio_asset && !video_asset ? MediaKind::Audio : MediaKind::Video;
            if (item.name.empty()) {
                item.name = basename_of(item.path);
            }

            const std::string asset_id = id != nullptr ? *id : std::string();
            const std::size_t index = media.size();
            media.push_back(std::move(item));
            if (!asset_id.empty()) {
                assets[asset_id] = index;
            }
        }
    }

    // The spine is one lane: FCPXML ordering is linear, so every clip lands on
    // a single track rather than being fanned across lanes.
    Project project;
    project.media = std::move(media);
    project.timelines.clear();

    Timeline timeline;
    timeline.id = "TL1";
    timeline.name = project_name;
    timeline.video = output;

    Track track;
    track.id = "T1";
    timeline.tracks.push_back(track);

    if (sequence != nullptr) {
        if (const XmlNode *spine = sequence->child("spine")) {
            for (const XmlNode &item : spine->children) {
                if (item.name == "gap") {
                    // A gap is empty time; each clip carries its own offset, so
                    // no cursor is needed and nothing is emitted.
                    continue;
                }
                // Final Cut and Premiere write file-backed items as
                // `<asset-clip ref>`; `<clip>` with a nested `<video>` is this
                // project's own shape. Both name a clip on the spine.
                const bool is_asset_clip = item.name == "asset-clip";
                if (item.name != "clip" && !is_asset_clip) {
                    continue;
                }

                Clip clip;
                clip.id = "c" + std::to_string(timeline.clips.size() + 1);
                clip.track_id = track.id;
                if (const std::string *offset = item.attribute("offset")) {
                    clip.start = parse_fcpx_time(*offset).value_or(Rational::ZERO);
                }
                if (const std::string *duration = item.attribute("duration")) {
                    clip.duration = parse_fcpx_time(*duration).value_or(Rational::ONE);
                }
                if (const std::string *start = item.attribute("start")) {
                    clip.source_start = parse_fcpx_time(*start).value_or(Rational::ZERO);
                }
                if (const std::string *name = item.attribute("name")) {
                    clip.name = *name;
                }

                // The reference names the asset: `ref` on an `<asset-clip>`,
                // a `<video ref>` child on a `<clip>`, or an `<asset-clip ref>`
                // child. An unknown ref leaves `media_id` empty; the settle
                // pass then drops the clip, which is the honest outcome for a
                // file that references media it never declares.
                const std::string *ref = nullptr;
                if (is_asset_clip) {
                    ref = item.attribute("ref");
                }
                if (ref == nullptr) {
                    if (const XmlNode *video_node = item.child("video")) {
                        ref = video_node->attribute("ref");
                    }
                }
                if (ref == nullptr) {
                    if (const XmlNode *asset_clip = item.child("asset-clip")) {
                        ref = asset_clip->attribute("ref");
                    }
                }
                if (ref != nullptr) {
                    if (const auto found = assets.find(*ref); found != assets.end()) {
                        const MediaItem &source = project.media[found->second];
                        clip.media_id = source.id;
                        clip.kind =
                                source.kind == MediaKind::Audio ? ClipKind::Audio : ClipKind::Video;
                    }
                }
                timeline.clips.push_back(std::move(clip));
            }
        }
    }

    if (timeline.name.empty()) {
        timeline.name = "Timeline";
    }
    const bool spine_named_clips = !timeline.clips.empty();
    project.timelines.push_back(std::move(timeline));
    project.active_timeline_id = "TL1";

    // Settle through the document layer, so an imported file is held to the
    // same invariants as a document read - which drops a clip that referenced
    // unknown media.
    std::optional<project::Project> readable = settle_project(std::move(project));
    if (!readable) {
        error = "the imported FCPXML does not settle into a project";
        return std::nullopt;
    }
    // A spine that named clips but settled to none means every reference was
    // unknown. Reporting success would look like opening a blank cut, so this
    // fails loudly instead.
    if (spine_named_clips && readable->timelines.front().clips.empty()) {
        error = "the spine's clips name no asset this reader recognises";
        return std::nullopt;
    }
    return readable;
}

} // namespace genesis::interchange
