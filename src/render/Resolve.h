// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/ClipTimeline.h"
#include "core/ShaderPass.h"
#include "render/ExportClip.h"

// The flattened clip list becoming the engine's timeline.
//
// `flatten` turns the document into `ExportClip`s; this turns those into a
// `core::Timeline` plus the per-clip facts the decoders and the compositor
// need that the engine's model has no field for. Every chain, pass, size and
// mask a clip renders with is decided here, so the render and the preview
// read what this builds and never look at an `ExportClip` again.

namespace genesis::render {

// Hashes an arena id, so it can key the unordered maps a resolved timeline
// carries - the Rust `HashMap`/`HashSet` these port. Index and generation are
// both folded in: a reused slot has a new generation, so a stale handle must
// not alias the new occupant here any more than it does in the arena.
struct ArenaIdHash
{
    template <typename T>
    std::size_t operator()(core::ArenaId<T> id) const noexcept
    {
        std::size_t seed = std::hash<std::uint32_t>{ }(id.index());
        seed ^= std::hash<std::uint32_t>{ }(id.generation()) + std::size_t{ 0x9e3779b9 }
                + (seed << 6) + (seed >> 2);
        return seed;
    }
};

// The crop and flips the frame plan draws for a clip.
struct PlannedGeometry
{
    project::Crop crop;
    bool flip_h = false;
    bool flip_v = false;
};

// The audio facts the engine applies to a clip's sound: the mixer gain, the
// constant stereo pan, and the head/tail ramps. Carried beside the timeline
// because the engine `Clip` has no field for any of them - the host model does
// (Clip::volume/pan/fade_in/fade_out), but the flattened clip is the only seam
// that carries them into resolve. A clip absent from `BuiltTimeline::audio`
// contributes no sound.
struct ClipAudio
{
    double volume = 1.0;
    double pan = 0.0;
    core::Rational fade_in = core::Rational::ZERO;
    core::Rational fade_out = core::Rational::ZERO;
};

// A packaged transition over a cut. The incoming clip has been overlapped
// onto the outgoing one and moved to `to_track`; over `[start, end)` the
// compositor combines the stack below `to_track` (the outgoing picture) with
// the incoming layer through the package's two-input shader. A machine with
// no GPU shows the dissolve the incoming clip already carries instead.
struct TransitionSpan
{
    core::Rational start;
    core::Rational end;
    std::size_t to_track = 0;
    std::string id;
    std::map<std::string, double> params;

    bool covers(core::Rational time) const { return start <= time && time < end; }

    // How far through the cut `time` is, `0..=1`.
    double progress(core::Rational time) const
    {
        const double span = end.as_double() - start.as_double();
        if (span <= 0.0) {
            return 1.0;
        }
        return std::clamp((time.as_double() - start.as_double()) / span, 0.0, 1.0);
    }
};

// The effect catalogue resolves a treatment's `effects` to passes; a bare
// pointer so a treatment does not copy the whole catalogue and so a caller may
// pin a specific store for tests.
class Catalogue;

// A layer clip, as the compositor needs it: when, over which tracks, what
// effects, and how hard.
struct Treatment
{
    core::Rational start;
    core::Rational end;
    std::size_t track = 0;
    // The layer's applied effects, resolved to passes at each frame, so a
    // keyed knob rides.
    std::vector<project::AppliedFilter> effects;
    float strength = 1.0F;
    double ramp_in = 0.0;
    double ramp_out = 0.0;
    // The catalogue resolving `effects` to passes. Null means the builtin
    // catalogue built from the conventional roots - the common case for an
    // app that has not installed a store. The aggregate initialiser in
    // `build_timeline` leaves it null, so the default must stay reachable.
    const Catalogue *catalogue = nullptr;

    bool covers(core::Rational time) const { return start <= time && time < end; }

    // The shader passes at `time`, each keyed knob at its value there. A
    // layer is never a title, so it never carries a reveal map.
    std::vector<core::ShaderPass> passes_at(core::Rational time) const;

    // How hard the treatment is applied at `time`: the strength, eased in
    // and out over the ramps at either end.
    float strength_at(core::Rational time) const
    {
        const double at = time.as_double() - start.as_double();
        const double left = end.as_double() - time.as_double();
        double ramp = 1.0;
        if (ramp_in > 0.0 && at < ramp_in) {
            ramp = std::min(ramp, at / ramp_in);
        }
        if (ramp_out > 0.0 && left < ramp_out) {
            ramp = std::min(ramp, left / ramp_out);
        }
        return static_cast<float>(static_cast<double>(strength) * std::clamp(ramp, 0.0, 1.0));
    }
};

// An engine timeline plus the per-clip facts the decoders need that the
// engine's model has no field for.
//
// A few of the Rust `BuiltTimeline` maps are not carried here yet because the
// data they hold has no host type or field: `shapes` (per-clip transition
// shapes; `ExportClip` has no `transition_shapes` field). `cutouts` is carried
// as a `MaskSpec` map instead of the Rust `CutoutJob`, which owned opening the
// mask store - here the store is opened by the driver, and the spec the
// flattener already carries is the whole seam the engine reads.
struct BuiltTimeline
{
    explicit BuiltTimeline(core::Timeline timeline) : timeline(std::move(timeline)) { }

    core::Timeline timeline;
    // Clips that are stills: one-frame streams, decoded looping.
    std::unordered_set<core::ClipId, ArenaIdHash> stills;
    // Contain-fitted decode size per clip, where the source's size is known.
    std::unordered_map<core::ClipId, std::pair<std::uint32_t, std::uint32_t>, ArenaIdHash>
            decode_sizes;
    // Each picture's track, so a treatment knows what lies beneath it.
    std::unordered_map<core::ClipId, std::size_t, ArenaIdHash> tracks;
    // The crop and flips the frame plan draws. See `planned_geometry`.
    std::unordered_map<core::ClipId, PlannedGeometry, ArenaIdHash> geometry;
    // The levels the clip's file is read as, where the person has said.
    std::unordered_map<core::ClipId, project::ColorRange, ArenaIdHash> ranges;
    // The clip's applied effects: the passes are resolved from them at each
    // frame, because a knob with keys is worth something different each frame
    // and the resolution is cheap.
    std::unordered_map<core::ClipId, std::vector<project::AppliedFilter>, ArenaIdHash> chains;
    // The clip's audio effects: the audio backends are resolved from them by
    // the engine adapter at build time. The audio-path sibling of `chains`;
    // carried separately so a picture effect and a sound effect never share a
    // pack's kind.
    std::unordered_map<core::ClipId, std::vector<project::AppliedFilter>, ArenaIdHash> audio_chains;
    // The clip whose cutout is drawn tinted rather than cut, if one is.
    std::optional<core::ClipId> highlight;
    // The audio facts per audio-bearing clip, by clip id: a clip whose file
    // carries sound it plays (an audio clip, or a video clip whose embedded
    // sound is not muted/detached) gets an entry with its gain, pan and
    // head/tail ramps. A clip absent from this map contributes no sound, and a
    // timeline whose every clip is absent has no audio track at all.
    std::unordered_map<core::ClipId, ClipAudio, ArenaIdHash> audio;
    // The layers: treatments over the stack, by span.
    std::vector<Treatment> treatments;
    // The packaged transitions over cuts, resolved before the timeline.
    std::vector<TransitionSpan> transitions;
    // Title clips' pictures, by clip id: rasterised by the app (which owns Qt)
    // and handed back before the graph is built. A title clip with no entry is
    // built without a picture rather than failing the whole timeline.
    std::unordered_map<std::string, std::string> title_images;
    // Title clips' per-word reveal maps, by clip id: produced by the app from the
    // same layout that drew the picture, and attached to those clips' passes so a
    // reveal shader can sample each pixel's word order. A clip with no entry
    // reveals everything, which is what an absent map means (RevealMap::identity).
    std::unordered_map<std::string, genesis::core::RevealMap> reveal_maps;
    // The reversed copy of a clip's source span, by clip id: the engine builds
    // the clip from this file instead of the source when a fresh reverse is
    // available. Only video clips the person flipped backwards and that had a
    // generated, still-fresh reverse file get an entry; every other clip is
    // absent and reads its source forward.
    std::unordered_map<core::ClipId, std::string, ArenaIdHash> reverse_sources;
    // The downscaled copy of a clip's whole source, by clip id: the engine
    // builds the clip from this file instead of the source when a fresh proxy
    // is available. Only forward-playing video clips that had a generated,
    // still-fresh proxy file get an entry; every other clip is absent and
    // reads its source full-size. Reverse wins: a reversed clip never gets a
    // proxy entry, its reverse copy already stands in for the source.
    std::unordered_map<core::ClipId, std::string, ArenaIdHash> proxy_sources;
    // The mask facts per cut clip, by clip id: the engine builds the clip's
    // alpha from a per-frame mask instead of leaving it whole. Carried here so
    // the engine looks masks up exactly as the driver wrote them - the root,
    // source, subject, model and stride ride the spec, never re-derived. A
    // cutout clip with no entry renders whole (its masks were never generated,
    // or the caller supplied no mask root).
    std::unordered_map<core::ClipId, genesis::ai::vision::MaskSpec, ArenaIdHash> cutouts;
    // The effect catalogue resolving every chain, treatment and audio filter at
    // build time (and, for treatments, per frame). Null means the builtin
    // catalogue; a caller that built one over the contributed pack roots passes
    // it here so an extension's packs resolve beside the shipped ones. A bare
    // pointer: the catalogue must outlive the timeline (and the session that
    // renders it), since the per-frame resolve dereferences it on the streaming
    // thread.
    const Catalogue *catalogue = nullptr;
};

// Converts the flattened clip list into an engine timeline.
//
// `visible` is the flattened list, one entry per clip; `transitions` the cuts
// already packaged by the transition resolver. Time is already `Rational` in
// the host's `ExportClip`, so the quantise seam the Rust `build_timeline`
// applies on the way in has already happened at flatten time.
// `title_images` maps a title clip's id to the PNG the app rasterised for it.
// Defaulted to empty rather than an injected resolver: the app already holds
// the rasterised pictures as one flat map, so a plain value parameter is the
// whole seam - no indirection, and a caller without titles (or from before the
// field existed) passes nothing and the graph builds exactly as it did before.
// `reveal_maps` is the same seam, one entry per title clip's per-word reveal
// map, handed over beside `title_images` and defaulted to empty for the same
// reason.
// `reverse_cache_root` is the folder the app generated (or is generating)
// reversed copies into. When set, a reversed video clip whose reverse file is
// present and still fresh against its recorded key gets a `reverse_sources`
// entry; without it (a caller from before the field existed, or a build before
// the generation finished) the clip reads its source forward exactly as it
// always did.
// `proxy_cache_root` is the same seam for the whole-source downscales: when
// set, a forward-playing video clip whose proxy file is present and still
// fresh against its recorded key gets a `proxy_sources` entry. Preview-only -
// export callers pass no root so the export reads originals.
BuiltTimeline
build_timeline(const ExportRequest &request, core::FrameRate rate,
               std::span<const ExportClip> visible, std::vector<TransitionSpan> transitions,
               std::unordered_map<std::string, std::string> title_images = { },
               std::unordered_map<std::string, genesis::core::RevealMap> reveal_maps = { },
               std::optional<std::filesystem::path> reverse_cache_root = std::nullopt,
               std::optional<std::filesystem::path> proxy_cache_root = std::nullopt,
               const Catalogue *catalogue = nullptr);

// The crop and flips the frame plan draws for this clip, or `nullopt` when
// it has neither. The plan crops and flips the picture as it draws it, before
// the clip's effects run over it: a vignette is centred on the picture as
// cropped, a wipe on a mirrored clip still wipes the way the frame is seen.
std::optional<PlannedGeometry> planned_geometry(const ExportClip &clip);

// The enabled entries of a chain whose package has a shader: the ones
// resolved to passes each frame. A link to a package not installed here draws
// nothing.
std::vector<project::AppliedFilter> shaded(const std::vector<project::AppliedFilter> &effects);

// The source's contain-fitted size inside the output frame, or `nullopt` when
// the UI never learnt the source's dimensions.
// `whole` asks for the uncropped picture - the frame plan crops it - at the
// scale that fits the part the crop keeps, so the kept part lands at the same
// size either way and a crop costs no sharpness.
std::optional<std::pair<std::uint32_t, std::uint32_t>>
fitted_size(const ExportRequest &request, const ExportClip &clip, bool whole);

// The f64 -> rational seam: the nearest frame-grid time to `seconds` at
// `rate`. A document value stops being approximate here.
core::Rational quantise(double seconds, core::FrameRate rate);

} // namespace genesis::render
