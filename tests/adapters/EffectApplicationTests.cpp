// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <ges/ges.h>
#include <gst/gst.h>

#include "adapters/engine/ges/session/GesBuilder.h"
#include "adapters/engine/ges/effects/ShaderChain.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Clip.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"
#include "project/model/Media.h"
#include "project/Editor.h"
#include "render/Catalogue.h"
#include "render/Flatten.h"
#include "render/Resolve.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

using namespace genesis::project;
namespace render = genesis::render;
namespace ges_adapter = genesis::adapters::engine::ges;
using genesis::core::Rational;

// A minimal valid extension.toml: a single [[effect]] block with a GLSL body,
// so from_manifest populates body_paths_ and passes_for can read the shader.
constexpr const char *kMinimalManifest = R"(
[extension]
id = "test.packs"
name = "Test Packs"
version = 1
api = 1

[[effect]]
id = "genesis.cache"
name = "Cache"
kind = "effect"
body = "effect.glsl"
)";

// The same manifest without a `body` field, so the pack resolves to a pass
// whose source is nullptr (no GLSL to read).
constexpr const char *kMinimalManifestNoBody = R"(
[extension]
id = "test.packs"
name = "Test Packs"
version = 1
api = 1

[[effect]]
id = "genesis.cache"
name = "Cache"
kind = "effect"
)";

// A bare clip carrying one effect - what passes_for hands the catalogue.
Clip clip_with(std::string id, AppliedFilter effect)
{
    Clip clip;
    clip.id = std::move(id);
    clip.video_effects = { std::move(effect) };
    return clip;
}

// The body is read once and cached, so deleting effect.glsl after the first
// resolve must not change the next one. This is the hot-path defect the fix
// removes: before it, every passes_for re-read the file from disk.
void test_catalogue_caches_bodies()
{
    const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "genesis-effect-cache-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const std::filesystem::path manifest_path = root / "extension.toml";
    {
        std::ofstream manifest(manifest_path);
        manifest << kMinimalManifest;
    }
    {
        std::ofstream body(root / "effect.glsl");
        body << "void main () { gl_FragColor = vec4 (1.0, 0.0, 0.0, 1.0); }\n";
    }

    const render::Catalogue catalogue = render::Catalogue::from_manifest(manifest_path);
    const Clip clip = clip_with("c1", AppliedFilter::create("genesis.cache"));
    const render::ChainResolution first = catalogue.passes_for(clip, 0.0);
    check(first.passes.size() == 1, "the synthetic pack resolves one pass");
    check(first.skips.empty(), "no link is skipped");
    check(first.passes[0].source != nullptr && !first.passes[0].source->empty(),
          "the body is read from effect.glsl");

    std::filesystem::remove(root / "effect.glsl", ec);

    const render::ChainResolution second = catalogue.passes_for(clip, 0.0);
    check(second.passes.size() == 1, "the cached pass still resolves");
    check(second.passes[0].source != nullptr && *second.passes[0].source == *first.passes[0].source,
          "the body is cached, not re-read after the file is gone");

    std::filesystem::remove_all(root, ec);
}

// The "no body" fact is cached too: a pack whose effect.glsl is missing stays
// bodyless even if the file appears after the first resolve. This is the same
// cache, storing nullptr, so a missing body is not re-probed every frame.
void test_catalogue_caches_a_missing_body()
{
    const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "genesis-effect-nobody-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const std::filesystem::path manifest_path = root / "extension.toml";
    {
        std::ofstream manifest(manifest_path);
        manifest << kMinimalManifestNoBody;
    }

    const render::Catalogue catalogue = render::Catalogue::from_manifest(manifest_path);
    const Clip clip = clip_with("c1", AppliedFilter::create("genesis.cache"));
    const render::ChainResolution first = catalogue.passes_for(clip, 0.0);
    check(first.passes.size() == 1, "a bodyless pack still resolves a pass");
    check(first.passes[0].source == nullptr, "with no body");

    {
        std::ofstream body(root / "effect.glsl");
        body << "void main () { gl_FragColor = vec4 (1.0); }\n";
    }
    const render::ChainResolution second = catalogue.passes_for(clip, 0.0);
    check(second.passes[0].source == nullptr, "the missing-body fact is cached, not re-read");

    std::filesystem::remove_all(root, ec);
}

// The media file the asset request must actually load. Generated on demand as
// a one-second VP8/WebM (the only encoder guaranteed on this machine), with
// the Phase 0 spike file as a documented fallback. Returns an empty path when
// neither is possible, and the test then fails loudly rather than skip.
struct MediaFixture
{
    std::string path;
    bool owns_file = false;
};

MediaFixture media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-effect-application-fixture.webm";
    std::filesystem::remove(generated);
    const std::string command = "gst-launch-1.0 -q videotestsrc num-buffers=30 ! videoconvert ! "
                                "vp8enc ! webmmux ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    if (std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
        && std::filesystem::file_size(generated) > 0) {
        return MediaFixture{ generated.string(), true };
    }
    std::filesystem::remove(generated);
    const std::filesystem::path spike =
            std::filesystem::temp_directory_path() / "ges-spike" / "a.webm";
    if (std::filesystem::exists(spike)) {
        return MediaFixture{ spike.string(), false };
    }
    return MediaFixture{ "", false };
}

// How many elements of one factory the timeline's effect clips carry, reached
// the same way the builder reaches them: each clip's track elements' element,
// into the effect bin, counting by factory name recursively. GES tracks are
// not plain GstBin children, so a bare iterate_recurse over the timeline would
// not descend into them - the layer/clip walk is the only route that sees the
// effect bin.
std::size_t count_elements_in_bin(GstBin *bin, const char *factory_name)
{
    std::size_t count = 0;
    GstIterator *iterator = gst_bin_iterate_recurse(bin);
    GValue item = G_VALUE_INIT;
    bool done = false;
    while (!done) {
        switch (gst_iterator_next(iterator, &item)) {
        case GST_ITERATOR_OK: {
            GstElement *element = GST_ELEMENT(g_value_get_object(&item));
            GstElementFactory *factory = gst_element_get_factory(element);
            if (factory != nullptr
                && g_strcmp0(GST_OBJECT_NAME(GST_OBJECT(factory)), factory_name) == 0) {
                ++count;
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(iterator);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = true;
            break;
        }
    }
    g_value_unset(&item);
    gst_iterator_free(iterator);
    return count;
}

std::size_t count_elements(GESTimeline *timeline, const char *factory_name)
{
    std::size_t count = 0;
    GList *layers = ges_timeline_get_layers(timeline);
    for (GList *layer_it = layers; layer_it != nullptr; layer_it = layer_it->next) {
        GList *clips = ges_layer_get_clips(GES_LAYER(layer_it->data));
        for (GList *clip_it = clips; clip_it != nullptr; clip_it = clip_it->next) {
            GList *children = ges_container_get_children(GES_CONTAINER(clip_it->data), TRUE);
            for (GList *child_it = children; child_it != nullptr; child_it = child_it->next) {
                GstElement *element =
                        ges_track_element_get_element(GES_TRACK_ELEMENT(child_it->data));
                if (element != nullptr && GST_IS_BIN(element)) {
                    count += count_elements_in_bin(GST_BIN(element), factory_name);
                }
            }
            g_list_free(children);
        }
        g_list_free(clips);
    }
    g_list_free(layers);
    return count;
}

std::size_t count_glshaders(GESTimeline *timeline)
{
    return count_elements(timeline, "glshader");
}

// A layer clip (a treatment) must install a glshader chain in the built GES
// timeline: the effect application path end to end, host command -> flatten ->
// resolve -> GES graph.
void test_builder_installs_a_chain()
{
    const MediaFixture media = media_fixture();
    check(!media.path.empty(), "a media fixture is available");
    if (media.path.empty()) {
        return;
    }

    Editor editor;
    NewMedia item;
    item.path = media.path;
    item.name = "fixture.webm";
    item.duration = Rational{ 1, 1 };
    item.kind = MediaKind::Video;
    item.width = 640;
    item.height = 360;
    item.has_audio = false;
    const auto add_media = editor.apply(Command{ AddMedia{ std::move(item) } });
    check(add_media.has_value() && add_media->created_id.has_value(), "adds media");
    const std::string media_id = *add_media->created_id;
    const std::string track_id = editor.project().timelines[0].tracks[0].id;
    check(editor.apply(Command{ AddClip{ media_id, track_id, Rational{ 0, 1 }, false } })
                  .has_value(),
          "adds a clip");

    AddLayerClip layer;
    layer.effect_id = "genesis.invert";
    layer.start = Rational{ 0, 1 };
    layer.name = "Invert";
    const auto add_layer = editor.apply(Command{ std::move(layer) });
    check(add_layer.has_value() && add_layer->created_id.has_value(), "adds a layer clip");

    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(flat.size() == 2, "the media clip and the layer flatten");

    render::ExportRequest request;
    request.width = 640;
    request.height = 360;
    const render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(flat), { });
    check(built.treatments.size() == 1, "the layer becomes one treatment");
    check(built.chains.empty(), "a layer installs no per-clip chain");

    GESTimeline *timeline = ges_adapter::build_timeline(built);
    check(timeline != nullptr, "the GES timeline builds");
    if (timeline == nullptr) {
        if (media.owns_file) {
            std::filesystem::remove(media.path);
        }
        return;
    }

    check(count_glshaders(timeline) == 1, "one glshader for the one-pass invert layer");

    gst_object_unref(timeline);
    if (media.owns_file) {
        std::filesystem::remove(media.path);
    }
}

// A CPU-only per-clip effect (genesis.video-balance: a `[cpu]` fallback onto
// the stock `videobalance` element) must install that stock element in the
// built GES timeline instead of a glshader chain: the clip effect application
// path end to end, host command -> flatten -> resolve -> GES graph.
void test_builder_installs_a_cpu_fallback()
{
    const MediaFixture media = media_fixture();
    check(!media.path.empty(), "a media fixture is available");
    if (media.path.empty()) {
        return;
    }

    Editor editor;
    NewMedia item;
    item.path = media.path;
    item.name = "fixture.webm";
    item.duration = Rational{ 1, 1 };
    item.kind = MediaKind::Video;
    item.width = 640;
    item.height = 360;
    item.has_audio = false;
    const auto add_media = editor.apply(Command{ AddMedia{ std::move(item) } });
    check(add_media.has_value() && add_media->created_id.has_value(), "adds media");
    const std::string media_id = *add_media->created_id;
    const std::string track_id = editor.project().timelines[0].tracks[0].id;
    const auto add_clip =
            editor.apply(Command{ AddClip{ media_id, track_id, Rational{ 0, 1 }, false } });
    check(add_clip.has_value() && add_clip->created_id.has_value(), "adds a clip");
    const std::string clip_id = *add_clip->created_id;

    ClipPatch patch;
    patch.video_effects = { AppliedFilter::create("genesis.video-balance") };
    check(editor.apply(Command{ UpdateClip{ clip_id, std::move(patch) } }).has_value(),
          "sets the CPU-fallback effect");

    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor.project(), std::nullopt);
    check(flat.size() == 1, "one clip flattens");

    render::ExportRequest request;
    request.width = 640;
    request.height = 360;
    const render::BuiltTimeline built =
            render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                   std::span<const render::ExportClip>(flat), { });
    check(built.chains.size() == 1, "the clip's chain reaches the built timeline");

    GESTimeline *timeline = ges_adapter::build_timeline(built);
    check(timeline != nullptr, "the GES timeline builds");
    if (timeline == nullptr) {
        if (media.owns_file) {
            std::filesystem::remove(media.path);
        }
        return;
    }

    check(count_glshaders(timeline) == 0, "no glshader for a CPU-only effect");
    check(count_elements(timeline, "videobalance") == 1,
          "one videobalance element for the fallback");

    gst_object_unref(timeline);
    if (media.owns_file) {
        std::filesystem::remove(media.path);
    }
}

// The buffer probe's update logic: it re-resolves a treatment every frame but
// uploads only when the knobs actually moved. A keyed `stops` knob on
// genesis.exposure rides 0 -> 4 across the layer's span, so the same time is a
// no-op and a later time is a change.
void test_probe_updates_on_change_not_every_buffer()
{
    AppliedFilter exposure = AppliedFilter::create("genesis.exposure");
    exposure.keys["stops"] = std::vector<ParamKey>{
        ParamKey{ 0.0, 0.0, KeyEase::linear() },
        ParamKey{ 1.0, 4.0, KeyEase::linear() },
    };
    render::Treatment treatment{
        Rational{ 0, 1 }, Rational{ 10, 1 }, 0, { std::move(exposure) }, 1.0f, 0.0, 0.0
    };
    ges_adapter::TreatmentChain chain{ std::move(treatment) };

    // The first frame resolves and reports a change (last was empty). The
    // chain holds no glshaders, so nothing is uploaded - the test drives the
    // resolve-and-compare logic only.
    check(ges_adapter::update_uniforms(chain, Rational{ 0, 1 }), "the first frame is a change");
    check(chain.last.size() == 1, "one pass resolves");
    {
        GstStructure *uniforms = ges_adapter::uniforms_for(chain.last[0]);
        gfloat stops = -1.0f;
        gst_structure_get(uniforms, "stops", G_TYPE_FLOAT, &stops, nullptr);
        check(stops == 0.0f, "stops reads 0 at the start");
        gst_structure_free(uniforms);
    }

    check(!ges_adapter::update_uniforms(chain, Rational{ 0, 1 }), "an unchanged frame is a no-op");

    check(ges_adapter::update_uniforms(chain, Rational{ 10, 1 }),
          "a later frame with moved knobs is a change");
    {
        GstStructure *uniforms = ges_adapter::uniforms_for(chain.last[0]);
        gfloat stops = -1.0f;
        gst_structure_get(uniforms, "stops", G_TYPE_FLOAT, &stops, nullptr);
        check(stops == 4.0f, "stops reads 4 at the end");
        gst_structure_free(uniforms);
    }
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    ges_init();

    test_catalogue_caches_bodies();
    test_catalogue_caches_a_missing_body();
    test_builder_installs_a_chain();
    test_builder_installs_a_cpu_fallback();
    test_probe_updates_on_change_not_every_buffer();

    return genesis::test::summary();
}
