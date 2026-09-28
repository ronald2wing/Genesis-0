// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/effects/RevealEffect.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <ges/ges-frame-composition-meta.h>
#include <ges/ges.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "adapters/engine/ges/effects/ShaderChain.h"
#include "adapters/engine/ges/effects/TextureMix.h"

// The element lives at file scope: G_DEFINE_TYPE generates the get_type/
// class-init machinery at global scope, so the GESBaseEffect subclass cannot
// sit inside the namespace (matching TextureMix.cpp). The public
// engine-neutral surface (register/make/install/update) wraps it below.
//
// Why a custom GESBaseEffect rather than a GESEffectClip or ges_effect_new:
// the reveal needs a second sampler (tex0 picture, tex1 reveal map), which
// stock glshader cannot express and a single-bin GES effect description cannot
// build. The custom factory path is the one the spike proved
// (docs/decisions/effects-execution.md §6): ges_asset_request on the custom
// gtype, ges_track_element_asset_set_track_type, ges_asset_extract, then
// ges_clip_add_top_effect - all public API, no private ABI fields.

namespace genesis::adapters::engine::ges {

// The pass-through body a reveal bin uses when the resolver left `source`
// empty: reads the picture (tex0) unchanged. Byte-for-byte the reveal identity
// in GesBuilder.cpp; the texturemix owns the prelude and binds tex0 (picture)
// and tex1 (map), so this body must never reference glshader's single `tex`.
constexpr const char *kRevealIdentityBody =
        "void main () { gl_FragColor = texture2D (tex0, v_texcoord); }";

} // namespace genesis::adapters::engine::ges

using genesis::adapters::engine::ges::kRevealIdentityBody;

typedef struct _RevealEffect RevealEffect;
typedef struct _RevealEffectClass RevealEffectClass;

// Per-instance state, allocated when the effect is constructed and freed in
// finalize. The ShaderPass is a full copy so update_reveal_progress can rewrite
// the `progress` field without touching the caller's pass; mix and map_src are
// borrowed from the bin (the bin owns them, this struct never unrefs them).
struct RevealEffectPrivate
{
    genesis::core::ShaderPass pass;
    GstElement *mix = nullptr;
    GstElement *map_src = nullptr;
    // The cached gray map frame (one GRAY8 buffer, unref'd in finalize). The
    // appsrc hands a reference of it to gst_app_src_push_buffer exactly once;
    // imagefreeze keeps its own writable copy, so the cached original's header
    // is never mutated.
    GstBuffer *map_template = nullptr;
    // True once the appsrc has pushed the map frame; later need-data emissions
    // only re-arm EOS. imagefreeze freezes the frame and re-emits it for every
    // post-seek segment, so the appsrc never produces a second buffer.
    std::atomic<bool> map_pushed{ false };

    // The GESFrameCompositionMeta the upstream frame positioner attaches to the
    // picture buffer (alpha, position, size, z-order, operator). The texturemix
    // aggregator allocates a fresh output buffer without this meta, so the
    // downstream smart mixer has nothing to scale/position with and composites
    // the title unscaled at the origin - black everywhere else. Cached here from
    // the picture input and re-attached to every output buffer. Guarded because
    // the picture probe and output probe run on different streaming threads.
    std::mutex meta_mutex;
    bool meta_valid = false;
    double meta_alpha = 1.0;
    double meta_posx = 0.0;
    double meta_posy = 0.0;
    double meta_width = -1.0;
    double meta_height = -1.0;
    guint meta_zorder = 0;
    gint meta_operator = 1; // COMPOSITOR_OPERATOR_OVER
};

struct _RevealEffect
{
    GESBaseEffect parent;
    RevealEffectPrivate *priv;
};

struct _RevealEffectClass
{
    GESBaseEffectClass parent_class;
};

// Caches the GESFrameCompositionMeta carried on the picture buffer so the
// output probe can re-attach it to the aggregator's fresh output buffers (see
// RevealEffectPrivate). Runs on the picture sink pad's streaming thread.
static GstPadProbeReturn reveal_cache_composition_meta(GstPad *, GstPadProbeInfo *info,
                                                       gpointer user_data)
{
    auto *priv = static_cast<RevealEffectPrivate *>(user_data);
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer == nullptr) {
        return GST_PAD_PROBE_OK;
    }
    GstMeta *meta = gst_buffer_get_meta(buffer, GES_TYPE_META_FRAME_COMPOSITION);
    if (meta != nullptr) {
        const auto *fm = reinterpret_cast<GESFrameCompositionMeta *>(meta);
        std::lock_guard<std::mutex> lock(priv->meta_mutex);
        priv->meta_valid = true;
        priv->meta_alpha = fm->alpha;
        priv->meta_posx = fm->posx;
        priv->meta_posy = fm->posy;
        priv->meta_width = fm->width;
        priv->meta_height = fm->height;
        priv->meta_zorder = fm->zorder;
        priv->meta_operator = fm->_operator;
    }
    return GST_PAD_PROBE_OK;
}

// Re-attaches the cached composition meta to each output buffer so the
// downstream smart mixer scales/positions the reveal result as the upstream
// frame positioner intended. Runs on the output src pad's streaming thread.
static GstPadProbeReturn reveal_attach_composition_meta(GstPad *, GstPadProbeInfo *info,
                                                        gpointer user_data)
{
    auto *priv = static_cast<RevealEffectPrivate *>(user_data);
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer == nullptr) {
        return GST_PAD_PROBE_OK;
    }
    std::lock_guard<std::mutex> lock(priv->meta_mutex);
    if (!priv->meta_valid) {
        return GST_PAD_PROBE_OK;
    }
    GESFrameCompositionMeta *fm = ges_buffer_add_frame_composition_meta(buffer);
    fm->alpha = priv->meta_alpha;
    fm->posx = priv->meta_posx;
    fm->posy = priv->meta_posy;
    fm->width = priv->meta_width;
    fm->height = priv->meta_height;
    fm->zorder = priv->meta_zorder;
    fm->_operator = priv->meta_operator;
    return GST_PAD_PROBE_OK;
}

// Builds the structural bin (picture chain, map chain, texturemix, output
// chain). The pass-specific bits - the map's caps, the fragment, and the
// uniforms - are set by make_reveal_effect after extract, because create_element
// runs during ges_asset_extract and receives no pass. The structure itself does
// not depend on the pass: the map appsrc caps are set later, and the texturemix
// fragment/uniforms are plain GObject properties.
static GstElement *reveal_create_element(GESTrackElement *track_element)
{
    RevealEffect *self = reinterpret_cast<RevealEffect *>(track_element);

    // The N-input shader, named "mix" so make_reveal_effect finds it.
    GstElement *mix = genesis::adapters::engine::ges::make_texture_mix("", nullptr);
    if (mix == nullptr) {
        return nullptr;
    }
    gst_object_set_name(GST_OBJECT(mix), "mix");

    GstBin *bin = GST_BIN(gst_bin_new("reveal"));
    if (bin == nullptr || !gst_bin_add(bin, mix)) {
        gst_clear_object(&bin);
        gst_object_unref(mix);
        return nullptr;
    }

    // Picture chain: sink ghost -> videoconvert -> glupload -> glcolorconvert
    // -> capsfilter(RGBA GLMemory) -> mix sink_0. GES hands the effect the
    // source in system memory (no auto videoconvert wrapper on a custom
    // GESBaseEffect), so the upload to GLMemory happens here.
    GstElement *p_convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *p_upload = gst_element_factory_make("glupload", nullptr);
    GstElement *p_colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement *p_filter = gst_element_factory_make("capsfilter", nullptr);
    if (p_convert == nullptr || p_upload == nullptr || p_colorconvert == nullptr
        || p_filter == nullptr) {
        gst_clear_object(&p_convert);
        gst_clear_object(&p_upload);
        gst_clear_object(&p_colorconvert);
        gst_clear_object(&p_filter);
        gst_object_unref(bin);
        return nullptr;
    }
    GstCaps *gl_caps = gst_caps_from_string("video/x-raw(memory:GLMemory),format=(string)RGBA");
    g_object_set(p_filter, "caps", gl_caps, nullptr);
    gst_caps_unref(gl_caps);

    // sink_0 is requested first so the picture is deterministically tex0.
    GstPad *sink0 = gst_element_request_pad_simple(mix, "sink_%u");
    if (sink0 == nullptr) {
        gst_object_unref(bin);
        return nullptr;
    }
    gst_bin_add_many(bin, p_convert, p_upload, p_colorconvert, p_filter, nullptr);
    GstPad *p_filter_src = gst_element_get_static_pad(p_filter, "src");
    const gboolean picture_linked =
            gst_element_link_many(p_convert, p_upload, p_colorconvert, p_filter, nullptr)
            && p_filter_src != nullptr && gst_pad_link(p_filter_src, sink0) == GST_PAD_LINK_OK;
    if (p_filter_src != nullptr) {
        gst_object_unref(p_filter_src);
    }
    gst_object_unref(sink0);
    if (!picture_linked) {
        gst_object_unref(bin);
        return nullptr;
    }

    GstPad *p_convert_sink = gst_element_get_static_pad(p_convert, "sink");
    GstPad *ghost_sink =
            p_convert_sink != nullptr ? gst_ghost_pad_new("sink", p_convert_sink) : nullptr;
    if (p_convert_sink != nullptr) {
        gst_pad_add_probe(p_convert_sink, GST_PAD_PROBE_TYPE_BUFFER, reveal_cache_composition_meta,
                          self->priv, nullptr);
        gst_object_unref(p_convert_sink);
    }
    if (ghost_sink == nullptr || !gst_element_add_pad(GST_ELEMENT(bin), ghost_sink)) {
        gst_clear_object(&ghost_sink);
        gst_object_unref(bin);
        return nullptr;
    }

    // Map chain: appsrc "map" (GRAY8, caps set after extract) -> imagefreeze
    // -> videoconvert -> glupload -> glcolorconvert -> capsfilter(RGBA
    // GLMemory) -> mix sink_1. imagefreeze is the canonical GES static-image
    // source: it freezes the appsrc's single frame, absorbs the flush seek on
    // its own src pad, and re-emits the frame under a SEGMENT carrying the
    // seek's seqnum. Both map and picture pads then carry the seek's seqnum, so
    // the aggregator's EOS always matches the seek. A plain seekable appsrc
    // instead stamps its post-seek SEGMENT with a FRESH seqnum
    // (gst_base_src_push_segment), which races the picture pad's seek-seqnum
    // segment for the aggregator's EOS seqnum and can drop the EOS -> hang.
    GstElement *map_src = gst_element_factory_make("appsrc", "map");
    GstElement *m_freeze = gst_element_factory_make("imagefreeze", nullptr);
    GstElement *m_convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *m_upload = gst_element_factory_make("glupload", nullptr);
    GstElement *m_colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement *m_filter = gst_element_factory_make("capsfilter", nullptr);
    if (map_src == nullptr || m_freeze == nullptr || m_convert == nullptr || m_upload == nullptr
        || m_colorconvert == nullptr || m_filter == nullptr) {
        gst_clear_object(&map_src);
        gst_clear_object(&m_freeze);
        gst_clear_object(&m_convert);
        gst_clear_object(&m_upload);
        gst_clear_object(&m_colorconvert);
        gst_clear_object(&m_filter);
        gst_object_unref(bin);
        return nullptr;
    }
    g_object_set(map_src, "format", GST_FORMAT_TIME, nullptr);
    GstCaps *m_gl_caps = gst_caps_from_string("video/x-raw(memory:GLMemory),format=(string)RGBA");
    g_object_set(m_filter, "caps", m_gl_caps, nullptr);
    gst_caps_unref(m_gl_caps);

    // sink_1 is requested second, so the map is deterministically tex1.
    GstPad *sink1 = gst_element_request_pad_simple(mix, "sink_%u");
    if (sink1 == nullptr) {
        gst_object_unref(bin);
        return nullptr;
    }
    gst_bin_add_many(bin, map_src, m_freeze, m_convert, m_upload, m_colorconvert, m_filter,
                     nullptr);
    GstPad *m_filter_src = gst_element_get_static_pad(m_filter, "src");
    const gboolean map_linked = gst_element_link_many(map_src, m_freeze, m_convert, m_upload,
                                                      m_colorconvert, m_filter, nullptr)
            && m_filter_src != nullptr && gst_pad_link(m_filter_src, sink1) == GST_PAD_LINK_OK;
    if (m_filter_src != nullptr) {
        gst_object_unref(m_filter_src);
    }
    gst_object_unref(sink1);
    if (!map_linked) {
        gst_object_unref(bin);
        return nullptr;
    }

    // Output chain: mix src -> gldownload -> videoconvert -> capsfilter(RGBA
    // system memory) -> src ghost. The download to system memory is what the
    // downstream GES compositor needs (it has no GLMemory conversion, and the
    // reveal's alpha must survive into the compositor).
    GstElement *o_download = gst_element_factory_make("gldownload", nullptr);
    GstElement *o_convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *o_filter = gst_element_factory_make("capsfilter", nullptr);
    if (o_download == nullptr || o_convert == nullptr || o_filter == nullptr) {
        gst_clear_object(&o_download);
        gst_clear_object(&o_convert);
        gst_clear_object(&o_filter);
        gst_object_unref(bin);
        return nullptr;
    }
    GstCaps *out_caps = gst_caps_from_string("video/x-raw,format=RGBA");
    g_object_set(o_filter, "caps", out_caps, nullptr);
    gst_caps_unref(out_caps);
    gst_bin_add_many(bin, o_download, o_convert, o_filter, nullptr);
    if (!gst_element_link_many(mix, o_download, o_convert, o_filter, nullptr)) {
        gst_object_unref(bin);
        return nullptr;
    }
    GstPad *o_filter_src = gst_element_get_static_pad(o_filter, "src");
    GstPad *ghost_src = o_filter_src != nullptr ? gst_ghost_pad_new("src", o_filter_src) : nullptr;
    if (o_filter_src != nullptr) {
        gst_pad_add_probe(o_filter_src, GST_PAD_PROBE_TYPE_BUFFER, reveal_attach_composition_meta,
                          self->priv, nullptr);
        gst_object_unref(o_filter_src);
    }
    if (ghost_src == nullptr || !gst_element_add_pad(GST_ELEMENT(bin), ghost_src)) {
        gst_clear_object(&ghost_src);
        gst_object_unref(bin);
        return nullptr;
    }

    // Borrowed pointers for make_reveal_effect and update_reveal_progress. The
    // bin owns both; this struct never unrefs them.
    self->priv->mix = mix;
    self->priv->map_src = map_src;

    return GST_ELEMENT(bin);
}

G_DEFINE_TYPE(RevealEffect, reveal_effect, GES_TYPE_BASE_EFFECT)

static void reveal_effect_class_init(RevealEffectClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GESTrackElementClass *track_class = GES_TRACK_ELEMENT_CLASS(klass);

    gobject_class->finalize = [](GObject *object) {
        RevealEffect *self = reinterpret_cast<RevealEffect *>(object);
        if (self->priv != nullptr) {
            g_clear_pointer(&self->priv->map_template, gst_buffer_unref);
            delete self->priv;
            self->priv = nullptr;
        }
        G_OBJECT_CLASS(reveal_effect_parent_class)->finalize(object);
    };

    track_class->create_element = reveal_create_element;
    // No ABI.abi.default_track_type access: the track type is set on the asset
    // via ges_track_element_asset_set_track_type before extraction, so
    // ges_track_element_set_asset hands it to the element. Public API only.
}

static void reveal_effect_init(RevealEffect *self)
{
    self->priv = new RevealEffectPrivate();
}

// Emits the one static map frame, then EOS. imagefreeze freezes that frame and
// re-emits it for every post-seek segment, so the appsrc only ever produces a
// single buffer and never re-arms on a seek. A reference is handed to
// gst_app_src_push_buffer (which steals it), leaving the cached template intact
// for the element's lifetime.
static void on_map_need_data(GstAppSrc *src, guint length, gpointer user_data)
{
    (void)length;
    auto *priv = static_cast<RevealEffectPrivate *>(user_data);
    if (priv->map_template == nullptr) {
        gst_app_src_end_of_stream(src);
        return;
    }
    if (!priv->map_pushed.exchange(true)) {
        GstBuffer *buffer = gst_buffer_ref(priv->map_template);
        GST_BUFFER_PTS(buffer) = 0;
        gst_app_src_push_buffer(src, buffer);
    }
    gst_app_src_end_of_stream(src);
}

namespace genesis::adapters::engine::ges {

bool register_reveal_effect()
{
    return g_type_is_a(reveal_effect_get_type(), GES_TYPE_BASE_EFFECT);
}

GESBaseEffect *make_reveal_effect(const genesis::core::ShaderPass &pass)
{
    // The reveal branch only exists for a pass carrying a reveal map; a normal
    // pass stays on the glshader path. Without a map the texturemix would
    // sample tex1 from the sentinel (TextureMix.cpp), rendering the picture
    // unchanged - refused up front rather than built useless.
    if (!pass.reveal_map.has_value()) {
        return nullptr;
    }
    if (!register_reveal_effect()) {
        return nullptr;
    }

    GError *request_error = nullptr;
    GESAsset *asset = ges_asset_request(reveal_effect_get_type(), kRevealFilterId, &request_error);
    if (asset == nullptr) {
        g_clear_error(&request_error);
        return nullptr;
    }
    ges_track_element_asset_set_track_type(GES_TRACK_ELEMENT_ASSET(asset), GES_TRACK_TYPE_VIDEO);

    GError *extract_error = nullptr;
    GESExtractable *extractable = ges_asset_extract(asset, &extract_error);
    gst_object_unref(asset);
    if (extractable == nullptr) {
        g_clear_error(&extract_error);
        return nullptr;
    }

    auto *self = reinterpret_cast<RevealEffect *>(extractable);
    self->priv->pass = pass;

    // create_element already built the bin and stored mix/map_src in priv, so
    // pass-specific configuration lands here: the map's caps, the one-shot
    // feed, the fragment, and the uniforms.
    GstElement *bin = ges_track_element_get_element(GES_TRACK_ELEMENT(self));
    if (bin == nullptr || self->priv->mix == nullptr || self->priv->map_src == nullptr) {
        gst_object_unref(extractable);
        return nullptr;
    }

    const std::uint32_t width = pass.reveal_map->width;
    const std::uint32_t height = pass.reveal_map->height;
    GstCaps *map_caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "GRAY8",
                                            "width", G_TYPE_INT, static_cast<gint>(width), "height",
                                            G_TYPE_INT, static_cast<gint>(height), "framerate",
                                            GST_TYPE_FRACTION, 30, 1, nullptr);
    g_object_set(self->priv->map_src, "caps", map_caps, nullptr);
    gst_caps_unref(map_caps);

    // STREAM mode: the appsrc is a one-shot source that pushes the single map
    // frame then EOS. imagefreeze absorbs the flush seek on its own src pad and
    // never forwards it upstream, so the appsrc has no seek to answer and no
    // re-arm to perform - imagefreeze re-emits its cached frame for every
    // post-seek segment with the seek's seqnum.
    gst_app_src_set_stream_type(GST_APP_SRC(self->priv->map_src), GST_APP_STREAM_TYPE_STREAM);

    // Cache the gray map as a template buffer the need-data callback hands a
    // reference of to the appsrc exactly once.
    const std::vector<std::uint8_t> &gray = *pass.reveal_map->gray;
    GstBuffer *template_buffer = gst_buffer_new_allocate(nullptr, gray.size(), nullptr);
    if (template_buffer == nullptr) {
        gst_object_unref(extractable);
        return nullptr;
    }
    GstMapInfo template_info;
    if (gst_buffer_map(template_buffer, &template_info, GST_MAP_WRITE)) {
        std::memcpy(template_info.data, gray.data(), gray.size());
        gst_buffer_unmap(template_buffer, &template_info);
    }
    self->priv->map_template = template_buffer;

    GstAppSrcCallbacks callbacks = { };
    callbacks.need_data = on_map_need_data;
    gst_app_src_set_callbacks(GST_APP_SRC(self->priv->map_src), &callbacks, self->priv, nullptr);

    const bool has_body = pass.source && !pass.source->empty();
    const std::string body = has_body ? *pass.source : std::string(kRevealIdentityBody);
    g_object_set(self->priv->mix, "fragment", body.c_str(), nullptr);
    GstStructure *uniforms = uniforms_for(pass);
    g_object_set(self->priv->mix, "uniforms", uniforms, nullptr);
    gst_structure_free(uniforms);

    // The extracted track element carries a floating reference the caller
    // sinks (via install_reveal_effect or g_object_ref_sink), mirroring the
    // ClipEffects.cpp ownership of ges_effect_new's result.
    return GES_BASE_EFFECT(extractable);
}

bool install_reveal_effect(GESClip *clip, GESBaseEffect *effect)
{
    if (clip == nullptr || effect == nullptr) {
        return false;
    }
    // Sink first so both of ges_clip_add_top_effect's failure paths (add
    // failing before sinking, or succeeding then being reverted) leave the
    // effect alive; the clip takes its own reference on success.
    g_object_ref_sink(effect);
    GError *error = nullptr;
    const gboolean added = ges_clip_add_top_effect(clip, effect, -1, &error);
    gst_object_unref(effect);
    if (!added) {
        g_clear_error(&error);
        return false;
    }
    return true;
}

bool update_reveal_progress(GESBaseEffect *effect, double progress)
{
    if (effect == nullptr) {
        return false;
    }
    auto *self = reinterpret_cast<RevealEffect *>(effect);
    if (self->priv == nullptr || self->priv->mix == nullptr) {
        return false;
    }

    genesis::core::ShaderPass &pass = self->priv->pass;
    const float value = static_cast<float>(progress);
    bool found = false;
    for (genesis::core::ParamField &field : pass.fields) {
        if (field.name == "progress") {
            std::memcpy(pass.params.data() + field.offset, &value, sizeof(value));
            pass.values["progress"] = progress;
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }

    GstStructure *uniforms = uniforms_for(pass);
    g_object_set(self->priv->mix, "uniforms", uniforms, nullptr);
    gst_structure_free(uniforms);
    return true;
}

} // namespace genesis::adapters::engine::ges
