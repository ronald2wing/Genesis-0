// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/effects/MaskEffect.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#include <ges/ges-frame-composition-meta.h>
#include <ges/ges.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "adapters/engine/ges/effects/TextureMix.h"

// The element lives at file scope: G_DEFINE_TYPE generates the get_type/
// class-init machinery at global scope, so the GESBaseEffect subclass cannot
// sit inside the namespace (matching RevealEffect.cpp). The public
// engine-neutral surface (register/make/install) wraps it below.
//
// Why a custom GESBaseEffect rather than a GESEffectClip or ges_effect_new:
// the mask needs a second sampler (tex0 picture, tex1 mask), which stock
// glshader cannot express, and the mask is per-frame - a static imagefreeze
// like the reveal's map would cut every frame with the first mask. The custom
// factory path is the one the reveal proved: ges_asset_request on the custom
// gtype, ges_track_element_asset_set_track_type, ges_asset_extract, then
// ges_clip_add_top_effect - all public API, no private ABI fields.

typedef struct _MaskEffect MaskEffect;
typedef struct _MaskEffectClass MaskEffectClass;

// Per-instance state, allocated when the effect is constructed and freed in
// finalize. The MaskSpec and FrameRate are full copies so the probe reads them
// without touching the caller; mix and mask_src are borrowed from the bin (the
// bin owns them, this struct never unrefs them).
struct MaskEffectPrivate
{
    genesis::ai::vision::MaskSpec spec;
    // Overwritten by make_mask_effect before any buffer flows; the default is
    // only the struct's required initial value (FrameRate has no null state).
    genesis::core::FrameRate rate = genesis::core::FrameRate::THIRTY;
    GstElement *mix = nullptr;
    GstElement *mask_src = nullptr;

    // The last sampled mask frame's alpha plane, cached so consecutive source
    // frames under a stride > 1 (which sample the same decimated mask) read the
    // file once, not once per frame. Filled lazily; the sentinel frame means
    // "nothing cached yet". Written only on the picture probe's single thread.
    std::uint32_t cached_frame = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint8_t> cached_alpha;

    // The seqnum of the most recent upstream seek, captured so the mask chain's
    // post-seek SEGMENT can be re-stamped to match the picture chain's segment.
    // gst_base_src_push_segment stamps the appsrc's post-seek SEGMENT with a
    // FRESH seqnum, which races the picture pad's seek-seqnum segment for the
    // texturemix aggregator's EOS seqnum (gstaggregator.c sets priv->seqnum from
    // the last SEGMENT it sees) and drops the EOS -> hang. Written on the seek
    // propagation thread, read on the appsrc streaming thread, so atomic.
    std::atomic<guint32> seek_seqnum{ GST_SEQNUM_INVALID };

    // The GESFrameCompositionMeta the upstream frame positioner attaches to the
    // picture buffer (alpha, position, size, z-order, operator). The texturemix
    // aggregator allocates a fresh output buffer without this meta, so the
    // downstream smart mixer has nothing to scale/position with and composites
    // the cut picture unscaled at the origin. Cached here from the picture
    // input and re-attached to every output buffer. Guarded because the picture
    // probe and output probe run on different streaming threads.
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

struct _MaskEffect
{
    GESBaseEffect parent;
    MaskEffectPrivate *priv;
};

struct _MaskEffectClass
{
    GESBaseEffectClass parent_class;
};

// The source frame index a picture buffer at source-time `pts` decodes to, at
// `rate`. The top effect's buffers carry source media time (see ClipEffects.cpp
// clip_buffer_probe: GES restamps timestamps only after the clip's effects), so
// the frame index is the source frame the masks were keyed by. Floor, matching
// FrameRate::frame_at's half-open interval semantics and the floor in the
// decimation mapping. A negative PTS (a preroll edge) clamps to frame 0.
static std::uint32_t source_frame_at(genesis::core::FrameRate rate, GstClockTime pts)
{
    const genesis::core::Rational fps = rate.fps();
    const std::int64_t num = fps.numerator();
    const std::int64_t den = fps.denominator();
    const std::int64_t pts_ns = static_cast<std::int64_t>(pts);
    // frame = pts_ns * fps, exact: pts_ns * num / (den * GST_SECOND).
    const std::int64_t scaled = pts_ns * num;
    const std::int64_t denom = den * static_cast<std::int64_t>(GST_SECOND);
    const std::int64_t frame = scaled / denom;
    return static_cast<std::uint32_t>(frame < 0 ? 0 : frame);
}

// Caches the GESFrameCompositionMeta carried on the picture buffer so the
// output probe can re-attach it to the aggregator's fresh output buffers (see
// MaskEffectPrivate). Runs on the picture sink pad's streaming thread.
static GstPadProbeReturn mask_cache_composition_meta(GstPad *, GstPadProbeInfo *info,
                                                     gpointer user_data)
{
    auto *priv = static_cast<MaskEffectPrivate *>(user_data);
    // The probe is also registered for EVENT_DOWNSTREAM, so it fires on events
    // too; GST_PAD_PROBE_INFO_BUFFER is only valid for buffer probe calls.
    if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
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
// downstream smart mixer scales/positions the cut result as the upstream frame
// positioner intended. Runs on the output src pad's streaming thread.
static GstPadProbeReturn mask_attach_composition_meta(GstPad *, GstPadProbeInfo *info,
                                                      gpointer user_data)
{
    auto *priv = static_cast<MaskEffectPrivate *>(user_data);
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

// The mask alpha plane a source frame samples, from disk, cached by the sampled
// frame so a stride > 1 reads each decimated mask once. A missing, corrupt or
// mis-sized mask degrades to an opaque plane - the clip renders whole rather
// than black - so a mask that was never generated (generation is not wired to
// the app yet) costs only the alpha multiply against 1.0.
static const std::vector<std::uint8_t> &mask_alpha_for(MaskEffectPrivate *priv,
                                                       std::uint32_t source_frame)
{
    const std::size_t n = static_cast<std::size_t>(priv->spec.width) * priv->spec.height;
    const std::uint32_t sampled = genesis::ai::vision::mask_frame_for(priv->spec, source_frame);
    if (priv->cached_frame != sampled) {
        priv->cached_frame = sampled;
        priv->cached_alpha.assign(n, 0xff);
        const std::filesystem::path path =
                genesis::ai::vision::mask_path_for(priv->spec, source_frame);
        const std::optional<genesis::ai::vision::MaskData> mask =
                genesis::ai::vision::read_mask(path);
        if (mask && mask->width == priv->spec.width && mask->height == priv->spec.height
            && mask->alpha.size() == n) {
            priv->cached_alpha = mask->alpha;
        }
    }
    return priv->cached_alpha;
}

// Handles the mask appsrc's seek-data signal by accepting any seek. The mask
// needs no real repositioning - the picture probe re-pushes the matching mask
// per frame by PTS - but accepting the seek makes gst_app_src_is_seekable
// return TRUE, so gst_base_src_perform_seek runs and pushes the flush-start /
// flush-stop pair the texturemix aggregator waits for. Returning FALSE (or
// leaving the signal unhandled) is what left the aggregator stuck flushing.
static gboolean mask_seek_data(GstAppSrc *, guint64, gpointer)
{
    return TRUE;
}

// Captures the upstream seek's seqnum so the appsrc's post-seek SEGMENT can be
// re-stamped to it (see MaskEffectPrivate::seek_seqnum). The seek arrives on the
// appsrc src pad as an upstream event; gst_base_src then runs the flush pair
// under the seek's seqnum but gst_base_src_push_segment stamps the SEGMENT that
// follows with a FRESH seqnum, so the seek's seqnum must be remembered here.
static GstPadProbeReturn mask_capture_seek(GstPad *, GstPadProbeInfo *info, gpointer user_data)
{
    auto *priv = static_cast<MaskEffectPrivate *>(user_data);
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (event != nullptr && GST_EVENT_TYPE(event) == GST_EVENT_SEEK) {
        priv->seek_seqnum.store(gst_event_get_seqnum(event), std::memory_order_relaxed);
    }
    return GST_PAD_PROBE_OK;
}

// Re-stamps the appsrc's post-seek SEGMENT with the seek's seqnum so both mask
// and picture pads carry it. The texturemix aggregator derives its EOS seqnum
// from the last SEGMENT it receives (gstaggregator.c), so a fresh-seqnum mask
// segment races the picture's seek-seqnum segment and the downstream compositor
// drops the EOS -> hang. The event may be shared (sticky), so make it writable
// before rewriting; GST_PAD_PROBE_INFO_DATA is assigned directly because
// make_writable already consumed the probe's reference.
static GstPadProbeReturn mask_restamp_segment(GstPad *, GstPadProbeInfo *info, gpointer user_data)
{
    auto *priv = static_cast<MaskEffectPrivate *>(user_data);
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (event == nullptr || GST_EVENT_TYPE(event) != GST_EVENT_SEGMENT) {
        return GST_PAD_PROBE_OK;
    }
    const guint32 seqnum = priv->seek_seqnum.load(std::memory_order_relaxed);
    if (seqnum == GST_SEQNUM_INVALID || gst_event_get_seqnum(event) == seqnum) {
        return GST_PAD_PROBE_OK;
    }
    event = gst_event_make_writable(event);
    gst_event_set_seqnum(event, seqnum);
    GST_PAD_PROBE_INFO_DATA(info) = event;
    return GST_PAD_PROBE_OK;
}

// Pushes the mask frame matching `buffer` (by source time) into the mask appsrc
// with the same PTS and duration, so the aggregator pairs it with the picture.
static void push_mask_frame(MaskEffectPrivate *priv, GstBuffer *buffer)
{
    if (priv->mask_src == nullptr || !GST_BUFFER_PTS_IS_VALID(buffer)) {
        return;
    }
    const std::uint32_t source_frame = source_frame_at(priv->rate, GST_BUFFER_PTS(buffer));
    const std::vector<std::uint8_t> &alpha = mask_alpha_for(priv, source_frame);

    GstBuffer *mask_buffer = gst_buffer_new_allocate(nullptr, alpha.size(), nullptr);
    if (mask_buffer == nullptr) {
        return;
    }
    GstMapInfo info;
    if (gst_buffer_map(mask_buffer, &info, GST_MAP_WRITE)) {
        std::memcpy(info.data, alpha.data(), alpha.size());
        gst_buffer_unmap(mask_buffer, &info);
    }
    GST_BUFFER_PTS(mask_buffer) = GST_BUFFER_PTS(buffer);
    if (GST_BUFFER_DURATION_IS_VALID(buffer)) {
        GST_BUFFER_DURATION(mask_buffer) = GST_BUFFER_DURATION(buffer);
    }
    // A FLUSHING result (a seek overtook the push) drops the buffer; the next
    // picture probe re-pushes for the new position, so it is not an error.
    gst_app_src_push_buffer(GST_APP_SRC(priv->mask_src), mask_buffer);
}

// One probe that does both jobs on the picture sink pad: cache the composition
// meta, and push the matching mask. Runs on the picture streaming thread.
static GstPadProbeReturn mask_picture_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    auto *priv = static_cast<MaskEffectPrivate *>(user_data);
    if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
        mask_cache_composition_meta(pad, info, user_data);
        push_mask_frame(priv, GST_PAD_PROBE_INFO_BUFFER(info));
    } else if ((info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
        // The mask appsrc is a live push source: nothing upstream sends it EOS,
        // so the picture chain's EOS must be forwarded manually or the
        // aggregator never emits EOS. Runs on the picture streaming thread, the
        // same thread that push_mask_frame uses, so no re-entrancy.
        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
        if (event != nullptr && GST_EVENT_TYPE(event) == GST_EVENT_EOS
            && priv->mask_src != nullptr) {
            gst_app_src_end_of_stream(GST_APP_SRC(priv->mask_src));
        }
    }
    return GST_PAD_PROBE_OK;
}

// Builds the structural bin (picture chain, mask chain, texturemix, output
// chain). The pass-specific bits - the mask caps, the fragment - are set by
// make_mask_effect after extract, because create_element runs during
// ges_asset_extract and receives no spec. The structure itself does not depend
// on the spec beyond requesting sink_0 for the picture and sink_1 for the mask.
static GstElement *mask_create_element(GESTrackElement *track_element)
{
    MaskEffect *self = reinterpret_cast<MaskEffect *>(track_element);

    // The N-input shader, named "mix" so make_mask_effect finds it.
    GstElement *mix = genesis::adapters::engine::ges::make_texture_mix("", nullptr);
    if (mix == nullptr) {
        return nullptr;
    }
    gst_object_set_name(GST_OBJECT(mix), "mix");

    GstBin *bin = GST_BIN(gst_bin_new("mask"));
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
        gst_pad_add_probe(p_convert_sink,
                          static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER
                                                       | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                          mask_picture_probe, self->priv, nullptr);
        gst_object_unref(p_convert_sink);
    }
    if (ghost_sink == nullptr || !gst_element_add_pad(GST_ELEMENT(bin), ghost_sink)) {
        gst_clear_object(&ghost_sink);
        gst_object_unref(bin);
        return nullptr;
    }

    // Mask chain: appsrc "mask" (GRAY8, caps set after extract) -> videoconvert
    // -> glupload -> glcolorconvert -> capsfilter(RGBA GLMemory) -> mix sink_1.
    // No imagefreeze: the mask is per-frame, pushed by the picture probe with
    // the matching PTS, so the aggregator pairs each picture with its own mask.
    GstElement *mask_src = gst_element_factory_make("appsrc", "mask");
    GstElement *m_convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *m_upload = gst_element_factory_make("glupload", nullptr);
    GstElement *m_colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement *m_filter = gst_element_factory_make("capsfilter", nullptr);
    if (mask_src == nullptr || m_convert == nullptr || m_upload == nullptr
        || m_colorconvert == nullptr || m_filter == nullptr) {
        gst_clear_object(&mask_src);
        gst_clear_object(&m_convert);
        gst_clear_object(&m_upload);
        gst_clear_object(&m_colorconvert);
        gst_clear_object(&m_filter);
        gst_object_unref(bin);
        return nullptr;
    }
    g_object_set(mask_src, "format", GST_FORMAT_TIME, nullptr);
    GstCaps *m_gl_caps = gst_caps_from_string("video/x-raw(memory:GLMemory),format=(string)RGBA");
    g_object_set(m_filter, "caps", m_gl_caps, nullptr);
    gst_caps_unref(m_gl_caps);

    // sink_1 is requested second, so the mask is deterministically tex1.
    GstPad *sink1 = gst_element_request_pad_simple(mix, "sink_%u");
    if (sink1 == nullptr) {
        gst_object_unref(bin);
        return nullptr;
    }
    gst_bin_add_many(bin, mask_src, m_convert, m_upload, m_colorconvert, m_filter, nullptr);
    GstPad *m_filter_src = gst_element_get_static_pad(m_filter, "src");
    const gboolean mask_linked =
            gst_element_link_many(mask_src, m_convert, m_upload, m_colorconvert, m_filter, nullptr)
            && m_filter_src != nullptr && gst_pad_link(m_filter_src, sink1) == GST_PAD_LINK_OK;
    if (m_filter_src != nullptr) {
        gst_object_unref(m_filter_src);
    }
    gst_object_unref(sink1);
    if (!mask_linked) {
        gst_object_unref(bin);
        return nullptr;
    }

    // Output chain: mix src -> gldownload -> videoconvert -> capsfilter(RGBA
    // system memory) -> src ghost. The download to system memory is what the
    // downstream GES compositor needs (it has no GLMemory conversion, and the
    // mask's alpha must survive into the compositor).
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
        gst_pad_add_probe(o_filter_src, GST_PAD_PROBE_TYPE_BUFFER, mask_attach_composition_meta,
                          self->priv, nullptr);
        gst_object_unref(o_filter_src);
    }
    if (ghost_src == nullptr || !gst_element_add_pad(GST_ELEMENT(bin), ghost_src)) {
        gst_clear_object(&ghost_src);
        gst_object_unref(bin);
        return nullptr;
    }

    // Borrowed pointers for make_mask_effect. The bin owns both; this struct
    // never unrefs them.
    self->priv->mix = mix;
    self->priv->mask_src = mask_src;

    return GST_ELEMENT(bin);
}

G_DEFINE_TYPE(MaskEffect, mask_effect, GES_TYPE_BASE_EFFECT)

static void mask_effect_class_init(MaskEffectClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GESTrackElementClass *track_class = GES_TRACK_ELEMENT_CLASS(klass);

    gobject_class->finalize = [](GObject *object) {
        MaskEffect *self = reinterpret_cast<MaskEffect *>(object);
        if (self->priv != nullptr) {
            delete self->priv;
            self->priv = nullptr;
        }
        G_OBJECT_CLASS(mask_effect_parent_class)->finalize(object);
    };

    track_class->create_element = mask_create_element;
    // No ABI.abi.default_track_type access: the track type is set on the asset
    // via ges_track_element_asset_set_track_type before extraction, so
    // ges_track_element_set_asset hands it to the element. Public API only.
}

static void mask_effect_init(MaskEffect *self)
{
    self->priv = new MaskEffectPrivate();
}

namespace genesis::adapters::engine::ges {

bool register_mask_effect()
{
    return g_type_is_a(mask_effect_get_type(), GES_TYPE_BASE_EFFECT);
}

GESBaseEffect *make_mask_effect(const genesis::ai::vision::MaskSpec &spec,
                                genesis::core::FrameRate rate)
{
    // A zero-sized mask can never be read or aligned; refuse up front rather
    // than building an effect that multiplies against nothing.
    if (spec.width == 0 || spec.height == 0) {
        return nullptr;
    }
    if (!register_mask_effect()) {
        return nullptr;
    }

    GError *request_error = nullptr;
    GESAsset *asset = ges_asset_request(mask_effect_get_type(), "genesis.mask", &request_error);
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

    auto *self = reinterpret_cast<MaskEffect *>(extractable);
    self->priv->spec = spec;
    self->priv->rate = rate;

    // create_element already built the bin and stored mix/mask_src in priv, so
    // spec-specific configuration lands here: the mask's caps and the fragment.
    GstElement *bin = ges_track_element_get_element(GES_TRACK_ELEMENT(self));
    if (bin == nullptr || self->priv->mix == nullptr || self->priv->mask_src == nullptr) {
        gst_object_unref(extractable);
        return nullptr;
    }

    // The mask caps force the mask size and the frame rate, so the appsrc's
    // GRAY8 buffers are width*height bytes and the aggregator matches a mask to
    // a picture by timestamp at the right cadence.
    const genesis::core::Rational fps = rate.fps();
    GstCaps *mask_caps = gst_caps_new_simple(
            "video/x-raw", "format", G_TYPE_STRING, "GRAY8", "width", G_TYPE_INT,
            static_cast<gint>(spec.width), "height", G_TYPE_INT, static_cast<gint>(spec.height),
            "framerate", GST_TYPE_FRACTION, static_cast<gint>(fps.numerator()),
            static_cast<gint>(fps.denominator()), nullptr);
    g_object_set(self->priv->mask_src, "caps", mask_caps, nullptr);
    gst_caps_unref(mask_caps);

    // SEEKABLE + an accepting seek-data handler make the appsrc seekable, so
    // the upstream seek (GES/nlecomposition initializing) runs the flush-start
    // / flush-stop pair the texturemix aggregator waits for instead of stalling
    // it in the flushing state forever (see mask_seek_data). The default
    // STREAM stream-type is not seekable, so the seek short-circuits without
    // pushing any flush-stop.
    g_object_set(self->priv->mask_src, "stream-type", GST_APP_STREAM_TYPE_SEEKABLE, nullptr);
    g_signal_connect(self->priv->mask_src, "seek-data", G_CALLBACK(mask_seek_data), nullptr);

    // Capture the seek's seqnum as it propagates up through the mask appsrc,
    // then re-stamp the post-seek SEGMENT with it so the mask pad's segment
    // carries the same seqnum as the picture pad's (see mask_capture_seek and
    // mask_restamp_segment). Without this the appsrc's fresh-seqnum segment
    // races the picture's seek-seqnum segment and the aggregator drops the EOS.
    {
        GstPad *src = gst_element_get_static_pad(self->priv->mask_src, "src");
        if (src != nullptr) {
            gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM, mask_capture_seek, self->priv,
                              nullptr);
            gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, mask_restamp_segment,
                              self->priv, nullptr);
            gst_object_unref(src);
        }
    }

    g_object_set(self->priv->mix, "fragment", kMaskBody, nullptr);

    // The extracted track element carries a floating reference the caller sinks
    // (via install_mask_effect or g_object_ref_sink), mirroring the
    // RevealEffect.cpp ownership of ges_effect_new's result.
    return GES_BASE_EFFECT(extractable);
}

bool install_mask_effect(GESClip *clip, GESBaseEffect *effect)
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

} // namespace genesis::adapters::engine::ges
