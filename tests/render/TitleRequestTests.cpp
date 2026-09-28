// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/ClipTimeline.h"
#include "effects/Pack.h"
#include "effects/Resolve.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/model/Clip.h"
#include "project/model/Effects.h"
#include "project/model/Text.h"
#include "project/Editor.h"
#include "render/Resolve.h"
#include "render/TitleRequests.h"

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
namespace render = genesis::render;
namespace effects = genesis::effects;
using genesis::core::Rational;

// Places a title on the active timeline through the real command layer and
// returns the clip id it minted, so the fixture cannot drift from the model.
std::string add_title(Editor &editor, const std::string &track_id, Rational start, TextStyle style)
{
    const auto result = editor.apply(Command{ AddTextClip{
            std::optional<std::string>{ track_id }, false, start,
            std::optional<TextStyle>{ std::move(style) }, std::nullopt, std::nullopt } });
    check(result.has_value(), "adds a title");
    check(result->created_id.has_value(), "mints an id");
    return *result->created_id;
}

TextStyle style_with(std::string content)
{
    TextStyle style;
    style.content = std::move(content);
    return style;
}

void test_one_title_yields_one_request()
{
    Editor editor;
    // A non-default frame, so the canvas is proven to come from the timeline.
    editor.project().timelines[0].video.width = 1280;
    editor.project().timelines[0].video.height = 720;
    const std::string track_id = editor.project().timelines[0].tracks[0].id;

    TextStyle style;
    style.content = "Hello world";
    style.color = "#ff0000";
    const std::string clip_id = add_title(editor, track_id, Rational{ 2, 1 }, style);

    const std::vector<render::TitleRequest> requests = render::title_requests(editor.project());
    check(requests.size() == 1, "one title, one request");
    check(requests[0].clip_id == clip_id, "the clip id");
    check(requests[0].text == "Hello world", "the words");
    check(requests[0].style == style, "the style");
    check(requests[0].canvas_width == 1280 && requests[0].canvas_height == 720,
          "the timeline's canvas");
}

void test_no_titles_yields_none()
{
    Editor editor;
    check(render::title_requests(editor.project()).empty(), "no titles, no requests");
}

void test_empty_text_yields_no_request()
{
    Editor editor;
    const std::string track_id = editor.project().timelines[0].tracks[0].id;
    add_title(editor, track_id, Rational::ZERO, style_with(""));
    check(render::title_requests(editor.project()).empty(),
          "an empty title asks for nothing to draw");
}

void test_two_titles_come_back_in_timeline_order()
{
    Editor editor;
    const std::string track_id = editor.project().timelines[0].tracks[0].id;
    // Inserted later-first, so insertion order would be wrong; time order is
    // what the reader must recover.
    const std::string later = add_title(editor, track_id, Rational{ 5, 1 }, style_with("Later"));
    const std::string earlier =
            add_title(editor, track_id, Rational{ 1, 1 }, style_with("Earlier"));

    const std::vector<render::TitleRequest> requests = render::title_requests(editor.project());
    check(requests.size() == 2, "two titles, two requests");
    check(requests[0].clip_id == earlier, "the earlier title comes first");
    check(requests[1].clip_id == later, "the later title comes second");
}

void test_title_images_carried_on_the_built_timeline()
{
    render::ExportRequest request;
    request.width = 1280;
    request.height = 720;

    // A title clip as the app hands it over: an image-kind clip whose path is
    // the clip id - the stand-in the resolver and the engine adapter key on.
    std::vector<render::ExportClip> visible;
    render::ExportClip title =
            render::ExportClip::blank(render::ClipKind::Image, Rational::ZERO, Rational{ 5, 1 }, 0);
    title.path = "c1";
    visible.push_back(std::move(title));

    const std::filesystem::path png = std::filesystem::temp_directory_path() / "title.png";
    std::unordered_map<std::string, std::string> images;
    images.emplace("c1", png.string());

    const render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(visible), { }, images);
    check(built.title_images.size() == 1, "one supplied picture carried");
    check(built.title_images.at("c1") == png.string(), "the entry matches the clip id");

    // Without the map the graph builds unchanged: no entry, no failure.
    const render::BuiltTimeline bare =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(visible), { });
    check(bare.title_images.empty(), "no entry without the map");
    check(bare.timeline.clip_count() == 1, "still builds without the map");
}

void test_reveal_map_carried_on_the_built_timeline()
{
    render::ExportRequest request;
    request.width = 1280;
    request.height = 720;

    // The same title clip the app hands over, carrying a per-word reveal map
    // keyed by its clip id - the stand-in path a title reaches the graph with.
    std::vector<render::ExportClip> visible;
    render::ExportClip title =
            render::ExportClip::blank(render::ClipKind::Image, Rational::ZERO, Rational{ 5, 1 }, 0);
    title.path = "c1";
    visible.push_back(std::move(title));

    // A two-word map at the canvas size, as the app bakes it beside the picture.
    const genesis::core::RevealMap map = genesis::core::RevealMap::from_rects(
            1280, 720, { { { 0, 0, 640, 720 }, { 640, 0, 640, 720 } } });
    std::unordered_map<std::string, genesis::core::RevealMap> maps;
    maps.emplace("c1", map);

    const render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(visible), { }, { }, maps);
    check(built.reveal_maps.size() == 1, "one supplied map carried");
    check(built.reveal_maps.at("c1") == map, "the map round-trips under the clip id");
    check(built.reveal_maps.at("c1").width == 1280 && built.reveal_maps.at("c1").height == 720,
          "the map keeps its canvas size");

    // Without the map the graph builds unchanged: no entry, no failure.
    const render::BuiltTimeline bare =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(visible), { });
    check(bare.reveal_maps.empty(), "no reveal entry without the map");
}

void test_reveal_map_attached_to_the_resolved_pass()
{
    // A minimal effect Pack - no parameters, no passes - is the simplest thing
    // resolve_pass accepts, enough to prove the map is attached, not resolved.
    effects::Pack pack;
    pack.id = "test.reveal";
    pack.kind = effects::Kind::Effect;
    const AppliedFilter instance = AppliedFilter::create("test.reveal");

    const genesis::core::RevealMap map =
            genesis::core::RevealMap::from_rects(4, 2, { { { 0, 0, 2, 2 }, { 2, 0, 2, 2 } } });

    const auto with_map = effects::resolve_pass(pack, instance, 0.0, map);
    check(with_map.has_value(), "the pass resolves with a map");
    if (!with_map) {
        return;
    }
    check(with_map->reveal_map.has_value(), "the pass carries the map");
    check(*with_map->reveal_map == map, "the map is the one supplied");

    const auto without = effects::resolve_pass(pack, instance, 0.0);
    check(without.has_value(), "the pass resolves without a map");
    if (!without) {
        return;
    }
    check(!without->reveal_map.has_value(), "no map supplied leaves reveal_map unset");
}

} // namespace

int main()
{
    test_one_title_yields_one_request();
    test_no_titles_yields_none();
    test_empty_text_yields_no_request();
    test_two_titles_come_back_in_timeline_order();
    test_title_images_carried_on_the_built_timeline();
    test_reveal_map_carried_on_the_built_timeline();
    test_reveal_map_attached_to_the_resolved_pass();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
