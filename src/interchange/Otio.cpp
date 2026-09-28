// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "interchange/Otio.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <tuple>
#include <utility>

#include "core/ClipTimeline.h"
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

nlohmann::json rational_time(double seconds, double rate)
{
    return nlohmann::json{ { "OTIO_SCHEMA", "RationalTime.1" },
                           { "rate", rate },
                           { "value", std::round(seconds * rate) } };
}

nlohmann::json time_range(double start, double duration, double rate)
{
    return nlohmann::json{ { "OTIO_SCHEMA", "TimeRange.1" },
                           { "start_time", rational_time(start, rate) },
                           { "duration", rational_time(duration, rate) } };
}

// A lane is audio only when every clip on it is audio; an empty lane reads as
// video, which is what an editor expects of a fresh track.
bool track_is_audio(const Timeline &timeline, const Track &track)
{
    bool any = false;
    for (const Clip &clip : timeline.clips) {
        if (clip.track_id != track.id) {
            continue;
        }
        any = true;
        if (clip.kind != ClipKind::Audio) {
            return false;
        }
    }
    return any;
}

nlohmann::json clip_to_otio(const Clip &clip, const Project &project, double rate,
                            const std::map<std::string, nlohmann::json> &clip_documents)
{
    nlohmann::json reference{ { "OTIO_SCHEMA", "MissingReference.1" } };
    if (const MediaItem *media = media_by_id(project, clip.media_id);
        media != nullptr && !media->path.empty()) {
        reference = nlohmann::json{ { "OTIO_SCHEMA", "ExternalReference.1" },
                                    { "target_url", "file://" + media->path } };
    }
    nlohmann::json node{
        { "OTIO_SCHEMA", "Clip.2" },
        { "name", clip.name },
        { "source_range",
          time_range(clip.source_start.as_double(), clip.duration.as_double(), rate) },
        { "media_reference", std::move(reference) },
    };
    // What OTIO cannot model travels beside the clip it belongs to.
    if (const auto found = clip_documents.find(clip.id); found != clip_documents.end()) {
        node["metadata"] =
                nlohmann::json{ { "genesis", nlohmann::json{ { "clip", found->second } } } };
    }
    return node;
}

std::optional<double> time_of(const nlohmann::json &node)
{
    if (!node.is_object()) {
        return std::nullopt;
    }
    const auto rate = node.find("rate");
    const auto value = node.find("value");
    if (rate == node.end() || value == node.end() || !rate->is_number() || !value->is_number()) {
        return std::nullopt;
    }
    const double seconds_per_unit = rate->get<double>();
    if (seconds_per_unit <= 0.0) {
        return std::nullopt;
    }
    return value->get<double>() / seconds_per_unit;
}

std::string resolve_url(const std::string &url, const std::filesystem::path &source_path)
{
    std::string path = url;
    if (path.rfind("file://", 0) == 0) {
        path = path.substr(7);
    }
    // A relative target resolves against the document that referenced it.
    if (!path.empty() && path.front() != '/' && !source_path.empty()) {
        path = (source_path.parent_path() / path).string();
    }
    return path;
}

// The positive rate carried by the first matching RationalTime, if any.
std::optional<double> rate_of(const nlohmann::json &range, const char *key)
{
    if (!range.is_object()) {
        return std::nullopt;
    }
    const auto rational = range.find(key);
    if (rational == range.end() || !rational->is_object()) {
        return std::nullopt;
    }
    const auto rate = rational->find("rate");
    if (rate == rational->end() || !rate->is_number() || rate->get<double>() <= 0.0) {
        return std::nullopt;
    }
    return rate->get<double>();
}

// The rate the timeline was cut at, in preference order:
//   1. `timeline.global_start_time.rate` - the one rate OTIO attaches to the
//      timeline itself, so it is authoritative when present;
//   2. the first clip's `source_range.start_time.rate`, else its
//      `duration.rate` - files that omit `global_start_time` still carry the
//      rate on every rational, so the first clip names it (start_time first
//      because it is the rate the source was cut at, not merely how long it
//      was held; using the first hit keeps a stills-only or all-gap timeline
//      deterministic without failing on a mixed-rate stack);
//   3. the caller's fallback rate.
// A clip's own rates are read but never trusted over `global_start_time`, and
// disagreement between clips is not an error (OTIO permits per-rational
// rates).
double resolve_rate(const nlohmann::json &timeline, const nlohmann::json &tracks,
                    const VideoSettings &fallback)
{
    const auto start = timeline.find("global_start_time");
    if (start != timeline.end() && start->is_object()) {
        const auto rate = start->find("rate");
        if (rate != start->end() && rate->is_number() && rate->get<double>() > 0.0) {
            return rate->get<double>();
        }
    }
    for (const nlohmann::json &track : tracks) {
        const auto children = track.find("children");
        if (children == track.end() || !children->is_array()) {
            continue;
        }
        for (const nlohmann::json &clip : *children) {
            const auto range = clip.find("source_range");
            if (range == clip.end() || !range->is_object()) {
                continue;
            }
            if (const std::optional<double> rate = rate_of(*range, "start_time")) {
                return *rate;
            }
            if (const std::optional<double> rate = rate_of(*range, "duration")) {
                return *rate;
            }
        }
    }
    return fallback.rate();
}

void rate_fraction(double rate, Timeline &timeline)
{
    const std::optional<Rational> exact = Rational::approximate(rate);
    if (!exact) {
        return;
    }
    timeline.video.rate_num = exact->numerator();
    timeline.video.rate_den = exact->denominator();
}

// A foreign file has none of our metadata, so it is rebuilt in the model and
// then settled by the document layer - the same validation a document read
// gets, rather than a second set of rules here.
std::optional<Project> from_foreign_otio(const nlohmann::json &timeline,
                                         const std::filesystem::path &source_path,
                                         const VideoSettings &fallback, std::string &error)
{
    const auto tracks = timeline.find("tracks");
    if (tracks == timeline.end() || !tracks->is_object()) {
        error = "the timeline carries no tracks";
        return std::nullopt;
    }
    const auto children = tracks->find("children");
    if (children == tracks->end() || !children->is_array() || children->empty()) {
        error = "the timeline carries no tracks";
        return std::nullopt;
    }

    Project project;
    project.timelines.clear();
    project.media.clear();

    // A reference with no file has no path to dedupe on, so its identity is the
    // whole reference: schema, display name, generator kind and its metadata.
    // Keying on the name alone would merge two different slates that share a
    // label, and two unrelated lost files that happen to be unnamed.
    using ReferenceKey = std::tuple<std::string, std::string, std::string, std::string>;
    std::map<ReferenceKey, std::string> reference_media;

    Timeline imported;
    imported.id = "TL1";
    imported.name = timeline.value("name", std::string("Timeline"));
    // OTIO has no whole-timeline frame size, so the fallback's survives. The
    // rate is resolved per the rules above, then rounded to the rational the
    // model stores.
    imported.video.width = fallback.width;
    imported.video.height = fallback.height;
    rate_fraction(resolve_rate(timeline, *children, fallback), imported);

    std::size_t track_number = 0;
    for (const nlohmann::json &track_node : *children) {
        if (!track_node.is_object()) {
            continue;
        }
        Track track;
        track.id = "T" + std::to_string(++track_number);
        imported.tracks.push_back(track);

        const std::string track_kind = track_node.value("kind", std::string("Video"));
        const auto clips = track_node.find("children");
        if (clips == track_node.end() || !clips->is_array()) {
            continue;
        }
        double position = 0.0;
        std::optional<double> pending_transition;
        for (const nlohmann::json &node : *clips) {
            if (!node.is_object()) {
                continue;
            }
            const std::string schema = node.value("OTIO_SCHEMA", std::string());
            // A gap is empty time on the lane: it advances the record position
            // but produces no clip.
            if (schema == "Gap.1") {
                if (const auto range = node.find("source_range");
                    range != node.end() && range->is_object()) {
                    if (const auto duration = range->find("duration"); duration != range->end()) {
                        position += time_of(*duration).value_or(0.0);
                    }
                }
                continue;
            }
            // A transition sits on the cut that follows it: remember its span
            // and fold it into the next clip's transition-in, without
            // consuming record time (it overlaps its neighbours).
            if (schema == "Transition.1") {
                const double in_offset =
                        time_of(node.value("in_offset", nlohmann::json())).value_or(0.0);
                const double out_offset =
                        time_of(node.value("out_offset", nlohmann::json())).value_or(0.0);
                pending_transition = in_offset + out_offset;
                continue;
            }
            // Both clip revisions are in the wild: 1.x exporters still write
            // Clip.1, and the schema number is not a compatibility boundary for
            // the fields this adapter reads.
            if (schema != "Clip.1" && schema != "Clip.2") {
                continue;
            }
            Clip clip;
            clip.id = "c" + std::to_string(imported.clips.size() + 1);
            clip.track_id = track.id;
            clip.name = node.value("name", std::string("Clip"));
            clip.kind = track_kind == "Audio" ? ClipKind::Audio : ClipKind::Video;

            if (const auto reference = node.find("media_reference");
                reference != node.end() && reference->is_object()) {
                const std::string url = reference->value("target_url", std::string());
                if (!url.empty()) {
                    const std::string path = resolve_url(url, source_path);
                    const auto existing = std::find_if(
                            project.media.begin(), project.media.end(),
                            [&path](const MediaItem &item) { return item.path == path; });
                    if (existing != project.media.end()) {
                        clip.media_id = existing->id;
                    } else {
                        MediaItem media;
                        media.id = "m" + std::to_string(project.media.size() + 1);
                        media.path = path;
                        media.name = basename_of(path);
                        media.kind =
                                clip.kind == ClipKind::Audio ? MediaKind::Audio : MediaKind::Video;
                        clip.media_id = media.id;
                        project.media.push_back(std::move(media));
                    }
                } else if (reference->value("OTIO_SCHEMA", std::string()) == "GeneratorReference.1"
                           || reference->value("OTIO_SCHEMA", std::string())
                                   == "MissingReference.1") {
                    // A generator, or a reference whose media has gone missing,
                    // has no file. The model can still carry it as a
                    // missing-media bin item, so the clip survives (relinkable
                    // or deletable) where an empty media id would settle the
                    // clip away. The empty path is deliberate: the writer maps a
                    // missing reference to `""`, so it round-trips.
                    const std::string schema = reference->value("OTIO_SCHEMA", std::string());
                    const std::string generator_kind =
                            reference->value("generator_kind", std::string());
                    // An unnamed reference falls back to the clip's own name,
                    // which is the only identity a lost file has left.
                    std::string generator_name = reference->value("name", std::string());
                    if (generator_name.empty()) {
                        generator_name = clip.name;
                    }
                    const auto metadata = reference->find("metadata");
                    const std::string metadata_key =
                            metadata != reference->end() ? metadata->dump() : std::string();

                    const ReferenceKey key{ schema, generator_name, generator_kind, metadata_key };
                    const auto existing = reference_media.find(key);
                    if (existing != reference_media.end()) {
                        clip.media_id = existing->second;
                    } else {
                        MediaItem generator;
                        generator.id = "m" + std::to_string(project.media.size() + 1);
                        generator.path.clear();
                        generator.name = !generator_name.empty()
                                ? generator_name
                                : (!generator_kind.empty() ? generator_kind
                                                           : std::string("Missing media"));
                        generator.kind = MediaKind::Video;
                        clip.media_id = generator.id;
                        reference_media.emplace(key, generator.id);
                        project.media.push_back(std::move(generator));
                    }
                }
            }

            // A remembered transition folds into this clip's cut-in: a
            // cross-fade whose span is the two offsets added, which settle
            // clamps to the catalogue's minimum.
            if (pending_transition) {
                clip.transition_in = project::Transition{
                    "cross-fade",
                    Rational::approximate(*pending_transition).value_or(Rational::ZERO)
                };
                pending_transition.reset();
            }

            double duration = 0.0;
            double source_start = 0.0;
            if (const auto range = node.find("source_range");
                range != node.end() && range->is_object()) {
                if (const auto value = range->find("duration"); value != range->end()) {
                    duration = time_of(*value).value_or(0.0);
                }
                if (const auto value = range->find("start_time"); value != range->end()) {
                    source_start = time_of(*value).value_or(0.0);
                }
            }
            clip.start = Rational::approximate(position).value_or(Rational::ZERO);
            clip.duration = Rational::approximate(duration).value_or(Rational::ZERO);
            clip.source_start = Rational::approximate(source_start).value_or(Rational::ZERO);

            // What OTIO cannot carry comes back from the clip's own metadata:
            // start from the recorded clip, then overwrite everything the spine
            // actually said. An external retime, reorder or rename therefore
            // wins, while effects, speed and text survive untouched.
            if (const auto metadata = node.find("metadata");
                metadata != node.end() && metadata->is_object()) {
                if (const auto genesis = metadata->find("genesis");
                    genesis != metadata->end() && genesis->is_object()) {
                    if (const auto recorded = genesis->find("clip");
                        recorded != genesis->end() && recorded->is_object()) {
                        if (std::optional<Clip> prior = project::clip_from_document(*recorded)) {
                            prior->id = clip.id;
                            prior->track_id = clip.track_id;
                            prior->name = clip.name;
                            prior->kind = clip.kind;
                            prior->media_id = clip.media_id;
                            prior->start = clip.start;
                            prior->duration = clip.duration;
                            prior->source_start = clip.source_start;
                            if (clip.transition_in) {
                                prior->transition_in = clip.transition_in;
                            }
                            clip = std::move(*prior);
                        }
                    }
                }
            }

            imported.clips.push_back(std::move(clip));
            position += duration;
        }
    }

    if (imported.tracks.empty()) {
        error = "the timeline carries no tracks";
        return std::nullopt;
    }
    project.timelines.push_back(std::move(imported));
    project.active_timeline_id = "TL1";

    // Settle through the document layer, so an imported file is held to the
    // same invariants as a document read.
    std::optional<Project> readable = settle_project(std::move(project));
    if (!readable) {
        error = "the imported timeline does not settle into a project";
    }
    return readable;
}

} // namespace

nlohmann::json to_otio(const Project &project)
{
    const Timeline *timeline = active_timeline(project);
    if (timeline == nullptr) {
        return nlohmann::json();
    }
    const double rate = timeline->video.rate();

    // The document supplies the parts OTIO cannot express: each clip's effects,
    // speed curve and text travel per clip rather than as one opaque blob, so
    // an editor that retimes or reorders the spine still gets its edits back.
    const nlohmann::json document = project::to_document(project);
    std::map<std::string, nlohmann::json> clip_documents;
    for (const nlohmann::json &timeline_json : document.at("timelines")) {
        if (timeline_json.value("id", std::string()) != timeline->id) {
            continue;
        }
        for (const nlohmann::json &clip_json : timeline_json.at("clips")) {
            clip_documents[clip_json.value("id", std::string())] = clip_json;
        }
    }

    nlohmann::json children = nlohmann::json::array();
    for (const Track &track : timeline->tracks) {
        nlohmann::json nodes = nlohmann::json::array();
        for (const Clip &clip : timeline->clips) {
            if (clip.track_id == track.id) {
                nodes.push_back(clip_to_otio(clip, project, rate, clip_documents));
            }
        }
        children.push_back(nlohmann::json{
                { "OTIO_SCHEMA", "Track.1" },
                { "name", track.id },
                { "kind", track_is_audio(*timeline, track) ? "Audio" : "Video" },
                { "children", std::move(nodes) },
        });
    }

    return nlohmann::json{
        { "OTIO_SCHEMA", "Timeline.1" },
        { "name", timeline->name },
        { "global_start_time", rational_time(0.0, rate) },
        { "tracks",
          nlohmann::json{ { "OTIO_SCHEMA", "Stack.1" },
                          { "name", "tracks" },
                          { "children", std::move(children) } } },
        // Bin and font metadata, keyed by the references the spine carries. The
        // spine itself stays authoritative for the cut.
        { "metadata",
          nlohmann::json{ { "genesis",
                            nlohmann::json{ { "media", document.at("media") },
                                            { "fonts", document.at("fonts") } } } } },
    };
}

std::optional<Project> from_otio(const nlohmann::json &timeline,
                                 const std::filesystem::path &source_path,
                                 const VideoSettings &fallback, std::string &error)
{
    if (!timeline.is_object()) {
        error = "the file is not an OpenTimelineIO timeline";
        return std::nullopt;
    }

    // The spine is always authoritative: another editor may have retimed or
    // reordered it, and restoring a stale snapshot would silently discard those
    // edits. Genesis metadata rides along per clip and per media, filling only
    // the fields OTIO cannot express.
    return from_foreign_otio(timeline, source_path, fallback, error);
}

} // namespace genesis::interchange
