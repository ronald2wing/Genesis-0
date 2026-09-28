// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/Flatten.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>

#include "project/Project.h"
#include "project/model/Clip.h"
#include "project/model/Keyframe.h"
#include "project/model/Media.h"
#include "project/model/Speed.h"
#include "project/model/Timeline.h"
#include "project/model/Track.h"
#include "render/MediaCache.h"

namespace genesis::render {

using genesis::project::Clip;
using genesis::project::ClipKey;
using genesis::project::KeyProperty;
using genesis::project::MediaItem;
using genesis::project::Project;
using genesis::project::SpeedPoint;
using genesis::project::Timeline;
using genesis::project::Track;
using ModelClipKind = genesis::project::ClipKind;

namespace {

// The timeline to flatten: the named one, or the document's active one,
// falling back to the first - the same preference the UI's tab strip has.
const Timeline *pick_timeline(const Project &project, std::optional<std::string_view> timeline_id)
{
    const std::string_view wanted = timeline_id.value_or(project.active_timeline_id);
    const auto it =
            std::find_if(project.timelines.begin(), project.timelines.end(),
                         [wanted](const Timeline &timeline) { return timeline.id == wanted; });
    if (it != project.timelines.end()) {
        return &*it;
    }
    return project.timelines.empty() ? nullptr : &project.timelines.front();
}

} // namespace

std::vector<ExportClip> flatten_timeline(const Project &project,
                                         std::optional<std::string_view> timeline_id)
{
    return flatten_timeline_in(project, timeline_id, std::nullopt);
}

std::vector<ExportClip> flatten_timeline_in(const Project &project,
                                            std::optional<std::string_view> timeline_id,
                                            std::optional<std::filesystem::path> mask_root)
{
    const Timeline *timeline = pick_timeline(project, timeline_id);
    if (timeline == nullptr) {
        return { };
    }

    // One look-up map each so the per-clip scan below is O(1) instead of an
    // O(T) track find and an O(M) media find per clip. Built once here; the
    // output is byte-for-byte the same as the linear scans they replace.
    std::unordered_map<std::string, std::size_t> track_index;
    track_index.reserve(timeline->tracks.size());
    for (std::size_t i = 0; i < timeline->tracks.size(); ++i) {
        track_index.try_emplace(timeline->tracks[i].id, i);
    }
    std::unordered_map<std::string, const MediaItem *> media_index;
    media_index.reserve(project.media.size());
    for (const MediaItem &item : project.media) {
        media_index.try_emplace(item.id, &item);
    }

    std::vector<ExportClip> out;
    for (const Clip &clip : timeline->clips) {
        const auto track_it = track_index.find(clip.track_id);
        if (track_it == track_index.end()) {
            continue;
        }
        const std::size_t index = track_it->second;
        const Track &track = timeline->tracks[index];

        // A layer: no file, a chain over the stack beneath its track, its
        // opacity the strength and its fades the ramps.
        if (clip.kind == ModelClipKind::Layer) {
            ExportClip layer = ExportClip::blank(ClipKind::Layer, clip.start, clip.duration, index);
            layer.hidden = !track.visible;
            layer.muted = true;
            layer.volume = 0.0;
            layer.fade_in = clip.fade_in;
            layer.fade_out = clip.fade_out;
            layer.effects = clip.video_effects;
            layer.opacity = clip.opacity;
            layer.has_audio = false;
            out.push_back(std::move(layer));
            continue;
        }

        // A title: no file of its own. The host knows the words but must not
        // grow a font engine, so the clip id stands in as the path - the app
        // rasterises the title to a PNG and build_timeline swaps the id for
        // that picture (BuiltTimeline::title_images). A title the app could not
        // draw therefore keeps its id, matches no entry, and is built
        // unpictured rather than failing the timeline.
        //
        // Titles used to be dropped here, which meant a title clip could never
        // reach the engine at all - the rasteriser and the layout were both
        // written and tested, with nothing joining them to the render path.
        if (clip.kind == ModelClipKind::Text) {
            ExportClip title = ExportClip::blank(ClipKind::Image, clip.start, clip.duration, index);
            title.path = clip.id;
            title.hidden = !track.visible;
            title.has_audio = false;
            // The media branch copies the clip's filters; the title branch did
            // not, so a title's effect chain never reached BuiltTimeline::chains
            // and a title could never be revealed - its genesis.reveal filter was
            // silently dropped and it rendered fully opaque. Caught by the
            // end-to-end production-path test, not by any unit test.
            title.effects = clip.video_effects;
            out.push_back(std::move(title));
            continue;
        }

        const MediaItem *media = nullptr;
        const auto media_it = media_index.find(clip.media_id);
        if (media_it != media_index.end()) {
            media = media_it->second;
        }
        if (media == nullptr) {
            continue;
        }

        // Initialised at the declaration and given a `default` arm so a
        // kind this build does not know is skipped rather than read
        // uninitialised: the switch below is exhaustive today, but a new
        // ModelClipKind must not become undefined behaviour.
        ClipKind kind = ClipKind::Video;
        switch (clip.kind) {
        case ModelClipKind::Video:
            kind = ClipKind::Video;
            break;
        case ModelClipKind::Audio:
            kind = ClipKind::Audio;
            break;
        case ModelClipKind::Image:
            kind = ClipKind::Image;
            break;
        // Handled above; unreachable spelled as a skip so a new kind
        // fails soft.
        case ModelClipKind::Text:
        case ModelClipKind::Layer:
            continue;
        default:
            continue;
        }

        ExportClip flat = ExportClip::blank(kind, clip.start, clip.duration, index);
        flat.path = media->path;
        flat.audio_stream = clip.audio_stream;
        flat.color_range = media->color_range;
        flat.source_start = clip.source_start;
        flat.hidden = !track.visible;
        flat.muted = track.muted || clip.muted.value_or(false);
        flat.volume = export_base(clip, KeyProperty::Volume);
        flat.pan = clip.pan;
        flat.fade_in = clip.fade_in;
        flat.fade_out = clip.fade_out;
        // The FFmpeg filter string the Filters tab builds stays unported, so
        // `filter_chain` remains empty; the clip's audio filters ride
        // `audio_filters` (below) as applied-effect records instead.
        flat.speed = clip.speed;
        flat.preserve_pitch = clip.preserve_pitch;
        if (clip.speed_curve) {
            flat.speed_curve.reserve(clip.speed_curve->size());
            for (const SpeedPoint &point : *clip.speed_curve) {
                flat.speed_curve.emplace_back(point.at, point.speed);
            }
        }
        flat.animation = export_keys(clip);
        flat.flip_h = clip.flip_h;
        flat.flip_v = clip.flip_v;
        flat.reverse = clip.reverse;
        flat.blend = clip.blend;
        if (clip.crop && !clip.crop->is_none()) {
            flat.crop = std::array<double, 4>{ clip.crop->left, clip.crop->top, clip.crop->right,
                                               clip.crop->bottom };
        }
        flat.effects = clip.video_effects;
        flat.audio_filters = clip.filters;
        flat.scale = export_base(clip, KeyProperty::Scale);
        flat.offset_x = export_base(clip, KeyProperty::OffsetX);
        flat.offset_y = export_base(clip, KeyProperty::OffsetY);
        flat.rotation = export_base(clip, KeyProperty::Rotation);
        flat.stretch_x = clip.stretch_x;
        flat.stretch_y = clip.stretch_y;
        flat.opacity = export_base(clip, KeyProperty::Opacity);
        // Passed through unconditionally: `resolve_transitions` is the one
        // adjacency judge (frame/2 tolerance). A fixed 1/60 s gate here would
        // agree at 30fps and disagree at every other rate; resolving is the
        // kinder read of what the user placed.
        if (clip.transition_in) {
            TransitionSpec transition;
            transition.kind = clip.transition_in->id;
            transition.duration = clip.transition_in->duration;
            flat.transition = std::move(transition);
        }
        flat.media_width = media->width;
        flat.media_height = media->height;
        flat.has_audio = media->has_audio;
        flat.cutout = clip.cutout;
        if (clip.cutout && mask_root) {
            // A cutout with a known mask root names its mask directory and
            // carries the facts the engine needs to find and step the masks:
            // the root, the source they were keyed by, the subject/model, and
            // the decimation stride. The stride is not recorded anywhere yet -
            // generation is not wired to the app and the driver's default is
            // one mask per frame - so the spec pins stride 1 and records no
            // last frame, meaning a frame whose mask was never generated reads
            // absent rather than clamping.
            const genesis::ai::vision::MaskTarget target =
                    genesis::ai::vision::mask_target(*clip.cutout);
            flat.mask_dir = mask_root->string();
            genesis::ai::vision::MaskSpec spec;
            spec.root = *mask_root;
            spec.source = media->path;
            spec.source_size = cache_source_size(media->path);
            spec.subject = target.subject;
            spec.model_id = target.model_id;
            spec.width = media->width.value_or(0);
            spec.height = media->height.value_or(0);
            spec.stride = 1;
            spec.revision = genesis::ai::vision::mask_revision(*clip.cutout);
            flat.mask = std::move(spec);
        }
        out.push_back(std::move(flat));
    }
    return out;
}

double export_base(const Clip &clip, KeyProperty property)
{
    if (!clip.is_keyed(property)) {
        return clip.constant(property);
    }
    switch (property) {
    // A factor's neutral is one; an addition's is zero.
    case KeyProperty::Scale:
    case KeyProperty::Opacity:
    case KeyProperty::Volume:
        return 1.0;
    case KeyProperty::OffsetX:
    case KeyProperty::OffsetY:
    case KeyProperty::Rotation:
        return 0.0;
    }
    return 1.0;
}

std::vector<ExportKey> export_keys(const Clip &clip)
{
    std::vector<ExportKey> out;
    for (KeyProperty property : genesis::project::key_property_all) {
        if (!clip.is_keyed(property)) {
            continue;
        }
        for (const ClipKey &key : clip.keys_on(property)) {
            ExportKey entry;
            entry.property = property;
            entry.at = key.at;
            entry.value = key.value;
            entry.ease = static_cast<genesis::core::Ease>(key.ease);
            out.push_back(std::move(entry));
        }
    }
    return out;
}

} // namespace genesis::render
