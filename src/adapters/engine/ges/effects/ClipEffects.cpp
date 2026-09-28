// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/effects/ClipEffects.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ges/ges.h>
#include <gst/gst.h>

#include "adapters/engine/ges/effects/LutGrade.h"
#include "adapters/engine/ges/effects/ShaderChain.h"
#include "effects/Resolve.h"

namespace genesis::adapters::engine::ges {

namespace {

using genesis::core::Rational;
using genesis::core::ShaderPass;
using genesis::effects::CpuFallbackValue;
using genesis::effects::ResolvedCpuFallback;
using genesis::render::Catalogue;
using genesis::render::ChainResolution;
using genesis::render::EffectSkip;

// One top effect per expressible pass. The description uploads the system
// memory GES hands the effect to GLMemory, converts to the RGBA glshader
// samples, runs the pass, and downloads back: GES wraps the effect's bin in
// system-memory `videoconvert` converters on both sides, so the whole
// glupload/gldownload sandwich sits between them (docs/decisions/effect-...).
// The glshader is named so configure_clip_effects can find it after add_track
// materialises the bin.
constexpr const char *kEffectDescription =
        "glupload ! glcolorconvert ! glshader name=t0 ! gldownload name=dl0";
constexpr const char *kShaderName = "t0";

// The same sandwich for a pass carrying a resolved 3D LUT: the lutgrade
// element replaces glshader in place, still named t0 so the same lookup finds
// it. A LUT pass cannot run on glshader (one 2D texture only), and a plain
// pass has nothing for lutgrade to sample, so the two never overlap.
constexpr const char *kLutEffectDescription =
        "glupload ! glcolorconvert ! lutgrade name=t0 ! gldownload name=dl0";

// The name a CPU fallback's stock element is given inside its effect bin, so
// configure_clip_effects can find it after add_track materialises the bin. A
// fixed name is enough: each CPU effect is its own bin, so the names never
// collide across slots.
constexpr const char *kCpuName = "cpu0";

// Whether a named element factory exists on this host: the default
// availability probe a build consults before emitting a CPU fallback. An empty
// `chain->available` means this answers; the probe is injectable so a test can
// force a fallback to degrade without uninstalling a plugin.
bool element_available(const std::string &name)
{
    GstElementFactory *factory = gst_element_factory_find(name.c_str());
    if (factory == nullptr) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

// The exact rational -> GES nanosecond seam, byte-for-byte the conversion the
// builder uses when it hands the clip its inpoint and duration (`to_ns` in
// GesBuilder.cpp). `as_double()` is the documented engine-argument seam.
std::int64_t to_ns(Rational time)
{
    return static_cast<std::int64_t>(std::llround(time.as_double() * GST_SECOND));
}

// The probe glshader runs from: each buffer's PTS is source media time - the
// top effects sit between the source and the rest of the pipeline, before GES
// restamps timestamps - so the clip-local fraction is (pts - source_start) /
// duration (clip_fraction). Runs on the streaming thread; it only re-resolves
// the chain it owns and g_object_sets the glshaders it already holds, touching
// nothing the UI owns.
GstPadProbeReturn clip_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad;
    auto *chain = static_cast<ClipEffectChain *>(user_data);
    if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer == nullptr || !GST_BUFFER_PTS_IS_VALID(buffer)) {
        return GST_PAD_PROBE_OK;
    }
    // The PTS is a guint64; timelines fit in int64 nanoseconds, so the cast is
    // the same one `to_ns` does in reverse and never overflows in practice.
    const std::int64_t pts = static_cast<std::int64_t>(GST_BUFFER_PTS(buffer));
    update_clip_uniforms(*chain, clip_fraction(*chain, pts));
    return GST_PAD_PROBE_OK;
}

// The probe's destroy notify: frees the chain the probe held. The chain's
// effects and shaders are borrowed (the clip owns the effects, their bins own
// the shaders), so nothing is unreffed here - the clip outlives the chain.
void free_clip_effect_chain(void *data)
{
    delete static_cast<ClipEffectChain *>(data);
}

// Whether two passes are the same shader and uniform layout, ignoring the
// per-frame knob values: `key` fingerprints the package version and source,
// `fields` the uniform block, `stages` the pre-stage shape. A pass whose
// params moved is the same pass and must upload; one whose key, fields, or
// stages differ is a different shader the installed slots do not hold.
bool same_pass_layout(const ShaderPass &a, const ShaderPass &b)
{
    return a.package == b.package && a.key == b.key && a.fields == b.fields && a.stages == b.stages;
}

// Reports a structural mismatch at most once per process: the probe runs per
// buffer, so a per-frame warning would drown the log. A mismatch repeats every
// frame until the chain is rebuilt, and nothing on the streaming thread can
// rebuild it.
void warn_structural_change_once(const ClipEffectChain &chain)
{
    static std::atomic<bool> warned{ false };
    bool expected = false;
    if (warned.compare_exchange_strong(expected, true)) {
        g_warning("clip %s: effect chain changed shape after install; keeping "
                  "the installed shaders and their last uniforms",
                  chain.clip.id.c_str());
    }
}

// A chain is animated when any enabled effect carries a keyed parameter: a
// keyed knob resolves to a different value at different fractions, so the
// chain must be re-resolved per buffer. A chain of fixed params resolves
// identically everywhere and is resolved once at install.
bool chain_is_animated(const genesis::project::Clip &clip)
{
    for (const genesis::project::AppliedFilter &filter : clip.video_effects) {
        if (filter.enabled && !filter.keys.empty()) {
            return true;
        }
    }
    return false;
}

} // namespace

bool install_clip_effects(GESClip *clip, ClipEffectChain *chain)
{
    if (clip == nullptr || chain == nullptr) {
        return false;
    }
    // The lutgrade element must be registered before ges_effect_new parses a
    // description naming it. Idempotent; a failure here only matters when a
    // LUT pass is actually installed (the per-pass build below then warns).
    register_lut_grade();

    const Catalogue *resolver = chain->catalogue;
    if (resolver == nullptr) {
        resolver = &Catalogue::builtin();
    }

    // The availability probe: the host's own registry unless the caller
    // injected one (a test forcing a fallback to degrade to an R9 skip).
    std::function<bool(const std::string &)> available = chain->available;
    if (!available) {
        available = element_available;
    }

    // The fraction denominator and offset, in nanoseconds, from the host clip's
    // source inpoint and timeline length - what clip_fraction subtracts and
    // divides by. Derived once here from the copied host clip.
    chain->source_start_ns = to_ns(chain->clip.source_start);
    chain->duration_ns = to_ns(chain->clip.duration);
    // Cached here so the per-buffer probe can skip re-resolving a chain whose
    // knobs never move.
    chain->is_animated = chain_is_animated(chain->clip);

    // A disabled link is a bypass, not a failure: dropped before resolving, so
    // it never surfaces as a skip (matching Catalogue::passes_for).
    genesis::project::Clip enabled = chain->clip;
    enabled.video_effects.clear();
    enabled.video_effects.reserve(chain->clip.video_effects.size());
    for (const genesis::project::AppliedFilter &filter : chain->clip.video_effects) {
        if (filter.enabled) {
            enabled.video_effects.push_back(filter);
        }
    }

    // Build-time resolution at fraction 0, with the probe so a CPU fallback is
    // emitted beside the shader passes.
    const ChainResolution resolved = resolver->passes_for(enabled, 0.0, available);

    // R9: every skipped link is reported, never silently dropped.
    for (const EffectSkip &skip : resolved.skips) {
        g_warning("clip %s: skipping effect '%s': %s", skip.clip_id.c_str(), skip.pack_id.c_str(),
                  skip.reason.c_str());
    }

    // The resolution splits the chain into three vectors, which loses the
    // caller order between a GPU pass and a CPU fallback. Recover it by walking
    // the filters in order and re-deriving each one's bucket, consuming the
    // pre-resolved entries so the installed passes keep their read bodies.
    struct Unit
    {
        bool cpu = false;
        ShaderPass pass;
        ResolvedCpuFallback fallback;
    };
    std::vector<Unit> units;
    units.reserve(enabled.video_effects.size());

    std::size_t pass_index = 0;
    std::size_t cpu_index = 0;
    for (const genesis::project::AppliedFilter &filter : enabled.video_effects) {
        const genesis::effects::Pack *pack = resolver->pack(filter.id);
        if (pack == nullptr) {
            continue; // reported in resolved.skips.
        }
        if (!genesis::effects::is_expressible(*pack)) {
            const std::optional<ResolvedCpuFallback> fallback =
                    genesis::effects::resolve_cpu_fallback(*pack, filter, 0.0);
            if (fallback && available(fallback->element)) {
                Unit unit;
                unit.cpu = true;
                unit.fallback = resolved.cpu[cpu_index++];
                units.push_back(std::move(unit));
            }
            continue; // otherwise reported in resolved.skips.
        }
        // is_expressible is the pack-level test; pass_is_expressible (below) is
        // the adapter-level one. resolve_pass runs only to learn whether the
        // filter resolved (an undeclared parameter or a failing uniform is an
        // R9 skip); the pre-resolved pass carries the read body, so it is the
        // one installed.
        if (!genesis::effects::resolve_pass(*pack, filter, 0.0)) {
            continue; // bad parameters; reported in resolved.skips.
        }
        Unit unit;
        unit.pass = resolved.passes[pass_index++];
        if (!pass_is_expressible(unit.pass)) {
            g_warning("clip %s: pass '%s' cannot run as one glshader; skipped",
                      chain->clip.id.c_str(), unit.pass.package.c_str());
            continue;
        }
        units.push_back(std::move(unit));
    }

    if (units.empty()) {
        chain->slots.clear();
        chain->cpu_slots.clear();
        chain->last.clear();
        return false;
    }

    // Add the effects in reverse order at index -1: GES applies the
    // highest-priority (highest-index) effect first and adds each new top
    // effect at a priority above every existing one, so adding the last unit
    // first and the first unit last leaves the caller order the slots record -
    // GPU passes and CPU fallbacks interleaved exactly as the clip lists them.
    std::vector<ClipEffectChain::Slot> slots(units.size());
    std::vector<ClipEffectChain::CpuSlot> cpu_slots(units.size());
    for (std::size_t i = units.size(); i-- > 0;) {
        const Unit &unit = units[i];
        GESEffect *effect = nullptr;
        if (unit.cpu) {
            // One stock element, named kCpuName so configure finds it after
            // add_track materialises the bin.
            const std::string description = unit.fallback.element + " name=" + kCpuName;
            effect = ges_effect_new(description.c_str());
        } else {
            const char *description =
                    unit.pass.lut.has_value() ? kLutEffectDescription : kEffectDescription;
            effect = ges_effect_new(description);
        }
        if (effect == nullptr) {
            g_warning("clip %s: could not build effect for '%s'", chain->clip.id.c_str(),
                      unit.cpu ? unit.fallback.element.c_str() : unit.pass.package.c_str());
            continue;
        }
        // Hold the floating reference as our own strong reference across the
        // add. ges_clip_add_top_effect has two failure paths with different
        // ownership: ges_container_add can fail without sinking (the floating
        // reference stays with us), or succeed and then be reverted by a
        // track-selection error, where ges_container_remove drops the
        // container's reference and would finalise the effect before a
        // post-hoc unref. Sinking first means the effect outlives both, and
        // the clip takes its own reference on success.
        g_object_ref_sink(effect);
        GError *error = nullptr;
        const gboolean added = ges_clip_add_top_effect(clip, GES_BASE_EFFECT(effect), -1, &error);
        gst_object_unref(effect); // drop ours; the clip keeps its own on success
        if (!added) {
            g_warning("clip %s: could not add effect for '%s': %s", chain->clip.id.c_str(),
                      unit.cpu ? unit.fallback.element.c_str() : unit.pass.package.c_str(),
                      error != nullptr ? error->message : "unknown");
            g_clear_error(&error);
            continue;
        }
        if (unit.cpu) {
            cpu_slots[i].effect = effect;
            cpu_slots[i].values = unit.fallback.values;
        } else {
            slots[i].effect = effect;
        }
    }

    // Keep only the units whose effect landed, in caller order. A unit that
    // failed to build or add leaves a gap; the survivors still run in their
    // relative order (the reverse-add ordering preserved it), so the chain is
    // exactly what was installed - no partial slot left behind.
    std::vector<ShaderPass> installed_passes;
    std::vector<ClipEffectChain::Slot> installed_slots;
    std::vector<ClipEffectChain::CpuSlot> installed_cpu_slots;
    installed_passes.reserve(units.size());
    installed_slots.reserve(units.size());
    installed_cpu_slots.reserve(units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        if (units[i].cpu) {
            if (cpu_slots[i].effect != nullptr) {
                installed_cpu_slots.push_back(std::move(cpu_slots[i]));
            }
        } else if (slots[i].effect != nullptr) {
            installed_passes.push_back(units[i].pass);
            installed_slots.push_back(std::move(slots[i]));
        }
    }
    if (installed_slots.empty() && installed_cpu_slots.empty()) {
        chain->slots.clear();
        chain->cpu_slots.clear();
        chain->last.clear();
        return false;
    }

    chain->slots = std::move(installed_slots);
    chain->cpu_slots = std::move(installed_cpu_slots);
    chain->last = std::move(installed_passes);
    return true;
}

bool configure_clip_effects(GESClip *clip, ClipEffectChain *chain)
{
    if (clip == nullptr || chain == nullptr || (chain->slots.empty() && chain->cpu_slots.empty())
        || chain->slots.size() != chain->last.size()) {
        return false;
    }

    // Walk the clip's child track elements (public GES container API) to find
    // each installed effect's bin - add_track materialised it - and the
    // glshader or stock element inside it. Each child is matched to its slot by
    // the effect's track-element identity, so the traversal order (by priority,
    // the reverse of caller order) cannot scramble the mapping.
    std::size_t found_gpu = 0;
    std::size_t found_cpu = 0;
    GList *children = ges_container_get_children(GES_CONTAINER(clip), TRUE);
    for (GList *it = children; it != nullptr; it = it->next) {
        GESTrackElement *child = GES_TRACK_ELEMENT(it->data);

        // A CPU fallback's slot: its bin holds one stock element (named
        // kCpuName), whose resolved property values ride its own GObject
        // properties. Those register as child props once add_track materialised
        // the bin, so this is the earliest they can be set.
        ClipEffectChain::CpuSlot *cpu_slot = nullptr;
        for (std::size_t i = 0; i < chain->cpu_slots.size(); ++i) {
            if (GES_TRACK_ELEMENT(chain->cpu_slots[i].effect) == child) {
                cpu_slot = &chain->cpu_slots[i];
                break;
            }
        }
        if (cpu_slot != nullptr) {
            GstElement *bin = ges_track_element_get_element(child);
            if (bin == nullptr || !GST_IS_BIN(bin)) {
                g_list_free(children);
                return false;
            }
            GstElement *element = gst_bin_get_by_name(GST_BIN(bin), kCpuName);
            if (element == nullptr) {
                g_list_free(children);
                return false;
            }
            // The resolved doubles land directly on the stock element's
            // double properties (videobalance's brightness, contrast, ...);
            // g_object_set warns on an unknown or mistyped property but the
            // fallback value is a double by schema, so the type is right.
            for (const CpuFallbackValue &value : cpu_slot->values) {
                g_object_set(G_OBJECT(element), value.key.c_str(), value.value, nullptr);
            }
            gst_object_unref(element);
            ++found_cpu;
            continue;
        }

        ClipEffectChain::Slot *slot = nullptr;
        std::size_t index = 0;
        for (std::size_t i = 0; i < chain->slots.size(); ++i) {
            if (GES_TRACK_ELEMENT(chain->slots[i].effect) == child) {
                slot = &chain->slots[i];
                index = i;
                break;
            }
        }
        if (slot == nullptr) {
            continue; // the source element, a core child, or not ours.
        }

        GstElement *bin = ges_track_element_get_element(child);
        if (bin == nullptr || !GST_IS_BIN(bin)) {
            g_list_free(children);
            return false;
        }
        GstElement *shader = gst_bin_get_by_name(GST_BIN(bin), kShaderName);
        if (shader == nullptr) {
            g_list_free(children);
            return false;
        }

        // The build-time pass already resolved the fragment and uniforms; a
        // source the resolver refused cannot reach here (install filtered it).
        // A LUT pass composes its source through the lutgrade convention and
        // also hands the element the table to upload; a plain pass stays on
        // glshader.
        const ShaderPass &pass = chain->last[index];
        std::optional<std::string> source =
                pass.lut.has_value() ? fragment_source_lut_for(pass) : fragment_source_for(pass);
        if (!source.has_value()) {
            gst_object_unref(shader);
            g_list_free(children);
            return false;
        }
        g_object_set(shader, "fragment", source->c_str(), nullptr);
        if (pass.lut.has_value()) {
            const std::size_t size = pass.lut->size;
            GBytes *data =
                    g_bytes_new(pass.lut->rgba->data(), size * size * size * 4 * sizeof(float));
            g_object_set(shader, "lut-size", static_cast<guint>(size), "lut-data", data, nullptr);
            g_bytes_unref(data);
        }
        GstStructure *uniforms = uniforms_for(pass);
        g_object_set(shader, "uniforms", uniforms, nullptr);
        gst_structure_free(uniforms);

        slot->shader = shader; // borrowed: the effect's bin owns it.
        gst_object_unref(shader); // drop the lookup reference.
        ++found_gpu;
    }
    g_list_free(children);

    if (found_gpu != chain->slots.size() || found_cpu != chain->cpu_slots.size()) {
        // At least one effect's bin or element never materialised; the chain
        // is not fully wired, so no probe goes on and the caller keeps it.
        return false;
    }

    // A CPU-only chain has no glshader to probe: its properties are set and
    // its effects render statically, but there is no per-frame uniform to
    // drive, so the caller keeps ownership of the chain (and frees it) rather
    // than handing it to a probe that will never run.
    if (chain->slots.empty()) {
        return false;
    }

    // One probe on the first shader's sink pad drives every pass: each buffer's
    // source-time PTS resolves the clip fraction, which re-resolves and uploads
    // the changed uniforms before the shader processes the buffer. The probe
    // owns the chain from here - its destroy notify frees it at teardown.
    GstElement *first = chain->slots.front().shader;
    GstPad *pad = gst_element_get_static_pad(first, "sink");
    if (pad == nullptr) {
        return false;
    }
    const gulong id = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, clip_buffer_probe, chain,
                                        free_clip_effect_chain);
    gst_object_unref(pad);
    // id == 0 means the probe did not attach: no destroy notify will run, so
    // the caller keeps ownership and frees the chain itself.
    return id != 0;
}

double clip_fraction(const ClipEffectChain &chain, std::int64_t pts_ns)
{
    if (chain.duration_ns <= 0) {
        return 0.0;
    }
    const double fraction = static_cast<double>(pts_ns - chain.source_start_ns)
            / static_cast<double>(chain.duration_ns);
    return std::clamp(fraction, 0.0, 1.0);
}

bool update_clip_uniforms(ClipEffectChain &chain, double fraction)
{
    // A static chain resolves identically at every fraction, so skip the
    // catalogue resolution and return unchanged: the shaders keep the uniforms
    // install seeded. Only an animated chain re-resolves per buffer.
    if (!chain.is_animated) {
        return false;
    }

    const Catalogue *resolver = chain.catalogue;
    if (resolver == nullptr) {
        resolver = &Catalogue::builtin();
    }

    // The runtime resolve reads the catalogue only: the pack-level R9 skips
    // (unknown pack, not expressible, bad parameters) are the same at every
    // frame and were reported at install, and the adapter-level refusals (a
    // curve's vec4[8] field, a second sampler, a pre-stage) are static per
    // pack and were settled there too. Re-deriving them here would rebuild
    // each pass's GLSL on the streaming thread every frame for nothing.
    std::vector<ShaderPass> next = resolver->passes_for(chain.clip, fraction).passes;
    if (next == chain.last) {
        return false;
    }

    // The installed slots drive exactly the passes install built. A resolution
    // that disagrees in count or in a pass's shader/layout cannot be uploaded:
    // uploading by position would pour one pass's uniforms into another pass's
    // shader, and matching duplicate packs by identity is not possible without
    // a stable instance index. Keep the last valid uniforms and wait for a
    // rebuild. The comparison is structural (key/fields/stages), so ordinary
    // time-varying knobs still flow through below.
    if (!chain.last.empty()) {
        bool structural = next.size() != chain.last.size();
        for (std::size_t i = 0; !structural && i < next.size(); ++i) {
            structural = !same_pass_layout(next[i], chain.last[i]);
        }
        if (structural) {
            warn_structural_change_once(chain);
            return false;
        }
    }

    // The guard above leaves next and last the same length; slots can still be
    // shorter on the resolve-only test path, so bound the upload by both.
    const std::size_t count = std::min(next.size(), chain.slots.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (chain.slots[i].shader == nullptr) {
            continue;
        }
        GstStructure *uniforms = uniforms_for(next[i]);
        g_object_set(chain.slots[i].shader, "uniforms", uniforms, nullptr);
        gst_structure_free(uniforms);
    }
    chain.last = std::move(next);
    return true;
}

} // namespace genesis::adapters::engine::ges
