// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/Logs.h"
#include "workspace/Projects.h"
#include "workspace/Templates.h"
#include "project/Project.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"
#include "project/model/VideoSettings.h"
#include "project/Editor.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

using namespace genesis::project;
namespace workspace = genesis::workspace;

// A scratch directory under the system temp dir, unique per test, removed
// first so a rerun starts clean. Never a hardcoded path: CI has its own
// temp, and a stale assumption about `/tmp` has broken the build before.
std::filesystem::path scratch(std::string_view name)
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("genesis-host-" + std::string(name));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

std::string read_all(const std::filesystem::path &file)
{
    std::ifstream in(file);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Two video items and two clips, one on lane 0 at 0s, one on lane 1 at 1s,
// built through the real command layer so the fixture cannot drift from the
// model. The media paths do not exist on disk, which is what the absent-media
// test leans on.
struct Cut
{
    Editor editor;
    std::string media_a;
    std::string media_b;
    std::string clip_a;
    std::string clip_b;
};

Cut make_cut()
{
    Cut cut;
    NewMedia a;
    a.path = "/footage/a.mp4";
    a.name = "a.mp4";
    a.duration = Rational{ 4, 1 };
    a.kind = MediaKind::Video;
    a.width = 1920;
    a.height = 1080;
    const auto add_a = cut.editor.apply(Command{ AddMedia{ std::move(a) } });
    cut.media_a = *add_a->created_id;

    NewMedia b;
    b.path = "/footage/b.mp4";
    b.name = "b.mp4";
    b.duration = Rational{ 2, 1 };
    b.kind = MediaKind::Video;
    b.width = 1920;
    b.height = 1080;
    const auto add_b = cut.editor.apply(Command{ AddMedia{ std::move(b) } });
    cut.media_b = *add_b->created_id;

    const std::string lane0 = cut.editor.project().active().tracks[0].id;
    const std::string lane1 = cut.editor.project().active().tracks[1].id;
    const auto add_clip_a =
            cut.editor.apply(Command{ AddClip{ cut.media_a, lane0, Rational::ZERO, false } });
    cut.clip_a = *add_clip_a->created_id;
    const auto add_clip_b =
            cut.editor.apply(Command{ AddClip{ cut.media_b, lane1, Rational{ 1, 1 }, false } });
    cut.clip_b = *add_clip_b->created_id;
    return cut;
}

// The lane a clip sits on, as its index in the timeline's top-to-bottom
// track order - what "lane fidelity" means, since a track has no name.
std::size_t lane_index(const Timeline &timeline, const Clip &clip)
{
    for (std::size_t index = 0; index < timeline.tracks.size(); ++index) {
        if (timeline.tracks[index].id == clip.track_id) {
            return index;
        }
    }
    return timeline.tracks.size();
}

const Clip *clip_with_media(const Timeline &timeline, const Project &project, std::string_view path)
{
    for (const Clip &clip : timeline.clips) {
        const MediaItem *media = project.media_by_id(clip.media_id);
        if (media != nullptr && media->path == path) {
            return &clip;
        }
    }
    return nullptr;
}

workspace::ProjectInfo make_info(const std::filesystem::path &root, std::string_view name)
{
    workspace::ProjectInfo info;
    info.path = root.string();
    info.name = std::string(name);
    info.width = 1920;
    info.height = 1080;
    info.rate_num = 30;
    info.rate_den = 1;
    return info;
}

// -- templates ---------------------------------------------------------------

void test_save_then_instantiate_round_trips()
{
    Cut cut = make_cut();
    const std::filesystem::path file = scratch("template") / "cut.json";
    const auto saved = workspace::save_template(cut.editor.project(), { cut.clip_a, cut.clip_b },
                                                "My Cut", file);
    check(saved.has_value(), "saves a template");
    check(saved->media == 2, "two media items sliced");
    check(saved->tracks == 2, "two lanes sliced");
    check(saved->clips == 2, "two clips sliced");

    Editor target;
    const auto result = workspace::instantiate_template(target, file);
    check(result.has_value(), "instantiates");
    const Timeline &timeline = target.project().active();
    check(timeline.clips.size() == 2, "two clips land");
    check(target.project().media.size() == 2, "two media land");

    const Clip *a = clip_with_media(timeline, target.project(), "/footage/a.mp4");
    const Clip *b = clip_with_media(timeline, target.project(), "/footage/b.mp4");
    check(a != nullptr && b != nullptr, "both clips land");
    if (a != nullptr) {
        check(lane_index(timeline, *a) == 0, "clip a lands on lane 0");
        check(a->start == Rational::ZERO, "clip a keeps its start");
        check(a->duration == Rational{ 4, 1 }, "clip a keeps its duration");
        check(a->kind == ClipKind::Video, "clip a keeps its kind");
    }
    if (b != nullptr) {
        check(lane_index(timeline, *b) == 1, "clip b lands on lane 1");
        check(b->start == Rational{ 1, 1 }, "clip b keeps its start");
        check(b->duration == Rational{ 2, 1 }, "clip b keeps its duration");
    }
}

void test_instantiate_undo_restores_the_project()
{
    Cut cut = make_cut();
    const std::filesystem::path file = scratch("undo") / "cut.json";
    check(workspace::save_template(cut.editor.project(), { cut.clip_a, cut.clip_b }, "Cut", file)
                  .has_value(),
          "saves");

    Editor target;
    const Project before = target.project();
    const auto result = workspace::instantiate_template(target, file);
    check(result.has_value(), "instantiates");
    check(!(target.project() == before), "the import changed the project");
    while (target.can_undo()) {
        target.undo();
    }
    check(target.project() == before, "undo returns the exact original project");
}

void test_a_missing_or_corrupt_template_is_refused()
{
    Editor editor;
    const Project before = editor.project();

    check(!workspace::instantiate_template(editor, scratch("missing") / "nope.json").has_value(),
          "a missing file is refused");
    check(editor.project() == before, "a missing file leaves the project");

    const std::filesystem::path garbage = scratch("corrupt") / "bad.json";
    {
        std::ofstream out(garbage);
        out << "not json at all";
    }
    check(!workspace::instantiate_template(editor, garbage).has_value(), "garbage is refused");
    check(editor.project() == before, "garbage leaves the project");

    const std::filesystem::path empty = scratch("empty") / "empty.json";
    {
        std::ofstream out(empty);
        out << "{}\n";
    }
    check(!workspace::instantiate_template(editor, empty).has_value(),
          "an empty document is refused");
    check(editor.project() == before, "an empty document leaves the project");
}

void test_absent_media_still_instantiates()
{
    Cut cut = make_cut();
    // The fixture's media paths do not exist on disk. A template outlives its
    // footage the way a project does (R9): importing reuses probed metadata
    // and never touches the filesystem.
    check(!std::filesystem::exists("/footage/a.mp4"), "the fixture media is absent");
    const std::filesystem::path file = scratch("absent") / "cut.json";
    check(workspace::save_template(cut.editor.project(), { cut.clip_a, cut.clip_b }, "Cut", file)
                  .has_value(),
          "saves");
    Editor target;
    const auto result = workspace::instantiate_template(target, file);
    check(result.has_value(), "instantiates despite absent media");
    check(target.project().media.size() == 2, "the bin stays intact");
    check(target.project().active().clips.size() == 2, "the clips stay intact");
}

// -- projects (recents) ------------------------------------------------------

void test_recents_add_list_remove_pin()
{
    const std::filesystem::path dir = scratch("recents");
    const std::filesystem::path config = dir / "config";
    VideoSettings video;
    video.width = 1920;
    video.height = 1080;
    video.rate_num = 30;
    video.rate_den = 1;
    const auto made = workspace::create_project(dir, "One", video);
    check(made.has_value(), "creates project One");
    const workspace::ProjectInfo one = make_info(std::filesystem::path(made->path), "One");

    workspace::add_recents(config, one);
    auto list = workspace::list_recents(config);
    check(list.size() == 1, "one recents entry");
    check(list[0].path == one.path, "the path survives");
    check(list[0].name == "One", "the name survives");

    workspace::pin_recents(config, one.path, true);
    list = workspace::list_recents(config);
    check(list.size() == 1 && list[0].pinned, "pinning sets the flag");

    workspace::remove_recents(config, one.path);
    check(workspace::list_recents(config).empty(), "removing empties the list");
}

void test_recents_are_bounded()
{
    const std::filesystem::path dir = scratch("bounded");
    const std::filesystem::path config = dir / "config";
    VideoSettings video;
    video.width = 1920;
    video.height = 1080;
    video.rate_num = 30;
    video.rate_den = 1;
    for (int index = 0; index < 15; ++index) {
        const std::string name = "P" + std::to_string(index);
        const auto made = workspace::create_project(dir, name, video);
        check(made.has_value(), "creates a project");
        workspace::add_recents(config, make_info(std::filesystem::path(made->path), name));
    }
    const std::vector<workspace::ProjectInfo> list = workspace::list_recents(config);
    check(list.size() == workspace::MAX_RECENTS, "the list is bounded");
    check(list.size() == 12, "twelve entries survive");
    check(list.front().name == "P14", "the newest is first");
    check(list.back().name == "P3", "the oldest survivors are last");
}

void test_a_garbage_recents_file_reads_empty()
{
    const std::filesystem::path config = scratch("garbage");
    std::ofstream out(config / "recents.json");
    out << "this is not json";
    out.close();
    check(workspace::list_recents(config).empty(), "a garbage list reads empty");
}

void test_a_duplicate_add_does_not_duplicate()
{
    const std::filesystem::path dir = scratch("dup");
    const std::filesystem::path config = dir / "config";
    VideoSettings video;
    video.width = 1920;
    video.height = 1080;
    video.rate_num = 30;
    video.rate_den = 1;
    const auto made = workspace::create_project(dir, "One", video);
    check(made.has_value(), "creates");
    const workspace::ProjectInfo info = make_info(std::filesystem::path(made->path), "One");
    workspace::add_recents(config, info);
    workspace::add_recents(config, info);
    check(workspace::list_recents(config).size() == 1, "a duplicate add does not duplicate");
}

void test_a_project_manifest_round_trips()
{
    const std::filesystem::path dir = scratch("manifest");
    VideoSettings video;
    video.width = 3840;
    video.height = 2160;
    video.rate_num = 30000;
    video.rate_den = 1001;
    const auto made = workspace::create_project(dir, "Feature", video);
    check(made.has_value(), "creates");
    check(made->width == 3840 && made->height == 2160, "the frame is stored");
    check(made->rate_num == 30000 && made->rate_den == 1001, "the rate is stored");

    const auto opened = workspace::open_project(std::filesystem::path(made->path));
    check(opened.has_value(), "opens");
    check(opened->name == "Feature", "the name opens");
    check(opened->width == 3840 && opened->height == 2160, "the frame opens");
    check(opened->rate_num == 30000 && opened->rate_den == 1001, "the rate opens");

    check(workspace::is_project(std::filesystem::path(made->path)), "is a project");
    check(!workspace::is_project(dir / "nothing"), "a folder without a manifest is not");
}

void test_folder_names_are_sanitised()
{
    check(workspace::folder_name("My: Project?") == "My- Project-",
          "reserved characters become dashes");
    check(workspace::folder_name("  trailing.  ") == "trailing",
          "leading and trailing dots and spaces go");
    check(workspace::folder_name("...") == "Untitled project", "a name of nothing is renamed");
    check(workspace::folder_name("") == "Untitled project", "an empty name is renamed");
}

// -- logs --------------------------------------------------------------------

void test_lines_are_written_and_read_back_in_order()
{
    const std::filesystem::path dir = scratch("loglines");
    auto log = workspace::LogFile::open(dir);
    check(log.has_value(), "opens a log");
    log->log("first");
    log->log("second");
    log->log("third");
    log->close();

    const std::string contents = read_all(log->path());
    const std::size_t first = contents.find("first");
    const std::size_t second = contents.find("second");
    const std::size_t third = contents.find("third");
    check(first != std::string::npos, "the first line is written");
    check(second != std::string::npos, "the second line is written");
    check(third != std::string::npos, "the third line is written");
    check(first < second && second < third, "the lines are in order");
}

void test_the_file_is_bounded()
{
    const std::filesystem::path dir = scratch("logbounded");
    auto log = workspace::LogFile::open(dir, 256);
    check(log.has_value(), "opens");
    const std::string line(64, 'x');
    for (int index = 0; index < 50; ++index) {
        log->log(line);
    }
    log->close();

    const std::string contents = read_all(log->path());
    check(contents.find("the rest is not written down") != std::string::npos,
          "the cap notice is written");
    // Three message lines fit under 256 bytes, then the notice; the remaining
    // forty-seven calls write nothing.
    const std::size_t newlines =
            static_cast<std::size_t>(std::count(contents.begin(), contents.end(), '\n'));
    check(newlines == 4, "the file stops after the cap");
}

void test_a_missing_directory_is_created()
{
    const std::filesystem::path dir = scratch("logcreated") / "nested" / "deeper";
    auto log = workspace::LogFile::open(dir);
    check(log.has_value(), "opens in a directory that did not exist");
    check(std::filesystem::is_directory(dir), "the directory is created");
}

} // namespace

int main()
{
    test_save_then_instantiate_round_trips();
    test_instantiate_undo_restores_the_project();
    test_a_missing_or_corrupt_template_is_refused();
    test_absent_media_still_instantiates();
    test_recents_add_list_remove_pin();
    test_recents_are_bounded();
    test_a_garbage_recents_file_reads_empty();
    test_a_duplicate_add_does_not_duplicate();
    test_a_project_manifest_round_trips();
    test_folder_names_are_sanitised();
    test_lines_are_written_and_read_back_in_order();
    test_the_file_is_bounded();
    test_a_missing_directory_is_created();

    return genesis::test::summary();
}
