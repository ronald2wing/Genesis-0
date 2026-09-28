// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/session/GesBuilder.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ges/ges.h>
#include <gst/controller/gstinterpolationcontrolsource.h>
#include <gst/gst.h>

#include "adapters/engine/ges/effects/ClipEffects.h"
#include "adapters/engine/ges/effects/LutGrade.h"
#include "adapters/engine/ges/effects/MaskEffect.h"
#include "adapters/engine/ges/effects/RevealEffect.h"
#include "adapters/engine/ges/effects/ShaderChain.h"
#include "adapters/engine/ges/effects/TextureMix.h"
#include "effects/Resolve.h"
#include "project/model/Clip.h"
#include "project/model/Effects.h"
#include "render/Catalogue.h"
#include "render/Resolve.h"
#include "render/Transitions.h"

namespace genesis::adapters::engine::ges {

namespace {

using genesis::core::Clip;
using genesis::core::ClipId;
using genesis::core::FrameRate;
using genesis::core::Rational;
using genesis::core::TrackId;
using genesis::core::TrackKind;
using genesis::render::Axis;
using genesis::render::Coord;
using genesis::render::Shape;
using genesis::render::ShapeKind;

// The diagnostic id for a clip's chain, from its arena handle. The built
// timeline keys chains by core::ClipId, and the project::Clip a chain resolves
// against carries a string id only for diagnostics - the host's own "cN" string
// does not survive into BuiltTimeline, so the handle's index and generation are
// the stable label here.
std::string clip_label(ClipId id)
{
    return "clip#" + std::to_string(id.index()) + "@" + std::to_string(id.generation());
}

// A clip's effect chain pending configuration: installed before add_track
// (top effects must land in the clip before its track element is created),
// configured after add_track materialises their bins and glshaders.
struct PendingClipEffects
{
    GESClip *clip = nullptr;
    // Owned until the probe's destroy notify takes it on configure success;
    // on failure the unique_ptr frees it when the vector unwinds.
    std::unique_ptr<ClipEffectChain> chain;
};

// The exact rational -> GES nanosecond seam. `as_double()` is deliberately
// lossy elsewhere; here it is the documented engine-argument seam, and the
// value is never converted back.
//
// A non-finite or overflowing value must never reach GES: `llround` of a NaN
// or an out-of-range double is undefined and lands on G_MININT64, which GES
// then hands to the NLE object's `duration` property (a gint64 bounded below
// by 0) and trips a GObject range warning. Clamp to the valid GstClockTime
// range instead, so one degenerate duration cannot poison the build.
GstClockTime to_ns(Rational time)
{
    const double ns = time.as_double() * GST_SECOND;
    if (!std::isfinite(ns) || ns <= 0.0) {
        return 0;
    }
    if (ns >= static_cast<double>(G_MAXINT64)) {
        return static_cast<GstClockTime>(G_MAXINT64);
    }
    return static_cast<GstClockTime>(std::llround(ns));
}

// A clip's audio facts, collected in the clip loop and applied after add_track
// materialises the audio source track element they ride on. The gain and pan
// are constants; the head/tail ramps ride a `direct` control source.
struct PendingAudio
{
    GESClip *clip = nullptr;
    double volume = 1.0;
    double pan = 0.0;
    Rational fade_in = Rational::ZERO;
    Rational fade_out = Rational::ZERO;
    Rational duration = Rational::ONE;
    // The clip's resolved audio effects, in caller order: each a stock element
    // plus its property values, applied on the audio path after the pan so they
    // run closer to the source.
    std::vector<genesis::effects::ResolvedAudio> audio_effects;
};

// Sets one child property of `element` from a resolved double, typed by the
// property's registered spec so an Int, Enum, Boolean or Float property is not
// fed a double (g_object_set refuses a type mismatch). The schema stores every
// value as a double, so the setter narrows it to the property's own fundamental
// type here. An unknown property, or a type no double can set (strings, boxes),
// is reported and skipped - a bad pack manifest degrades to a no-op, never a
// failed build.
void set_child_property_typed(GESTimelineElement *element, const char *key, double value)
{
    guint count = 0;
    GParamSpec **specs = ges_timeline_element_list_children_properties(element, &count);
    GParamSpec *spec = nullptr;
    for (guint i = 0; i < count; ++i) {
        if (std::strcmp(g_param_spec_get_name(specs[i]), key) == 0) {
            spec = specs[i];
            break;
        }
    }
    g_free(specs);
    if (spec == nullptr) {
        g_warning("audio effect: no child property `%s`; skipped", key);
        return;
    }

    const GType type = G_PARAM_SPEC_VALUE_TYPE(spec);
    GValue gvalue = G_VALUE_INIT;
    g_value_init(&gvalue, type);
    switch (G_TYPE_FUNDAMENTAL(type)) {
    case G_TYPE_BOOLEAN:
        g_value_set_boolean(&gvalue, value >= 0.5);
        break;
    case G_TYPE_ENUM:
        g_value_set_enum(&gvalue, static_cast<gint>(std::llround(value)));
        break;
    case G_TYPE_INT:
        g_value_set_int(&gvalue, static_cast<gint>(std::llround(value)));
        break;
    case G_TYPE_UINT:
        g_value_set_uint(&gvalue, static_cast<guint>(std::llround(value)));
        break;
    case G_TYPE_INT64:
        g_value_set_int64(&gvalue, static_cast<gint64>(std::llround(value)));
        break;
    case G_TYPE_UINT64:
        g_value_set_uint64(&gvalue, static_cast<guint64>(std::llround(value)));
        break;
    case G_TYPE_FLOAT:
        g_value_set_float(&gvalue, static_cast<gfloat>(value));
        break;
    case G_TYPE_DOUBLE:
        g_value_set_double(&gvalue, value);
        break;
    default:
        g_warning("audio effect: property `%s` has type `%s`, which a "
                  "double cannot set; skipped",
                  key, G_PARAM_SPEC_TYPE_NAME(spec));
        g_value_unset(&gvalue);
        return;
    }
    GError *error = nullptr;
    ges_timeline_element_set_child_property_full(element, key, &gvalue, &error);
    g_value_unset(&gvalue);
    g_clear_error(&error);
}

// Applies a clip's audio facts to the audio source track element add_track
// materialised for it: the mixer gain, the constant stereo pan (an
// audiopanorama top effect), and the head/tail ramps (a `direct` control
// source whose timestamps are the source's own media time, so 0 is the clip's
// in-point). A clip the resolver marked silent has no audio source element and
// is a no-op.
void apply_clip_audio(const PendingAudio &audio)
{
    GESClip *clip = audio.clip;
    if (clip == nullptr) {
        return;
    }

    // The audio source track element is the clip's child add_track created for
    // its sound; the top effects are GESEffect children and never match.
    GESTrackElement *audio_source = nullptr;
    GList *children = ges_container_get_children(GES_CONTAINER(clip), TRUE);
    for (GList *it = children; it != nullptr; it = it->next) {
        if (GES_IS_AUDIO_SOURCE(it->data)) {
            audio_source = GES_TRACK_ELEMENT(it->data);
            break;
        }
    }
    g_list_free(children);
    if (audio_source == nullptr) {
        return;
    }

    GError *error = nullptr;

    // Gain: the static mixer level, unity unless the person changed it.
    GValue volume = G_VALUE_INIT;
    g_value_init(&volume, G_TYPE_DOUBLE);
    g_value_set_double(&volume, audio.volume);
    ges_timeline_element_set_child_property_full(GES_TIMELINE_ELEMENT(audio_source), "volume",
                                                 &volume, &error);
    g_value_unset(&volume);
    g_clear_error(&error);

    // Pan: a constant stereo position. Only installed when off-centre, so a
    // centred clip keeps no extra element. The audiopanorama reads its
    // `panorama` in [-1, 1] - the host's own convention - so the value rides
    // through unchanged.
    if (audio.pan != 0.0) {
        GESEffect *pan = ges_effect_new("audiopanorama");
        if (pan != nullptr) {
            // Hold the floating reference across the add, as the clip effects
            // do: ges_clip_add_top_effect sinks on success and may revert
            // without sinking on failure, so a strong reference here keeps the
            // effect alive over both.
            g_object_ref_sink(pan);
            GValue panorama = G_VALUE_INIT;
            g_value_init(&panorama, G_TYPE_FLOAT);
            g_value_set_float(&panorama, static_cast<gfloat>(audio.pan));
            ges_timeline_element_set_child_property_full(GES_TIMELINE_ELEMENT(pan), "panorama",
                                                         &panorama, &error);
            g_value_unset(&panorama);
            g_clear_error(&error);
            ges_clip_add_top_effect(clip, GES_BASE_EFFECT(pan), -1, &error);
            g_clear_error(&error);
            gst_object_unref(pan);
        }
    }

    // Audio effects: the clip's resolved backends, added after the pan so each
    // lands at a higher priority and runs closer to the source (source ->
    // filters -> pan -> volume -> sink). Added in reverse order so the
    // manifest's first effect runs first, closest to the source, exactly as the
    // video path orders its passes.
    for (auto it = audio.audio_effects.rbegin(); it != audio.audio_effects.rend(); ++it) {
        GESEffect *effect = ges_effect_new(it->element.c_str());
        if (effect == nullptr) {
            g_warning("audio effect `%s` could not be created; skipped", it->element.c_str());
            continue;
        }
        // Hold the floating reference across the add, as the pan does.
        g_object_ref_sink(effect);
        for (const genesis::effects::CpuFallbackValue &value : it->values) {
            set_child_property_typed(GES_TIMELINE_ELEMENT(effect), value.key.c_str(), value.value);
        }
        GError *error = nullptr;
        ges_clip_add_top_effect(clip, GES_BASE_EFFECT(effect), -1, &error);
        g_clear_error(&error);
        gst_object_unref(effect);
    }

    // Fades: a linear volume ramp at each end. The control source holds the
    // nearest control point's value outside its own range, so between the two
    // ramps the level stays at `volume` - the gain set above.
    const GstClockTime fade_in_ns = to_ns(audio.fade_in);
    const GstClockTime fade_out_ns = to_ns(audio.fade_out);
    if (fade_in_ns == 0 && fade_out_ns == 0) {
        return;
    }
    const GstClockTime duration_ns = to_ns(audio.duration);
    GstControlSource *source = gst_interpolation_control_source_new();
    g_object_set(source, "mode", GST_INTERPOLATION_MODE_LINEAR, nullptr);
    GstTimedValueControlSource *timed = GST_TIMED_VALUE_CONTROL_SOURCE(source);
    if (fade_in_ns > 0) {
        gst_timed_value_control_source_set(timed, 0, 0.0);
        gst_timed_value_control_source_set(timed, fade_in_ns, audio.volume);
    }
    if (fade_out_ns > 0) {
        gst_timed_value_control_source_set(timed, duration_ns - fade_out_ns, audio.volume);
        gst_timed_value_control_source_set(timed, duration_ns, 0.0);
    }
    ges_track_element_set_control_source(audio_source, source, "volume", "direct");
    gst_object_unref(source);
}

// Resolves a clip's audio effect chain into the elements and property values
// the audio path runs. Each filter resolves through the built catalogue (or the
// builtin when null); a filter whose pack is unknown, whose parameters it
// cannot declare, or whose element this host lacks is reported and skipped (an
// R9 degrade, never fatal) - the clip's audio still plays, minus the effect.
// Resolution is at fraction 0 (the clip start), the same build-time seam the
// video chain uses.
std::vector<genesis::effects::ResolvedAudio>
resolve_audio_effects(const std::vector<genesis::project::AppliedFilter> &filters,
                      const std::string &label, const genesis::render::Catalogue *catalogue)
{
    std::vector<genesis::effects::ResolvedAudio> resolved;
    if (filters.empty()) {
        return resolved;
    }
    const genesis::render::Catalogue &effective =
            catalogue != nullptr ? *catalogue : genesis::render::Catalogue::builtin();
    resolved.reserve(filters.size());
    for (const genesis::project::AppliedFilter &filter : filters) {
        const genesis::effects::Pack *pack = effective.pack(filter.id);
        if (pack == nullptr) {
            g_warning("clip %s: skipping audio effect '%s': no such pack", label.c_str(),
                      filter.id.c_str());
            continue;
        }
        const std::optional<genesis::effects::ResolvedAudio> effect =
                genesis::effects::resolve_audio(*pack, filter, 0.0);
        if (!effect) {
            g_warning("clip %s: skipping audio effect '%s': cannot resolve", label.c_str(),
                      filter.id.c_str());
            continue;
        }
        GstElementFactory *factory = gst_element_factory_find(effect->element.c_str());
        if (factory == nullptr) {
            g_warning("clip %s: skipping audio effect '%s': element '%s' is "
                      "not available",
                      label.c_str(), filter.id.c_str(), effect->element.c_str());
            continue;
        }
        gst_object_unref(factory);
        resolved.push_back(std::move(*effect));
    }
    return resolved;
}

// The supported-formats a clip is added with, from its host track kind and
// whether it carries sound: an audio clip is sound only; a video clip carries
// picture and sound when the resolver marked it audio-bearing (`has_audio`),
// and picture only otherwise. This is what makes GES create an audio track
// element for a video clip's embedded sound without a second clip.
GESTrackType supported_formats_of(TrackKind kind, bool has_audio)
{
    if (kind == TrackKind::Audio) {
        return GES_TRACK_TYPE_AUDIO;
    }
    return has_audio ? static_cast<GESTrackType>(GES_TRACK_TYPE_VIDEO | GES_TRACK_TYPE_AUDIO)
                     : GES_TRACK_TYPE_VIDEO;
}

// One in-flight asset request, completed synchronously on a GMainLoop: the
// sync ges_asset_request returns null for an uncached asset (by design), so
// the async form plus a loop is how an asset actually loads.
struct AssetRequest
{
    GMainLoop *loop = nullptr;
    GESAsset *asset = nullptr;
    GError *error = nullptr;
};

void on_asset_ready(GObject *, GAsyncResult *result, gpointer user_data)
{
    auto *request = static_cast<AssetRequest *>(user_data);
    request->asset = ges_asset_request_finish(result, &request->error);
    g_main_loop_quit(request->loop);
}

// Loads the clip asset for `uri`, or nullptr on failure. The caller owns the
// returned reference (gst_object_unref).
GESAsset *request_asset(const gchar *uri)
{
    AssetRequest request;
    request.loop = g_main_loop_new(nullptr, FALSE);
    ges_asset_request_async(GES_TYPE_URI_CLIP, uri, nullptr, on_asset_ready, &request);
    g_main_loop_run(request.loop);
    g_main_loop_unref(request.loop);
    if (request.error != nullptr) {
        g_error_free(request.error);
        return nullptr;
    }
    return request.asset;
}

// The standard GES transition a transition id stands in for, or nullopt where
// GES has no type close enough to draw it. Only the ids a standard type
// actually matches are mapped; the rest fall back to the dissolve the
// incoming clip already carries, exactly like a missing shader.
//
// The id is tried as an FFmpeg xfade name through the shape table, so a
// packaged transition whose name is an xfade shape reaches its GES counterpart
// and a namespaced package id without the catalogue does not.
//
// Mapped: Cross->CROSSFADE; Clock->CLOCK_CW12; Iris/Box->IRIS_RECT (GES has
// one expanding rectangle, no separate circle, so the crop and the close are
// the same type - a caveat); Wipe Left/Right->BAR_WIPE_LR and Up/Down->
// BAR_WIPE_TB (GES bars sweep one fixed direction each, so the opposite
// direction is the same type with the direction caveat documented); Wipe
// TopLeft->DIAGONAL_TL and TopRight->DIAGONAL_TR; Bars->BARNDOOR_V for the X
// axis and BARNDOOR_H for the Y axis.
//
// Gaps (nullopt): ThroughBlack/ThroughWhite (no through-colour standard type;
// the fade itself is a colour modulation GES has no element for), Slide
// (GES bars move an edge, not a picture), Zoom and Blocks (no spatial type),
// and the BottomLeft/BottomRight diagonal wipes (no reverse-diagonal type).
std::optional<GESVideoStandardTransitionType> transition_type_of(std::string_view id)
{
    // The xfade names, through the same shape table the CPU compositor uses.
    const std::optional<Shape> shape = genesis::render::named(id);
    if (!shape) {
        return std::nullopt;
    }
    switch (shape->kind) {
    case ShapeKind::Cross:
        return GES_VIDEO_STANDARD_TRANSITION_TYPE_CROSSFADE;
    case ShapeKind::Clock:
        return GES_VIDEO_STANDARD_TRANSITION_TYPE_CLOCK_CW12;
    case ShapeKind::Iris:
    case ShapeKind::Box:
        return GES_VIDEO_STANDARD_TRANSITION_TYPE_IRIS_RECT;
    case ShapeKind::Wipe:
        switch (shape->coord) {
        case Coord::Left:
        case Coord::Right:
            return GES_VIDEO_STANDARD_TRANSITION_TYPE_BAR_WIPE_LR;
        case Coord::Up:
        case Coord::Down:
            return GES_VIDEO_STANDARD_TRANSITION_TYPE_BAR_WIPE_TB;
        case Coord::TopLeft:
            return GES_VIDEO_STANDARD_TRANSITION_TYPE_DIAGONAL_TL;
        case Coord::TopRight:
            return GES_VIDEO_STANDARD_TRANSITION_TYPE_DIAGONAL_TR;
        case Coord::BottomLeft:
        case Coord::BottomRight:
            return std::nullopt;
        }
        return std::nullopt;
    case ShapeKind::Bars:
        return shape->axis == Axis::X ? GES_VIDEO_STANDARD_TRANSITION_TYPE_BARNDOOR_V
                                      : GES_VIDEO_STANDARD_TRANSITION_TYPE_BARNDOOR_H;
    case ShapeKind::ThroughBlack:
    case ShapeKind::ThroughWhite:
    case ShapeKind::Slide:
    case ShapeKind::Zoom:
    case ShapeKind::Blocks:
        return std::nullopt;
    }
    return std::nullopt;
}

// A treatment resolved at build time: its passes and their glshader fragment
// sources, or nullopt when any pass cannot run as one glshader (the
// all-or-nothing refusal `append_shader_chain` applies). The sources are
// composed here, not baked into the bin description, so the description stays
// a bare "glshader name=..." list and the fragments are set once the
// glshaders materialise.
struct ResolvedTreatment
{
    std::vector<genesis::core::ShaderPass> passes;
    std::vector<std::string> sources;
};

std::optional<ResolvedTreatment> resolve_treatment(const genesis::render::Treatment &treatment)
{
    ResolvedTreatment resolved;
    resolved.passes = treatment.passes_at(treatment.start);
    if (resolved.passes.empty()) {
        return std::nullopt;
    }
    for (const genesis::core::ShaderPass &pass : resolved.passes) {
        if (!pass_is_expressible(pass)) {
            return std::nullopt;
        }
        // A LUT pass composes its fragment through the lutgrade convention;
        // every other pass stays on glshader.
        if (pass.lut.has_value()) {
            resolved.sources.push_back(std::move(fragment_source_lut_for(pass).value()));
        } else {
            resolved.sources.push_back(std::move(fragment_source_for(pass).value()));
        }
    }
    return resolved;
}

// The bin description `ges_effect_clip_new` turns into a chain of named
// elements: "glshader name=t0 ! glshader name=t1" for two plain passes, with
// "lutgrade name=tN" substituted for any pass carrying a resolved 3D LUT. The
// names are what the post-add pass looks the elements up by.
std::string shader_chain_description(const std::vector<genesis::core::ShaderPass> &passes)
{
    std::string description;
    for (std::size_t i = 0; i < passes.size(); ++i) {
        if (!description.empty()) {
            description += " ! ";
        }
        description += passes[i].lut.has_value() ? "lutgrade name=t" : "glshader name=t";
        description += std::to_string(i);
    }
    return description;
}

// A treatment's effect clip, held until add_track materialises the effect's
// bin and the named glshaders can be wired and probed.
struct PendingTreatment
{
    GESClip *clip = nullptr;
    std::vector<genesis::core::ShaderPass> passes;
    std::vector<std::string> sources;
    // Owned until the probe's destroy notify takes it; the probe then frees it
    // when the glshader's pad is disposed (the timeline's teardown).
    std::unique_ptr<TreatmentChain> chain;
};

// The GstBin the effect clip's child GESEffect wraps, or nullptr before
// add_track or when the effect has no element. Borrowed from the clip.
GstElement *effect_bin_of(GESClip *clip)
{
    GList *children = ges_container_get_children(GES_CONTAINER(clip), TRUE);
    GstElement *bin = nullptr;
    for (GList *it = children; it != nullptr; it = it->next) {
        GstElement *element = ges_track_element_get_element(GES_TRACK_ELEMENT(it->data));
        if (element != nullptr && GST_IS_BIN(element)) {
            bin = element;
            break;
        }
    }
    g_list_free(children);
    return bin;
}

// The probe's destroy notify: frees the chain the probe held. The chain's
// `shaders` are borrowed from the effect bin, so nothing is unreffed here - the
// bin (and the glshaders it owns) outlives the chain.
void free_treatment_chain(void *data)
{
    delete static_cast<TreatmentChain *>(data);
}

// The pass-through body a reveal bin uses when the resolver left `source`
// empty: reads the picture (tex0) unchanged. Unlike the glshader path's
// identity (ShaderChain.cpp kIdentityBody), this names tex0 - the texturemix
// owns the prelude and binds tex0 (picture) and tex1 (map), so the body must
// never reference glshader's single `tex`.
constexpr std::string_view kRevealIdentityBody =
        "void main () { gl_FragColor = texture2D (tex0, v_texcoord); }";

// The reveal pass the builder hands make_reveal_effect: the resolver never
// produces a reveal pass (it leaves reveal_map unset and there is no shipped
// genesis.reveal pack), so the builder lays the uniform block out itself - one
// float `progress` at offset 0 - and attaches the baked map beside the picture
// the reveal samples. The progress is the filter's value at the clip start (its
// constant, or the first key of a ride), the same start-fraction resolve
// install_clip_effects applies to the ordinary chain.
genesis::core::ShaderPass reveal_pass_for(const genesis::project::AppliedFilter &filter,
                                          const genesis::core::RevealMap &map)
{
    genesis::core::ShaderPass pass;
    pass.package = kRevealFilterId;
    pass.key = "genesis.reveal@builtin";
    pass.source = std::make_shared<const std::string>(kRevealBody);
    pass.reveal_map = map;
    pass.fields = { { "progress", "float", 0, 4 } };
    pass.params = std::vector<std::uint8_t>(genesis::core::ShaderPass::MIN_PARAMS, 0);
    const float progress = static_cast<float>(filter.value_at("progress", 0.0, 0.0));
    std::memcpy(pass.params.data(), &progress, sizeof(progress));
    pass.values["progress"] = progress;
    return pass;
}

} // namespace

GESTimeline *build_timeline(const genesis::render::BuiltTimeline &built)
{
    GESTimeline *timeline = ges_timeline_new();
    if (timeline == nullptr) {
        return nullptr;
    }

    // The lutgrade element must be registered before any description naming it
    // is parsed (the clip-effect and treatment effect clips below). Idempotent;
    // a failure here only bites when a LUT pass is actually routed.
    register_lut_grade();

    // One layer per host track, bottom-first, so track 0 composites first.
    // The layers are kept beside the loop so a transition can find its
    // incoming clip's layer by the track index the span carries.
    const genesis::core::Timeline &host = built.timeline;
    std::vector<GESLayer *> layers;
    layers.reserve(host.track_ids().size());
    // Clip effect chains installed in the clip loop below and configured after
    // add_track materialises their top effects' bins and glshaders.
    std::vector<PendingClipEffects> clip_effects;
    // Audio facts collected in the clip loop below and applied after add_track
    // materialises the audio source track elements they ride on.
    std::vector<PendingAudio> pending_audio;
    for (const TrackId track_id : host.track_ids()) {
        const genesis::core::Track *track = host.track(track_id);
        if (track == nullptr) {
            continue;
        }
        GESLayer *layer = ges_timeline_append_layer(timeline);
        layers.push_back(layer);

        for (const ClipId clip_id : track->clips()) {
            const Clip *clip = host.clip(clip_id);
            if (clip == nullptr) {
                continue;
            }
            // A clip with no media has nothing to decode, so nothing to add.
            const std::string &path = clip->media().path();
            if (path.empty()) {
                continue;
            }
            // A zero-duration clip has no span to place. GES would add it with
            // a zero stop, and the preroll seek then trips GStreamer's
            // "start <= stop" assertion; skip it so one degenerate clip cannot
            // fail the whole build.
            if (clip->duration() <= Rational::ZERO) {
                continue;
            }

            // A title clip's media path is its clip id, not a file; its
            // picture is the PNG the app rasterised under that id. Substitute
            // it when present, so the clip decodes like the stills do. A title
            // with no entry - or whose PNG has since gone missing - falls back
            // to a path that loads as nothing, leaving the clip unpictured
            // rather than failing the build.
            //
            // A reversed clip reads its generated reverse copy instead of the
            // source. The copy covers exactly the reversed span and plays from
            // its own start, so the source inpoint is dropped to zero; every
            // other clip keeps its source inpoint.
            //
            // A forward-playing clip with a fresh proxy reads its generated
            // downscale instead of the source. The proxy covers the whole file
            // and plays forward, so the source inpoint is kept. Reverse wins:
            // a reversed clip never carries a proxy entry, so the reverse
            // branch is checked first and the proxy only applies to the rest.
            const auto reverse = built.reverse_sources.find(clip_id);
            const auto proxy = built.proxy_sources.find(clip_id);
            const auto title_image = built.title_images.find(path);
            const std::string &asset_path = reverse != built.reverse_sources.end()
                    ? reverse->second
                    : (proxy != built.proxy_sources.end()
                               ? proxy->second
                               : (title_image != built.title_images.end() ? title_image->second
                                                                          : path));
            const Rational inpoint =
                    reverse != built.reverse_sources.end() ? Rational::ZERO : clip->source_start();

            // GES keys URI-clip assets on a file:// URI, not a bare path.
            gchar *uri = gst_filename_to_uri(asset_path.c_str(), nullptr);
            GESAsset *asset = uri != nullptr ? request_asset(uri) : nullptr;
            g_free(uri);
            if (asset == nullptr) {
                // One unloadable asset does not poison the whole build: skip
                // it and keep the clips that did load. A caller that needs the
                // failure can compare clip counts against the host graph.
                continue;
            }
            // A video clip carrying sound is added with both picture and audio
            // supported formats, so GES creates an audio track element for its
            // embedded sound without a second clip; a silent clip gets its
            // picture only.
            const GESTrackType track_type =
                    supported_formats_of(track->kind(), built.audio.contains(clip_id));
            GESClip *ges_clip =
                    ges_layer_add_asset(layer, asset, to_ns(clip->start()), to_ns(inpoint),
                                        to_ns(clip->duration()), track_type);
            // The layer owns the extracted clip; our asset reference is ours
            // to release either way.
            gst_object_unref(asset);
            if (ges_clip == nullptr) {
                continue;
            }
            // A still plays its one frame looping; GES must be told it is an
            // image rather than a one-frame video.
            if (built.stills.contains(clip_id)) {
                ges_uri_clip_set_is_image(GES_URI_CLIP(ges_clip), TRUE);
            }

            // An audio-bearing clip's gain, pan and fades ride the audio source
            // track element add_track will create; collect them here and apply
            // them after add_track.
            const auto audio_it = built.audio.find(clip_id);
            if (audio_it != built.audio.end()) {
                // The clip's resolved audio backends, one per enabled audio
                // filter, from the same audio_chains map the resolver filled
                // beside `chains`. Resolution is at fraction 0 (the clip start),
                // the same build-time seam the video chain uses; a filter whose
                // pack is unknown, undeclared or unavailable is skipped by
                // resolve_audio_effects itself (an R9 degrade, never fatal).
                std::vector<genesis::effects::ResolvedAudio> audio_effects;
                const auto audio_chain_it = built.audio_chains.find(clip_id);
                if (audio_chain_it != built.audio_chains.end()) {
                    audio_effects = resolve_audio_effects(audio_chain_it->second,
                                                          clip_label(clip_id), built.catalogue);
                }
                pending_audio.push_back(
                        PendingAudio{ ges_clip, audio_it->second.volume, audio_it->second.pan,
                                      audio_it->second.fade_in, audio_it->second.fade_out,
                                      clip->duration(), std::move(audio_effects) });
            }

            // A clip whose flatten carried a mask spec renders cut: install the
            // mask effect before the ordinary chain so GES applies it last
            // (closest to the sink), masking the already-graded picture - a
            // grade applied after it would repaint the transparent cut pixels.
            // The mask is keyed by the original source frame, so a reversed,
            // proxied, or non-unit-speed clip (whose picture-buffer time no
            // longer maps one-to-one onto source frames) is reported and left
            // whole rather than cut with the wrong frame.
            const auto cutout_it = built.cutouts.find(clip_id);
            if (cutout_it != built.cutouts.end()) {
                const bool masked_ok = !built.reverse_sources.contains(clip_id)
                        && !built.proxy_sources.contains(clip_id) && clip->speed() == Rational::ONE;
                if (!masked_ok) {
                    g_warning("clip %s: mask skipped (reversed, proxied, or "
                              "non-unit speed changes the source-frame mapping)",
                              clip_label(clip_id).c_str());
                } else {
                    GESBaseEffect *mask = make_mask_effect(cutout_it->second, host.frame_rate());
                    // make_mask_effect returns a floating reference (or null);
                    // install_mask_effect sinks and drops its own reference
                    // either way, so both branches are leak-free.
                    if (mask == nullptr || !install_mask_effect(ges_clip, mask)) {
                        g_warning("clip %s: could not build the mask effect; skipped",
                                  clip_label(clip_id).c_str());
                    }
                }
            }

            // Per-clip effects: the clip's chain from `built.chains`, installed
            // as GES top effects before add_track (they must land in the clip
            // before its track element is created). The title-only reveal filter
            // is pulled out and built as its own custom effect; every other
            // filter installs through the ordinary glshader path. A non-unit
            // speed would mis-time keyed animation (the clip-local fraction
            // assumes unit speed), so the ordinary chain is reported and skipped
            // rather than rendered wrong - the reveal is a static mask and is
            // not affected. The ordinary chain is configured - fragment,
            // uniforms, probe - after add_track below.
            const auto chain_it = built.chains.find(clip_id);
            if (chain_it == built.chains.end()) {
                continue;
            }
            std::optional<genesis::project::AppliedFilter> reveal_filter;
            std::vector<genesis::project::AppliedFilter> effects;
            effects.reserve(chain_it->second.size());
            for (const genesis::project::AppliedFilter &filter : chain_it->second) {
                if (filter.id == kRevealFilterId) {
                    if (reveal_filter.has_value()) {
                        g_warning("clip %s: more than one '%s' filter; using the "
                                  "first",
                                  clip_label(clip_id).c_str(), kRevealFilterId);
                        continue;
                    }
                    reveal_filter = filter;
                    continue;
                }
                effects.push_back(filter);
            }

            // The reveal is installed before the ordinary chain so GES applies
            // it last (closest to the sink): each top effect added later lands
            // at a higher priority and runs earlier, and the reveal must mask
            // the already-graded picture - a grade applied after it would
            // repaint the transparent hidden pixels. The reveal needs its baked
            // map; a title opted in without one is reported and skipped (R9),
            // leaving the clip unrevealed exactly as before.
            if (reveal_filter.has_value()) {
                const auto reveal_map_it = built.reveal_maps.find(path);
                if (reveal_map_it == built.reveal_maps.end()) {
                    g_warning("clip %s: '%s' has no reveal map; the reveal is "
                              "skipped",
                              clip_label(clip_id).c_str(), kRevealFilterId);
                } else {
                    GESBaseEffect *reveal = make_reveal_effect(
                            reveal_pass_for(*reveal_filter, reveal_map_it->second));
                    // make_reveal_effect returns a floating reference (or null);
                    // install_reveal_effect sinks and drops its own reference
                    // either way, so both branches are leak-free.
                    if (reveal == nullptr || !install_reveal_effect(ges_clip, reveal)) {
                        g_warning("clip %s: could not build the reveal effect; "
                                  "skipped",
                                  clip_label(clip_id).c_str());
                    }
                }
            }

            if (effects.empty()) {
                continue;
            }
            if (clip->speed() != Rational::ONE) {
                g_warning("clip %s: non-unit speed would mis-time keyed clip "
                          "effects; skipped",
                          clip_label(clip_id).c_str());
                continue;
            }

            // The project-style clip the chain resolves against, copied from
            // the host clip so the builder never touches the UI's model and the
            // fraction seam (source_start, duration) and the diagnostic id stay
            // beside the effects.
            genesis::project::Clip host;
            host.id = clip_label(clip_id);
            host.media_id = path;
            host.start = clip->start();
            host.duration = clip->duration();
            host.source_start = clip->source_start();
            host.video_effects = std::move(effects);

            auto chain = std::make_unique<ClipEffectChain>();
            chain->clip = std::move(host);
            chain->catalogue = built.catalogue;
            // install_clip_effects reports its own skips; on false the chain
            // dies here and the clip stays without top effects.
            if (install_clip_effects(ges_clip, chain.get())) {
                clip_effects.push_back(PendingClipEffects{ ges_clip, std::move(chain) });
            }
        }
    }

    // Treatments: each becomes its own GESEffectClip - GES's adjustment-layer
    // primitive - spanning the layer's window on its track. The glshader chain
    // is installed once here (the graph is static); its uniforms ride per
    // buffer through the probe attached after add_track
    // (docs/decisions/effect-animation.md §3). Effect clips must land in their
    // layers before add_track, so this runs ahead of the track creation below.
    std::vector<PendingTreatment> treatments;
    for (const genesis::render::Treatment &treatment : built.treatments) {
        if (treatment.track >= layers.size()) {
            continue;
        }
        const std::optional<ResolvedTreatment> resolved = resolve_treatment(treatment);
        if (!resolved) {
            continue;
        }
        GESEffectClip *effect_clip =
                ges_effect_clip_new(shader_chain_description(resolved->passes).c_str(), nullptr);
        if (effect_clip == nullptr) {
            continue;
        }
        ges_timeline_element_set_start(GES_TIMELINE_ELEMENT(effect_clip), to_ns(treatment.start));
        ges_timeline_element_set_duration(GES_TIMELINE_ELEMENT(effect_clip),
                                          to_ns(treatment.end - treatment.start));
        if (!ges_layer_add_clip(layers[treatment.track], GES_CLIP(effect_clip))) {
            gst_object_unref(effect_clip);
            continue;
        }

        PendingTreatment pending;
        pending.clip = GES_CLIP(effect_clip);
        pending.passes = resolved->passes;
        pending.sources = std::move(resolved->sources);
        pending.chain = std::make_unique<TreatmentChain>(treatment);
        // Seed `last` with the build-time resolution so the probe's first
        // buffer uploads nothing when its knobs still read the same.
        pending.chain->last = pending.passes;
        treatments.push_back(std::move(pending));
    }

    // Layers and clips alone select no track element, so the timeline cannot
    // preroll, seek, or report position. One video and one audio GESTrack make
    // the returned graph playable as-is. The audio track is added even when the
    // graph has no audio clips: an empty track contributes no elements and costs
    // nothing, and keeps the built graph uniform regardless of contents.
    GESTrack *video = GES_TRACK(ges_video_track_new());
    GESTrack *audio = GES_TRACK(ges_audio_track_new());
    if (video == nullptr || audio == nullptr) {
        gst_object_unref(timeline);
        return nullptr;
    }
    // add_track sinks the floating reference (gst_bin_add); the timeline owns
    // both tracks from here.
    if (!ges_timeline_add_track(timeline, video) || !ges_timeline_add_track(timeline, audio)) {
        gst_object_unref(timeline);
        return nullptr;
    }

    // Wire each clip's top effects. add_track materialised their bins, so the
    // glshaders can now be found, given their fragments and build-time
    // uniforms, and the per-buffer probe attached. On success the probe owns
    // the chain (its destroy notify frees it at teardown); on failure the
    // unique_ptr frees it when `clip_effects` unwinds.
    for (PendingClipEffects &pending : clip_effects) {
        if (configure_clip_effects(pending.clip, pending.chain.get())) {
            pending.chain.release();
        }
    }

    // Apply each audio-bearing clip's gain, pan and fades. add_track
    // materialised the audio source track elements, so the volume and the fade
    // control source have somewhere to land; the pan is a top effect added to
    // the same clip.
    for (const PendingAudio &pending : pending_audio) {
        apply_clip_audio(pending);
    }

    // Transitions: one GESTransitionClip per span, on the incoming clip's
    // layer, blending with the layer below over the span's own window. A span
    // whose id has no standard GES type (or whose track is out of range, or
    // that names the bottom track with nothing below it to blend with) is
    // skipped: the incoming clip's dissolve ramp already covers it, the same
    // degrade a machine with no shader gets.
    for (const genesis::render::TransitionSpan &span : built.transitions) {
        if (span.to_track == 0 || span.to_track >= layers.size()) {
            continue;
        }
        const std::optional<GESVideoStandardTransitionType> type = transition_type_of(span.id);
        if (!type) {
            continue;
        }
        GESTransitionClip *transition = ges_transition_clip_new(*type);
        if (transition == nullptr) {
            continue;
        }
        // The transition sits over the span on the incoming layer and owns no
        // timeline element of its own; set start/duration before adding so the
        // layer positions it rather than defaulting to the timeline's origin.
        ges_timeline_element_set_start(GES_TIMELINE_ELEMENT(transition), to_ns(span.start));
        ges_timeline_element_set_duration(GES_TIMELINE_ELEMENT(transition),
                                          to_ns(span.end - span.start));
        if (!ges_layer_add_clip(layers[span.to_track], GES_CLIP(transition))) {
            // add_clip sinks the floating reference on success; on failure
            // the reference is still ours to release.
            gst_object_unref(transition);
        }
    }

    // Wire each treatment's glshaders. add_track materialised the effect
    // clip's bin, so the named glshaders can now be found, given their
    // fragments and build-time uniforms, and the per-buffer probe attached to
    // the first glshader's sink pad.
    for (PendingTreatment &pending : treatments) {
        GstElement *bin = effect_bin_of(pending.clip);
        if (bin == nullptr) {
            // No element materialised: the chain dies with `treatments` below
            // (its unique_ptr), and the effect clip stays as an empty layer.
            continue;
        }
        std::vector<GstElement *> shaders;
        shaders.reserve(pending.passes.size());
        for (std::size_t i = 0; i < pending.passes.size(); ++i) {
            const std::string name = "t" + std::to_string(i);
            GstElement *shader = gst_bin_get_by_name(GST_BIN(bin), name.c_str());
            if (shader == nullptr) {
                break;
            }
            g_object_set(shader, "fragment", pending.sources[i].c_str(), nullptr);
            if (pending.passes[i].lut.has_value()) {
                const std::size_t size = pending.passes[i].lut->size;
                GBytes *data = g_bytes_new(pending.passes[i].lut->rgba->data(),
                                           size * size * size * 4 * sizeof(float));
                g_object_set(shader, "lut-size", static_cast<guint>(size), "lut-data", data,
                             nullptr);
                g_bytes_unref(data);
            }
            GstStructure *uniforms = uniforms_for(pending.passes[i]);
            g_object_set(shader, "uniforms", uniforms, nullptr);
            gst_structure_free(uniforms);
            shaders.push_back(shader);
            // The bin owns the glshader; the chain holds it borrowed, so this
            // lookup reference is released at once.
            gst_object_unref(shader);
        }
        if (shaders.size() != pending.passes.size()) {
            continue;
        }
        pending.chain->shaders = std::move(shaders);
        if (!attach_treatment_probe(pending.chain->shaders.front(), pending.chain.get(),
                                    free_treatment_chain)) {
            // The probe did not attach, so no destroy notify will free the
            // chain; the unique_ptr does when `treatments` unwinds.
            continue;
        }
        pending.chain.release();
    }

    return timeline;
}

GstElement *make_reveal_bin(const genesis::core::ShaderPass &pass)
{
    // The routing branch is only reachable when the pass carries a reveal map;
    // a normal pass stays on the glshader path. A reveal bin without a map
    // would sample tex1 from the sentinel (TextureMix.cpp), rendering the
    // picture unchanged - refused up front rather than built useless.
    if (!pass.reveal_map.has_value()) {
        return nullptr;
    }

    // The body the texturemix samples: the pass's own, or the pass-through
    // identity when the resolver left `source` empty. The body must follow the
    // texturemix convention - it references tex0 (picture) and tex1 (map) and
    // declares its own non-sampler uniforms - so it never goes through
    // `fragment_source_for`, which is glshader's single-`tex` convention.
    const bool has_body = pass.source && !pass.source->empty();
    const std::string body = has_body ? *pass.source : std::string(kRevealIdentityBody);

    GstStructure *uniforms = uniforms_for(pass);
    GstElement *mix = make_texture_mix(body, uniforms);
    gst_structure_free(uniforms);
    if (mix == nullptr) {
        return nullptr;
    }

    GstBin *bin = GST_BIN(gst_bin_new("reveal"));
    if (bin == nullptr || !gst_bin_add(bin, mix)) {
        gst_clear_object(&bin);
        gst_object_unref(mix);
        return nullptr;
    }

    // sink_0 carries the picture: ghosted as "sink" so the caller links the
    // picture chain to the bin itself. Requested first so it is deterministically
    // sink_0 (the texturemix maps sink_N to texN by the pad name's suffix).
    GstPad *sink0 = gst_element_request_pad_simple(mix, "sink_%u");
    if (sink0 == nullptr) {
        gst_object_unref(bin);
        return nullptr;
    }
    GstPad *ghost_sink = gst_ghost_pad_new("sink", sink0);
    gst_object_unref(sink0);
    if (ghost_sink == nullptr || !gst_element_add_pad(GST_ELEMENT(bin), ghost_sink)) {
        gst_clear_object(&ghost_sink);
        gst_object_unref(bin);
        return nullptr;
    }

    // The reveal-map chain: the map arrives as GRAY8, one byte per pixel, and
    // is converted up to the RGBA GLMemory the texturemix sink expects - the
    // same upload path the picture chain and the spike use, so the GL context
    // (answered on the bus, as ever) is the only outside requirement.
    const std::uint32_t width = pass.reveal_map->width;
    const std::uint32_t height = pass.reveal_map->height;

    GstElement *map_src = gst_element_factory_make("appsrc", "map");
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *upload = gst_element_factory_make("glupload", nullptr);
    GstElement *colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement *filter = gst_element_factory_make("capsfilter", nullptr);
    if (map_src == nullptr || convert == nullptr || upload == nullptr || colorconvert == nullptr
        || filter == nullptr) {
        gst_clear_object(&map_src);
        gst_clear_object(&convert);
        gst_clear_object(&upload);
        gst_clear_object(&colorconvert);
        gst_clear_object(&filter);
        gst_object_unref(bin);
        return nullptr;
    }

    // The map's caps force the canvas size and a time base, so the caller
    // pushes width*height bytes plus EOS and the aggregator matches the map
    // frame to the picture frame by timestamp.
    GstCaps *map_caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "GRAY8",
                                            "width", G_TYPE_INT, static_cast<gint>(width), "height",
                                            G_TYPE_INT, static_cast<gint>(height), "framerate",
                                            GST_TYPE_FRACTION, 30, 1, nullptr);
    g_object_set(map_src, "caps", map_caps, "format", GST_FORMAT_TIME, nullptr);
    gst_caps_unref(map_caps);

    // Forced RGBA GLMemory: the load-bearing cap the spike proved negotiates
    // the upload to the exact memory the texturemix sink templates accept.
    GstCaps *filter_caps = gst_caps_from_string("video/x-raw(memory:GLMemory),format=(string)RGBA");
    g_object_set(filter, "caps", filter_caps, nullptr);
    gst_caps_unref(filter_caps);

    gst_bin_add_many(bin, map_src, convert, upload, colorconvert, filter, nullptr);
    if (!gst_element_link_many(map_src, convert, upload, colorconvert, filter, nullptr)) {
        gst_object_unref(bin);
        return nullptr;
    }

    // The map feeds sink_1, leaving the picture on sink_0.
    GstPad *sink1 = gst_element_request_pad_simple(mix, "sink_%u");
    if (sink1 == nullptr) {
        gst_object_unref(bin);
        return nullptr;
    }
    GstPad *filter_src = gst_element_get_static_pad(filter, "src");
    const gboolean linked =
            filter_src != nullptr && gst_pad_link(filter_src, sink1) == GST_PAD_LINK_OK;
    if (filter_src != nullptr) {
        gst_object_unref(filter_src);
    }
    gst_object_unref(sink1);
    if (!linked) {
        gst_object_unref(bin);
        return nullptr;
    }

    // The revealed picture leaves through a ghost src, so the caller links the
    // bin to its sink exactly like any other element.
    GstPad *mix_src = gst_element_get_static_pad(mix, "src");
    GstPad *ghost_src = mix_src != nullptr ? gst_ghost_pad_new("src", mix_src) : nullptr;
    if (mix_src != nullptr) {
        gst_object_unref(mix_src);
    }
    if (ghost_src == nullptr || !gst_element_add_pad(GST_ELEMENT(bin), ghost_src)) {
        gst_clear_object(&ghost_src);
        gst_object_unref(bin);
        return nullptr;
    }

    return GST_ELEMENT(bin);
}

} // namespace genesis::adapters::engine::ges
