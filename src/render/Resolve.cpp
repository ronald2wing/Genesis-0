// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-export/src/resolve.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include "render/Catalogue.h"
#include "render/ProxyCache.h"
#include "render/Resolve.h"
#include "render/ReverseCache.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

namespace genesis::render {

using genesis::core::Clip;
using genesis::core::ClipId;
using genesis::core::FrameRate;
using genesis::core::MediaRef;
using genesis::core::Rational;
using genesis::core::TrackId;
using genesis::core::TrackKind;
using genesis::core::Transform;
using genesis::project::AppliedFilter;

// Whether a flattened clip contributes sound to the mix: an audio clip, or a
// video clip whose file carries sound that is not muted/detached. A muted clip
// - track or clip level - contributes nothing audible; a video clip's embedded
// sound that was detached out of it is silent here and plays from the separate
// audio clip the detach created instead.
bool contributes_audio(const ExportClip &clip)
{
    if (clip.muted) {
        return false;
    }
    if (clip.kind == ClipKind::Audio) {
        return true;
    }
    if (clip.kind == ClipKind::Video) {
        return clip.has_audio.value_or(false);
    }
    return false;
}

BuiltTimeline build_timeline(const ExportRequest &request, FrameRate rate,
                             std::span<const ExportClip> visible,
                             std::vector<TransitionSpan> transitions,
                             std::unordered_map<std::string, std::string> title_images,
                             std::unordered_map<std::string, genesis::core::RevealMap> reveal_maps,
                             std::optional<std::filesystem::path> reverse_cache_root,
                             std::optional<std::filesystem::path> proxy_cache_root,
                             const Catalogue *catalogue)
{
    BuiltTimeline built(genesis::core::Timeline(request.width, request.height, rate));

    std::size_t lanes = 0;
    for (const ExportClip &clip : visible) {
        lanes = std::max(lanes, clip.track);
    }
    ++lanes;

    std::vector<TrackId> tracks;
    tracks.reserve(lanes);
    for (std::size_t index = 0; index < lanes; ++index) {
        tracks.push_back(built.timeline.add_track(
                genesis::core::Track("T" + std::to_string(index), TrackKind::Video)));
    }

    // An audio track per lane that carries sound, created only when that lane
    // has an audio-bearing clip, so a silent timeline keeps its video-only
    // shape and the export path's `has_audio` gate stays honest. An audio clip
    // lands on its lane's audio track; a video clip's embedded sound is
    // signalled through `built.audio` rather than a second clip, so the clip
    // count is unchanged and a video clip keeps one place in the timeline.
    std::vector<bool> lane_has_audio(lanes, false);
    for (const ExportClip &clip : visible) {
        if (clip.track < lanes && contributes_audio(clip)) {
            lane_has_audio[clip.track] = true;
        }
    }
    std::vector<std::optional<TrackId>> audio_tracks(lanes);
    for (std::size_t index = 0; index < lanes; ++index) {
        if (lane_has_audio[index]) {
            audio_tracks[index] = built.timeline.add_track(
                    genesis::core::Track("A" + std::to_string(index), TrackKind::Audio));
        }
    }

    for (const ExportClip &clip : visible) {
        // Time is already rational at this layer: the host's `ExportClip`
        // holds exact times, so the quantise seam the Rust applies here has
        // already happened at flatten time.
        const Rational start = clip.start;
        const Rational duration = clip.duration;
        if (duration.is_zero()) {
            continue;
        }

        // A layer has no pixels to decode: it is a treatment over the stack,
        // kept beside the timeline rather than in it.
        if (clip.kind == ClipKind::Layer) {
            std::vector<AppliedFilter> effects = shaded(clip.effects);
            if (!effects.empty()) {
                built.treatments.push_back(
                        Treatment{ start, start + duration, clip.track, std::move(effects),
                                   static_cast<float>(std::clamp(clip.opacity, 0.0, 1.0)),
                                   std::max(clip.fade_in.as_double(), 0.0),
                                   std::max(clip.fade_out.as_double(), 0.0), catalogue });
            }
            continue;
        }

        // A muted audio clip has nothing left to place: no pixels, and its
        // sound is switched off. Dropping it keeps the audio track for its
        // lane empty of silent decoders.
        if (clip.kind == ClipKind::Audio && clip.muted) {
            continue;
        }

        // An audio clip lands on its lane's audio track (which exists because
        // the clip contributes sound); everything else lands on its video
        // track. A video clip's embedded sound rides `built.audio` beside it.
        const bool on_audio_track = clip.kind == ClipKind::Audio;
        const TrackId target_track =
                on_audio_track ? *audio_tracks[clip.track] : tracks[clip.track];

        Clip engine_clip(MediaRef(clip.path), start, duration);
        engine_clip.set_source_start(clip.source_start);
        // The same clamp the audio path applies, so a 2x clip means the same
        // thing to picture and sound. The host model already clamps speed to
        // the engine's range (Clip::tidy), so there is no separate
        // `audio::clamp_speed` to call. A still has no meaningful rate.
        if (clip.kind != ClipKind::Image) {
            engine_clip.set_speed(Rational::approximate(clip.speed).value_or(Rational::ONE));
            // The Rust also sets `engine_clip.retime` from the clip's speed
            // curve; the engine `Clip` has no retime field yet (Phase 4), so
            // the curve rides the flattened clip until then.
        }
        // The Rust sets `engine_clip.animation = animation_of(...)`; the
        // engine `Clip` has no animation field yet (Phase 4), so the keys
        // ride beside the clip until the model grows one.
        engine_clip.set_blend(genesis::core::blend_parse(clip.blend));
        engine_clip.set_transform(Transform{ clip.scale, clip.offset_x, clip.offset_y,
                                             clip.rotation, clip.stretch_x, clip.stretch_y });
        engine_clip.set_opacity(static_cast<float>(std::clamp(clip.opacity, 0.0, 1.0)));
        // Quantised like every other time in the Rust; already rational here.
        engine_clip.set_video_fade_in(clip.video_fade_in);

        if (std::optional<ClipId> id =
                    built.timeline.add_clip(target_track, std::move(engine_clip))) {
            built.tracks.emplace(*id, clip.track);
            if (contributes_audio(clip)) {
                built.audio.emplace(
                        *id, ClipAudio{ clip.volume, clip.pan, clip.fade_in, clip.fade_out });
            }
            if (clip.kind == ClipKind::Image) {
                built.stills.insert(*id);
            }
            // A reversed video clip reads its source span backwards through a
            // generated copy rather than through the source. The span is the
            // source in-point plus the timeline duration - the engine does not
            // apply `speed` in the GES path, so the span is `duration`, not
            // `duration * speed`. The copy is only substituted when the reverse
            // file exists and is still fresh against the recorded key: a build
            // before generation finished (or a caller that supplied no root)
            // reads the source forward, exactly as it did before this existed.
            if (clip.reverse && clip.kind == ClipKind::Video && reverse_cache_root
                && !clip.path.empty()) {
                const double span_start = clip.source_start.as_double();
                const double span_duration = clip.duration.as_double();
                const std::filesystem::path reverse_file = reverse_cache_path(
                        *reverse_cache_root, clip.path, span_start, span_duration);
                const CacheKey key = reverse_key(clip.path, cache_source_size(clip.path),
                                                 span_start, span_duration);
                if (cache_is_fresh(reverse_file, key)) {
                    built.reverse_sources.emplace(*id, reverse_file.string());
                }
            }
            // A forward-playing video clip reads its whole source through a
            // generated downscale instead of the original. The proxy covers the
            // whole file and plays forward, so the source inpoint is kept. A
            // reversed clip is skipped: its reverse copy already stands in for
            // the source. The proxy is preview-only - the export caller passes
            // no proxy root, so export always reads originals.
            if (!clip.reverse && clip.kind == ClipKind::Video && proxy_cache_root
                && !clip.path.empty()) {
                const std::filesystem::path proxy_file =
                        proxy_cache_path(*proxy_cache_root, clip.path);
                const CacheKey key = proxy_key(clip.path, cache_source_size(clip.path));
                if (cache_is_fresh(proxy_file, key)) {
                    built.proxy_sources.emplace(*id, proxy_file.string());
                }
            }
            const std::optional<PlannedGeometry> planned = planned_geometry(clip);
            if (const std::optional<std::pair<std::uint32_t, std::uint32_t>> size =
                        fitted_size(request, clip, planned.has_value())) {
                built.decode_sizes.emplace(*id, *size);
            }
            if (planned) {
                built.geometry.emplace(*id, *planned);
            }
            if (clip.color_range) {
                built.ranges.emplace(*id, *clip.color_range);
            }
            const std::vector<AppliedFilter> effects = shaded(clip.effects);
            if (!effects.empty()) {
                built.chains.emplace(*id, effects);
            }
            const std::vector<AppliedFilter> audio = shaded(clip.audio_filters);
            if (!audio.empty()) {
                built.audio_chains.emplace(*id, audio);
            }
            if (clip.highlighted) {
                built.highlight = *id;
            }
            // A clip whose flatten carried a mask spec renders cut: the spec
            // rides into the engine as-is, so the builder steps and finds the
            // masks exactly as the driver wrote them. A cutout with no spec
            // (no mask root, or generation not wired) is absent and whole.
            if (clip.mask) {
                built.cutouts.emplace(*id, *clip.mask);
            }
        }
    }

    built.transitions = std::move(transitions);
    for (const ExportClip &clip : visible) {
        if (!clip.transition) {
            continue;
        }
        // The incoming clip carries the transition on the cut into it: its own
        // track is the span's `to_track`, its start the cut, and the
        // transition's duration the window. A shape the engine cannot express
        // is still emitted - the adapter's degrade path covers it, so resolve
        // must not silently drop it.
        built.transitions.push_back(TransitionSpan{ clip.start,
                                                    clip.start + clip.transition->duration,
                                                    clip.track,
                                                    clip.transition->kind,
                                                    { } });
    }

    // A title clip reaches the graph with its clip id standing in for a media
    // path. Carry forward each supplied picture, keyed by that id, so the
    // engine adapter builds the clip from the rasterised PNG. A title whose
    // id the caller did not supply - or that is not in this timeline - simply
    // keeps no entry and is built without a picture. The reveal map is carried
    // beside the picture, under the same key, so a clip with a map keeps both
    // and one with neither keeps neither.
    for (const ExportClip &clip : visible) {
        const auto supplied = title_images.find(clip.path);
        if (supplied != title_images.end()) {
            built.title_images.emplace(supplied->first, supplied->second);
        }
        const auto revealed = reveal_maps.find(clip.path);
        if (revealed != reveal_maps.end()) {
            built.reveal_maps.emplace(revealed->first, revealed->second);
        }
    }

    built.catalogue = catalogue;

    return built;
}

std::optional<PlannedGeometry> planned_geometry(const ExportClip &clip)
{
    genesis::project::Crop crop;
    if (clip.crop) {
        crop = genesis::project::Crop{ (*clip.crop)[0], (*clip.crop)[1], (*clip.crop)[2],
                                       (*clip.crop)[3] };
    }
    if (crop.is_none() && !clip.flip_h && !clip.flip_v) {
        return std::nullopt;
    }
    return PlannedGeometry{ crop, clip.flip_h, clip.flip_v };
}

std::vector<AppliedFilter> shaded(const std::vector<AppliedFilter> &effects)
{
    // The catalogue half - dropping a link whose package has no shader or is
    // not installed - belongs to the effect catalogue (concat-effects), which
    // is not ported yet. Until then every enabled link rides through; a link
    // no package answers to draws nothing in the renderer. Phase 4.
    std::vector<AppliedFilter> out;
    out.reserve(effects.size());
    for (const AppliedFilter &applied : effects) {
        if (applied.enabled) {
            out.push_back(applied);
        }
    }
    return out;
}

std::optional<genesis::core::Animation> animation_of(const std::vector<ExportKey> &keys)
{
    if (keys.empty()) {
        return std::nullopt;
    }
    std::array<std::vector<genesis::core::Key>, 6> tracks;
    for (const ExportKey &key : keys) {
        std::size_t slot = 0;
        switch (key.property) {
        case project::KeyProperty::Scale:
            slot = 0;
            break;
        case project::KeyProperty::OffsetX:
            slot = 1;
            break;
        case project::KeyProperty::OffsetY:
            slot = 2;
            break;
        case project::KeyProperty::Rotation:
            slot = 3;
            break;
        case project::KeyProperty::Opacity:
            slot = 4;
            break;
        case project::KeyProperty::Volume:
            slot = 5;
            break;
        }
        tracks[slot].push_back(genesis::core::Key{ key.at, key.value, key.ease });
    }
    genesis::core::Animation animation;
    animation.scale = genesis::core::KeyTrack(std::move(tracks[0]));
    animation.offset_x = genesis::core::KeyTrack(std::move(tracks[1]));
    animation.offset_y = genesis::core::KeyTrack(std::move(tracks[2]));
    animation.rotation = genesis::core::KeyTrack(std::move(tracks[3]));
    animation.opacity = genesis::core::KeyTrack(std::move(tracks[4]));
    animation.volume = genesis::core::KeyTrack(std::move(tracks[5]));
    if (animation.is_empty()) {
        return std::nullopt;
    }
    return animation;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>>
fitted_size(const ExportRequest &request, const ExportClip &clip, bool whole)
{
    const std::uint32_t source_width =
            clip.media_width && *clip.media_width > 0 ? *clip.media_width : 0;
    const std::uint32_t source_height =
            clip.media_height && *clip.media_height > 0 ? *clip.media_height : 0;
    if (source_width == 0 || source_height == 0) {
        return std::nullopt;
    }

    // What is left after the crop is what gets fitted.
    std::uint32_t media_width = source_width;
    std::uint32_t media_height = source_height;
    if (clip.crop) {
        const double left = (*clip.crop)[0];
        const double top = (*clip.crop)[1];
        const double right = (*clip.crop)[2];
        const double bottom = (*clip.crop)[3];
        media_width = static_cast<std::uint32_t>(std::max(
                std::round(static_cast<double>(media_width) * std::max(1.0 - left - right, 0.1)),
                2.0));
        media_height = static_cast<std::uint32_t>(std::max(
                std::round(static_cast<double>(media_height) * std::max(1.0 - top - bottom, 0.1)),
                2.0));
    }

    const double fit =
            std::min(static_cast<double>(request.width) / static_cast<double>(media_width),
                     static_cast<double>(request.height) / static_cast<double>(media_height));
    const std::uint32_t fit_width = whole ? source_width : media_width;
    const std::uint32_t fit_height = whole ? source_height : media_height;
    const std::uint32_t width = std::max(
            static_cast<std::uint32_t>(std::round(static_cast<double>(fit_width) * fit)), 2u);
    const std::uint32_t height = std::max(
            static_cast<std::uint32_t>(std::round(static_cast<double>(fit_height) * fit)), 2u);
    // Even, because a decoder asked for an odd width may round it itself and
    // then every frame read is misaligned by a pixel's worth of bytes.
    return std::pair{ width & ~std::uint32_t{ 1 }, height & ~std::uint32_t{ 1 } };
}

Rational quantise(double seconds, FrameRate rate)
{
    const double frames = std::round(seconds * rate.fps().as_double());
    const std::int64_t index = static_cast<std::int64_t>(std::max(frames, 0.0));
    return rate.time_of_frame(index);
}

std::vector<genesis::core::ShaderPass> Treatment::passes_at(Rational time) const
{
    // How far through the layer's span `time` is, 0..=1 - what the effect
    // catalogue resolves each keyed knob at.
    const double span = (end - start).as_double();
    const double at = span > 0.0 ? std::clamp((time - start).as_double() / span, 0.0, 1.0) : 0.0;

    // The catalogue resolves the chain at `at`. A treatment is a layer with no
    // clip id of its own, so the chain is handed over as a bare clip; a skip's
    // clip_id is empty, which is fine - a treatment's skips are not surfaced,
    // only the passes that resolve. A null catalogue is the builtin one.
    const Catalogue *resolver = catalogue;
    if (resolver == nullptr) {
        resolver = &Catalogue::builtin();
    }
    genesis::project::Clip clip;
    clip.video_effects = effects;
    return resolver->passes_for(clip, at).passes;
}

} // namespace genesis::render
