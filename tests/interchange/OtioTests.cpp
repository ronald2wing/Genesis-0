// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Interchange over the typed model: the OTIO shape, an exact round trip, a
// foreign file, the EDL text, and the two host verbs that expose them.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "api/Api.h"
#include "api/Requests.h"
#include "core/ClipTimeline.h"
#include "interchange/Cmx3600.h"
#include "interchange/FcpXml.h"
#include "interchange/Otio.h"
#include "project/Document.h"
#include "project/Editor.h"
#include "project/Project.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"
#include "../support/Checks.h"
#include "Fixtures.h"

namespace {

using genesis::test::check;

namespace api = genesis::api;
namespace interchange = genesis::interchange;
namespace project = genesis::project;

using genesis::core::Rational;
using genesis::test::interchange::fixture;
using genesis::test::interchange::seconds;

void test_otio_shape()
{
    const project::Project edited = fixture();
    const nlohmann::json timeline = interchange::to_otio(edited);
    check(timeline.is_object(), "to_otio returns a document");
    check(timeline.value("OTIO_SCHEMA", std::string()) == "Timeline.1",
          "the document is an OTIO Timeline");
    check(timeline.value("name", std::string()) == "Cut", "the timeline keeps its name");

    const nlohmann::json &tracks = timeline.at("tracks");
    check(tracks.value("OTIO_SCHEMA", std::string()) == "Stack.1", "tracks is an OTIO Stack");
    const nlohmann::json &children = tracks.at("children");
    check(children.size() == 4, "every lane is a track");
    if (children.size() < 2) {
        return;
    }
    check(children[0].value("kind", std::string()) == "Video", "T1 reads as video");
    check(children[0].at("children").size() == 2, "T1 carries both pictures");
    check(children[1].value("kind", std::string()) == "Audio", "T2 reads as audio");
    check(children[1].at("children").size() == 1, "T2 carries the sound");

    const nlohmann::json &clip = children[0].at("children")[0];
    check(clip.value("OTIO_SCHEMA", std::string()) == "Clip.2", "a lane child is a Clip");
    const nlohmann::json &source = clip.at("source_range");
    check(source.at("start_time").at("rate").get<double>() == 30.0,
          "the source range carries the timeline rate");
    check(source.at("duration").at("value").get<double>() == 30.0,
          "one second at 30 fps is 30 frames");
    check(clip.at("media_reference").value("target_url", std::string()) == "file:///media/a.mp4",
          "a picture points at its media path");
    check(clip.at("media_reference").value("OTIO_SCHEMA", std::string()) == "ExternalReference.1",
          "a resolvable picture is an external reference");

    // What OTIO cannot express travels per clip and per bin entry, so an editor
    // that rewrites the spine still gets its edits back.
    check(timeline.at("metadata").at("genesis").contains("media"),
          "the bin metadata travels with the file");
    const nlohmann::json &recorded = clip.at("metadata").at("genesis").at("clip");
    check(recorded.value("id", std::string()) == "c1", "the clip carries its own metadata");
    check(recorded.contains("videoEffects"), "including fields OTIO cannot model");
}

void test_otio_structure_wins_over_metadata()
{
    // A real editor may retime the spine while preserving our metadata. The
    // spine is the truth: re-importing must reflect the edit, not restore the
    // snapshot we wrote.
    const project::Project edited = fixture();
    nlohmann::json timeline = interchange::to_otio(edited);
    timeline["tracks"]["children"][0]["children"][0]["source_range"]["duration"]["value"] = 90.0;

    std::string error;
    const std::optional<project::Project> loaded =
            interchange::from_otio(timeline, "/tmp/edited.otio", project::VideoSettings{ }, error);
    check(loaded.has_value(), "an externally retimed timeline imports: " + error);
    if (!loaded || loaded->timelines.empty() || loaded->timelines[0].clips.empty()) {
        return;
    }
    check(loaded->timelines[0].clips[0].duration == seconds(3.0),
          "the spine's retime wins over the recorded clip");
    check(loaded->timelines[0].clips[0].name == "One",
          "the recorded clip still supplies the fields the spine omits");
}

void test_otio_missing_media_survives()
{
    // A clip whose media has gone missing exports as a MissingReference; the
    // re-import must keep it as a relinkable bin item rather than settle it
    // away, or an export/import silently deletes part of the cut.
    project::Project edited;
    project::Clip orphan;
    orphan.id = "c1";
    orphan.track_id = "T1";
    orphan.name = "Lost";
    orphan.kind = project::ClipKind::Video;
    orphan.start = seconds(0.0);
    orphan.duration = seconds(1.0);
    edited.timelines[0].clips.push_back(orphan);

    const nlohmann::json timeline = interchange::to_otio(edited);
    check(timeline.at("tracks")
                          .at("children")[0]
                          .at("children")[0]
                          .at("media_reference")
                          .value("OTIO_SCHEMA", std::string())
                  == "MissingReference.1",
          "a clip with no media exports as a missing reference");

    std::string error;
    const std::optional<project::Project> loaded =
            interchange::from_otio(timeline, "/tmp/missing.otio", project::VideoSettings{ }, error);
    check(loaded.has_value(), "the missing reference imports: " + error);
    if (!loaded || loaded->timelines.empty()) {
        return;
    }
    check(loaded->timelines[0].clips.size() == 1, "the orphaned clip survives the round trip");
    check(loaded->media.size() == 1 && loaded->media[0].path.empty(),
          "it becomes a pathless bin item, ready to relink");
}

void test_otio_round_trip()
{
    const project::Project edited = fixture();
    const nlohmann::json timeline = interchange::to_otio(edited);
    std::string error;
    const std::optional<project::Project> loaded = interchange::from_otio(
            timeline, "/tmp/round-trip.otio", project::VideoSettings{ }, error);
    check(loaded.has_value(), "an exported timeline reads back");
    if (!loaded) {
        return;
    }
    check(loaded->timelines.size() == edited.timelines.size(), "the timeline count survives");
    check(loaded->media.size() == edited.media.size(), "the bin survives");
    if (loaded->timelines.empty()) {
        return;
    }
    check(loaded->timelines[0].clips.size() == edited.timelines[0].clips.size(),
          "every clip survives");
    for (std::size_t index = 0; index < loaded->timelines[0].clips.size(); ++index) {
        const project::Clip &before = edited.timelines[0].clips[index];
        const project::Clip &after = loaded->timelines[0].clips[index];
        check(after.id == before.id && after.name == before.name
                      && after.track_id == before.track_id && after.start == before.start
                      && after.duration == before.duration
                      && after.source_start == before.source_start,
              "clip " + before.id + " round-trips exactly");
    }
}

// A file with none of our metadata: the adapter must rebuild the model from the
// OTIO structure alone.
void test_foreign_otio_is_synthesised()
{
    nlohmann::json range = {
        { "OTIO_SCHEMA", "TimeRange.1" },
    };
    range["start_time"] = { { "OTIO_SCHEMA", "RationalTime.1" },
                            { "rate", 25.0 },
                            { "value", 50.0 } };
    range["duration"] = { { "OTIO_SCHEMA", "RationalTime.1" },
                          { "rate", 25.0 },
                          { "value", 75.0 } };

    nlohmann::json clip = {
        { "OTIO_SCHEMA", "Clip.2" },
        { "name", "Shot" },
        { "source_range", range },
        { "media_reference",
          { { "OTIO_SCHEMA", "ExternalReference.1" },
            { "target_url", "file:///media/foreign.mp4" } } },
    };

    nlohmann::json track = {
        { "OTIO_SCHEMA", "Track.1" },
        { "name", "V1" },
        { "kind", "Video" },
        { "children", nlohmann::json::array({ clip }) },
    };

    nlohmann::json timeline = {
        { "OTIO_SCHEMA", "Timeline.1" },
        { "name", "Foreign" },
        { "tracks",
          { { "OTIO_SCHEMA", "Stack.1" },
            { "name", "tracks" },
            { "children", nlohmann::json::array({ track }) } } },
    };

    std::string error;
    const std::optional<project::Project> loaded =
            interchange::from_otio(timeline, "/tmp/foreign.otio", project::VideoSettings{ }, error);
    check(loaded.has_value(), "a foreign timeline reads: " + error);
    if (!loaded) {
        return;
    }
    check(loaded->timelines.size() == 1, "one timeline is built");
    check(loaded->media.size() == 1, "the reference becomes a media item");
    if (loaded->timelines.empty() || loaded->media.empty()) {
        return;
    }
    check(loaded->media[0].path == "/media/foreign.mp4", "the media path resolves");
    check(loaded->timelines[0].clips.size() == 1, "the clip is rebuilt");
    if (loaded->timelines[0].clips.empty()) {
        return;
    }
    const project::Clip &rebuilt = loaded->timelines[0].clips[0];
    check(rebuilt.duration == seconds(3.0), "75 frames at 25 fps is three seconds");
    check(rebuilt.source_start == seconds(2.0), "50 frames at 25 fps is a two-second in-point");
    check(loaded->timelines[0].video.rate_num == 25 && loaded->timelines[0].video.rate_den == 1,
          "the timeline takes the file's rate");
}

// The fixtures live beside the suite; the walk matches the other asset-reading
// suites, which run with the source directory as their working directory.
std::filesystem::path fixture_directory()
{
    std::filesystem::path dir = std::filesystem::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        const std::filesystem::path candidate = dir / "tests" / "interchange" / "fixtures";
        if (std::filesystem::is_directory(candidate)) {
            return candidate;
        }
        if (!dir.has_parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    return { };
}

std::optional<nlohmann::json> load_timeline(const std::filesystem::path &path)
{
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    const nlohmann::json parsed = nlohmann::json::parse(in, nullptr, false);
    if (parsed.is_discarded()) {
        return std::nullopt;
    }
    return std::make_optional(std::move(parsed));
}

bool near(double left, double right)
{
    return std::abs(left - right) < 0.01;
}

// A 1.x exporter: Clip.1 entries, a track with no `kind`, absolute media URLs.
void test_foreign_simple_cut()
{
    const std::filesystem::path directory = fixture_directory();
    check(!directory.empty(), "the interchange fixtures are found");
    if (directory.empty()) {
        return;
    }
    const std::filesystem::path path = directory / "foreign-simple-cut.otio";
    const std::optional<nlohmann::json> timeline = load_timeline(path);
    check(timeline.has_value(), "the simple-cut fixture parses");
    if (!timeline) {
        return;
    }
    std::string error;
    const std::optional<project::Project> loaded =
            interchange::from_otio(*timeline, path, project::VideoSettings{ }, error);
    check(loaded.has_value(), "a Clip.1 timeline imports: " + error);
    if (!loaded || loaded->timelines.empty()) {
        return;
    }
    const project::Timeline &imported = loaded->timelines[0];
    check(imported.clips.size() == 3, "all three Clip.1 entries import");
    check(loaded->media.size() == 3, "each reference becomes a media item");
    check(near(imported.video.rate(), 24.0), "the timeline takes the file's rate");
    if (imported.clips.size() != 3) {
        return;
    }
    check(imported.clips[0].duration == seconds(2.0), "48 frames at 24 fps is two seconds");
    check(imported.clips[1].start == seconds(2.0), "the second clip follows the first");
    check(imported.clips[2].start == seconds(3.0), "the third follows the second");
}

// Gaps advance the record position without producing clips, and an Audio track
// becomes an audio lane.
void test_foreign_multitrack()
{
    const std::filesystem::path directory = fixture_directory();
    if (directory.empty()) {
        return;
    }
    const std::filesystem::path path = directory / "foreign-multitrack.otio";
    const std::optional<nlohmann::json> timeline = load_timeline(path);
    check(timeline.has_value(), "the multi-track fixture parses");
    if (!timeline) {
        return;
    }
    std::string error;
    const std::optional<project::Project> loaded =
            interchange::from_otio(*timeline, path, project::VideoSettings{ }, error);
    check(loaded.has_value(), "a multi-track timeline imports: " + error);
    if (!loaded || loaded->timelines.empty()) {
        return;
    }
    const project::Timeline &imported = loaded->timelines[0];
    check(imported.tracks.size() == 3, "all three lanes are rebuilt");
    check(imported.clips.size() == 4, "gaps produce no clips, every real clip survives");
    check(near(imported.video.rate(), 29.97), "the NTSC rate is preserved");
    if (imported.clips.size() != 4) {
        return;
    }
    // V1 starts with a four-frame gap, so its first picture sits 4 frames in.
    check(near(imported.clips[0].start.as_double(), 4.0 / 29.97),
          "a leading gap offsets the first clip");
    check(near(imported.clips[1].start.as_double(), 64.0 / 29.97),
          "a gap between clips offsets the next one");
    check(imported.clips[2].kind == project::ClipKind::Video, "V2 is video");
    check(imported.clips[3].kind == project::ClipKind::Audio, "A1 is audio");
}

// Clip.2, a transition, a marker, a generator reference and a relative URL.
void test_foreign_premiere()
{
    const std::filesystem::path directory = fixture_directory();
    if (directory.empty()) {
        return;
    }
    const std::filesystem::path path = directory / "foreign-premiere.otio";
    const std::optional<nlohmann::json> timeline = load_timeline(path);
    check(timeline.has_value(), "the premiere-style fixture parses");
    if (!timeline) {
        return;
    }
    std::string error;
    const std::optional<project::Project> loaded =
            interchange::from_otio(*timeline, path, project::VideoSettings{ }, error);
    check(loaded.has_value(), "a Clip.2 timeline imports: " + error);
    if (!loaded || loaded->timelines.empty()) {
        return;
    }
    const project::Timeline &imported = loaded->timelines[0];
    // A generator reference has no file, but the model carries it as missing
    // media so the clip survives in the bin as relinkable/deletable instead of
    // being dropped; the transition folds onto the following clip's cut-in.
    check(imported.clips.size() == 2, "the generator clip and the file clip both survive");
    if (imported.clips.size() != 2) {
        return;
    }
    check(imported.clips[0].name == "Slate", "the generator clip keeps its name");
    check(!imported.clips[0].media_id.empty(), "the generator clip has a media item");
    check(imported.clips[1].name == "Interview", "the file-backed clip survives");
    if (!imported.clips[0].media_id.empty()) {
        const project::MediaItem *generator = loaded->media_by_id(imported.clips[0].media_id);
        check(generator != nullptr, "the generator media is in the bin");
        if (generator != nullptr) {
            check(generator->path.empty(), "the generator media's path is empty");
            check(generator->name == "Slate", "the generator media keeps its name");
        }
    }
    check(loaded->media.size() == 2, "both clips add a media item");
    check(imported.clips[1].media_id == "m2",
          "the file-backed clip follows the generator in the bin");
    if (!loaded->media.empty()) {
        check(loaded->media[1].path == (directory / "media" / "interview.mov").string(),
              "a relative URL resolves against the timeline's directory");
    }
    check(imported.clips[1].start == seconds(1.0),
          "the generator's time and the transition leave record time intact");
    check(imported.clips[1].transition_in.has_value(), "the transition survives");
    if (imported.clips[1].transition_in) {
        check(imported.clips[1].transition_in->id == "cross-fade",
              "the transition is the catalogue's cross-fade");
        check(near(imported.clips[1].transition_in->duration.as_double(), 0.48),
              "in+out offsets of 6+6 frames at 25 fps are 0.48 s");
    }
    check(!imported.clips[0].transition_in.has_value(), "the first clip carries no transition");
}

// A single video clip whose source range carries no rate: a stills-only or
// stripped export that the importer must place with the caller's rate.
nlohmann::json rate_free_timeline()
{
    nlohmann::json clip = {
        { "OTIO_SCHEMA", "Clip.2" },
        { "name", "Still" },
        { "source_range",
          { { "OTIO_SCHEMA", "TimeRange.1" },
            { "duration", { { "OTIO_SCHEMA", "RationalTime.1" }, { "value", 25.0 } } } } },
        { "media_reference", { { "OTIO_SCHEMA", "MissingReference.1" } } },
    };
    nlohmann::json track = {
        { "OTIO_SCHEMA", "Track.1" },
        { "name", "V1" },
        { "kind", "Video" },
        { "children", nlohmann::json::array({ clip }) },
    };
    return nlohmann::json{
        { "OTIO_SCHEMA", "Timeline.1" },
        { "name", "Stills" },
        { "tracks",
          { { "OTIO_SCHEMA", "Stack.1" },
            { "name", "tracks" },
            { "children", nlohmann::json::array({ track }) } } },
    };
}

// A timeline with no clip-declared rate and no global_start_time lands on the
// fallback, not the old silent 30 fps: frames are never displaced by a guess.
void test_otio_fallback_rate()
{
    project::VideoSettings fallback{ };
    fallback.rate_num = 25;
    fallback.rate_den = 1;

    const nlohmann::json timeline = rate_free_timeline();
    std::string error;
    const std::optional<project::Project> loaded =
            interchange::from_otio(timeline, "/tmp/stills.otio", fallback, error);
    check(loaded.has_value(), "a rate-free timeline imports: " + error);
    if (!loaded || loaded->timelines.empty()) {
        return;
    }
    const project::Timeline &imported = loaded->timelines[0];
    check(imported.video.rate_num == 25 && imported.video.rate_den == 1,
          "the timeline takes the fallback rate when the file declares none");
    check(imported.video.width == fallback.width && imported.video.height == fallback.height,
          "the fallback frame survives when the file declares none");
}

// global_start_time is the one rate OTIO pins to the timeline itself, so it
// wins over the fallback even when every clip is rate-free.
void test_otio_global_start_time_wins()
{
    project::VideoSettings fallback{ };
    fallback.rate_num = 25;
    fallback.rate_den = 1;

    nlohmann::json timeline = rate_free_timeline();
    timeline["global_start_time"] = {
        { "OTIO_SCHEMA", "RationalTime.1" },
        { "rate", 50.0 },
        { "value", 0.0 },
    };

    std::string error;
    const std::optional<project::Project> loaded =
            interchange::from_otio(timeline, "/tmp/stills-start-time.otio", fallback, error);
    check(loaded.has_value(), "a timeline with global_start_time imports: " + error);
    if (!loaded || loaded->timelines.empty()) {
        return;
    }
    check(loaded->timelines[0].video.rate_num == 50 && loaded->timelines[0].video.rate_den == 1,
          "global_start_time's rate wins over the fallback");
}

} // namespace

// A reference with no file is identified by its whole reference, not its label:
// two slates that share a name but carry different parameters are different
// sources, and an unnamed lost file is identified by its clip's name.
void test_synthetic_reference_identity()
{
    auto generator = [](const std::string &name, const std::string &kind,
                        const nlohmann::json &metadata) {
        return nlohmann::json{
            { "OTIO_SCHEMA", "Clip.2" },
            { "name", name },
            { "source_range",
              { { "OTIO_SCHEMA", "TimeRange.1" },
                { "start_time",
                  { { "OTIO_SCHEMA", "RationalTime.1" }, { "rate", 25.0 }, { "value", 0.0 } } },
                { "duration",
                  { { "OTIO_SCHEMA", "RationalTime.1" },
                    { "rate", 25.0 },
                    { "value", 25.0 } } } } },
            { "media_reference",
              { { "OTIO_SCHEMA", "GeneratorReference.1" },
                { "name", name },
                { "generator_kind", kind },
                { "metadata", metadata } } },
        };
    };

    const nlohmann::json red = { { "color", "red" } };
    const nlohmann::json blue = { { "color", "blue" } };

    nlohmann::json track;
    track["OTIO_SCHEMA"] = "Track.1";
    track["name"] = "V1";
    track["kind"] = "Video";
    track["children"] = nlohmann::json::array({ generator("Slate", "SMPTEBars", red),
                                                generator("Slate", "SMPTEBars", blue),
                                                generator("Slate", "SMPTEBars", red) });

    nlohmann::json stack;
    stack["OTIO_SCHEMA"] = "Stack.1";
    stack["name"] = "tracks";
    stack["children"] = nlohmann::json::array({ track });

    nlohmann::json timeline;
    timeline["OTIO_SCHEMA"] = "Timeline.1";
    timeline["name"] = "Slates";
    timeline["tracks"] = stack;

    std::string error;
    const std::optional<project::Project> loaded =
            interchange::from_otio(timeline, "/tmp/slates.otio", project::VideoSettings{ }, error);
    check(loaded.has_value(), "generator references import: " + error);
    if (!loaded) {
        return;
    }
    check(loaded->timelines.front().clips.size() == 3, "every slate clip survives");
    check(loaded->media.size() == 2,
          "the two parameter sets are two sources; the repeat shares the first");
    if (loaded->media.size() == 2) {
        check(loaded->media[0].name == "Slate" && loaded->media[0].path.empty(),
              "each is a pathless bin item ready to relink");
    }
}

int main()
{
    test_otio_shape();
    test_otio_round_trip();
    test_otio_missing_media_survives();
    test_otio_structure_wins_over_metadata();
    test_foreign_otio_is_synthesised();
    test_foreign_simple_cut();
    test_foreign_multitrack();
    test_foreign_premiere();
    test_otio_fallback_rate();
    test_otio_global_start_time_wins();
    test_synthetic_reference_identity();
    return genesis::test::summary();
}
