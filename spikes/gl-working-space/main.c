// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Working-space gate spike: two render-layer unknowns from
// docs/decisions/effect-execution.md section 6 / docs/dependencies.md.
//
//   Q1 - linear-light half-float working space: can the GL chain carry a
//        half-float (or any >8-bit) RGBA frame, and is a linear (gamma
//        decoded) working space reachable?
//   Q2 - non-blocking scopes readback: can a monitor histogram/waveform be
//        read without stalling the render path?
//
// Headless EGL probe. Reuses the NEED_CONTEXT handling and the forced
// capsfilter(caps=video/x-raw(memory:GLMemory)) approach from
// spikes/gl-multipass/.
//
// Usage: gl-working-space <case> <mode> [arg]
//   mode:
//     probe    run to EOS, report negotiated caps + GLMemory residency
//     capture  run to EOS, write PNGs (for image evidence)
//     analyze  run to EOS, compare two named appsinks pixel-by-pixel (Q1 rtt)
//     measure  run N seconds of wall time, report main-branch frame rate (Q2)

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/gl/gl.h>
#include <gst/gl/egl/gstgldisplay_egl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shaders.h"
#include "glscopes.h"

/* sRGB and linear (gamma 1.0) colorimetry strings: range:matrix:transfer:primaries
 * = 0_255:RGB:sRGB(or GAMMA10):BT709. See video-color.h enum values. */
#define CLR_SRGB    "1:1:7:1"
#define CLR_LINEAR  "1:1:1:1"
#define CLR_BT709   "1:1:5:1"

/* Source head: a smpte frame, upconverted to 16-bit or 8-bit RGB as needed. */
#define SRC_8 \
    "videotestsrc pattern=smpte num-buffers=%d ! " \
    "video/x-raw,width=%d,height=%d,framerate=%s ! " \
    "videoconvert ! video/x-raw,format=RGBA,colorimetry=" CLR_SRGB

#define SRC_64 \
    "videotestsrc pattern=smpte num-buffers=%d ! " \
    "video/x-raw,width=%d,height=%d,framerate=%s ! " \
    "videoconvert ! video/x-raw,format=RGBA64_LE,colorimetry=" CLR_SRGB

#define PROBE_TAIL \
    " ! tee name=t t. ! queue ! appsink name=sink sync=false max-buffers=1 drop=true " \
    "t. ! queue ! fakesink"

#define CAPTURE_TAIL \
    " ! tee name=t t. ! queue ! appsink name=sink sync=false max-buffers=1 drop=true " \
    "t. ! queue ! gldownload ! videoconvert ! pngenc ! multifilesink name=f"

static GstGLDisplay *g_display = NULL;
static GstGLContext *g_glctx = NULL;
static const char *g_case = "";
static int g_frames = 0;
static int g_glmem = 0;
static int g_scopes_frames = 0;

/* Q2 measure config (env overridable). */
static int g_width = 320;
static int g_height = 240;
static const char *g_rate = "30/1";
static int g_seconds = 5;

static GstBusSyncReply on_bus_sync(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus;
    (void)data;
    if (GST_MESSAGE_TYPE(msg) != GST_MESSAGE_NEED_CONTEXT)
        return GST_BUS_PASS;
    const gchar *type = NULL;
    gst_message_parse_context_type(msg, &type);
    if (g_strcmp0(type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) {
        GstContext *ctx = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
        gst_context_set_gl_display(ctx, g_display);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), ctx);
        gst_context_unref(ctx);
        return GST_BUS_DROP;
    }
    if (g_strcmp0(type, "gst.gl.app_context") == 0) {
        GstContext *ctx = gst_context_new("gst.gl.app_context", TRUE);
        GstStructure *s = gst_context_writable_structure(ctx);
        gst_structure_set(s, "context", GST_TYPE_GL_CONTEXT, g_glctx, NULL);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), ctx);
        gst_context_unref(ctx);
        return GST_BUS_DROP;
    }
    return GST_BUS_PASS;
}

static const char *mem_name(GstCapsFeatures *feat) {
    if (gst_caps_features_contains(feat, "memory:GLMemory")) return "GLMemory";
    if (gst_caps_features_contains(feat, "memory:DMABuf")) return "DMABuf";
    if (gst_caps_features_contains(feat, "memory:SystemMemory")) return "SystemMemory";
    return "other";
}

/* Probe/capture/measure: count frames and report the first frame's caps. */
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer data) {
    (void)sink;
    (void)data;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_ERROR;
    GstCaps *caps = gst_sample_get_caps(sample);
    g_frames++;
    if (caps) {
        GstCapsFeatures *feat = gst_caps_get_features(caps, 0);
        if (gst_caps_features_contains(feat, "memory:GLMemory"))
            g_glmem++;
        if (g_frames == 1) {
            gchar *s = gst_caps_to_string(caps);
            fprintf(stderr, "[%s] FIRST FRAME caps: %s\n", g_case, s);
            fprintf(stderr, "[%s] FIRST FRAME memory: %s\n", g_case, mem_name(feat));
            g_free(s);
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* Scopes branch: compute a 256-bin luma histogram on the CPU. */
static GstFlowReturn on_scopes_sample(GstAppSink *sink, gpointer data) {
    (void)sink;
    (void)data;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_ERROR;
    GstVideoInfo info;
    GstVideoFrame frame;
    if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
        gst_video_frame_map(&frame, &info, gst_buffer_ref(gst_sample_get_buffer(sample)),
                            GST_MAP_READ)) {
        guint hist[256] = {0};
        gint w = GST_VIDEO_FRAME_WIDTH(&frame);
        gint h = GST_VIDEO_FRAME_HEIGHT(&frame);
        gint y, x;
        for (y = 0; y < h; y++) {
            const guint8 *p = (const guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)
                              + y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
            for (x = 0; x < w; x++) {
                guint luma = (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
                hist[luma]++;
                p += 4;
            }
        }
        g_scopes_frames++;
        if (g_scopes_frames == 1) {
            guint nonzero = 0, i;
            for (i = 0; i < 256; i++)
                if (hist[i]) nonzero++;
            fprintf(stderr, "[%s] SCOPES histogram: %dx%d nonzero_bins=%u\n",
                    g_case, w, h, nonzero);
        }
        gst_video_frame_unmap(&frame);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static void set_frag(GstElement *pipeline, const char *name, const char *frag) {
    GstElement *e = gst_bin_get_by_name(GST_BIN(pipeline), name);
    if (!e) {
        fprintf(stderr, "[%s] missing element %s\n", g_case, name);
        exit(1);
    }
    g_object_set(e, "fragment", frag, NULL);
    gst_object_unref(e);
}

static GstElement *parse(const char *desc) {
    GError *perr = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &perr);
    if (!pipeline) {
        fprintf(stderr, "[%s] PARSE FAILED: %s\n", g_case,
                perr ? perr->message : "?");
        if (perr)
            g_error_free(perr);
        return NULL;
    }
    return pipeline;
}

/* Runs a pipeline to EOS/error, reporting frames + residency (probe/capture). */
static int drive_probe(GstElement *pipeline, const char *out, int is_capture) {
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, on_bus_sync, NULL, NULL);

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (sink) {
        GstAppSinkCallbacks cb = {0};
        cb.new_sample = on_new_sample;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, NULL, NULL);
        gst_object_unref(sink);
    }
    if (is_capture && out) {
        GstElement *f = gst_bin_get_by_name(GST_BIN(pipeline), "f");
        if (f) {
            g_object_set(f, "location", out, NULL);
            gst_object_unref(f);
        }
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *e = NULL;
            gchar *d = NULL;
            gst_message_parse_error(msg, &e, &d);
            fprintf(stderr, "[%s] ERROR: %s\n", g_case, e ? e->message : "?");
            if (d) {
                fprintf(stderr, "[%s] DEBUG: %s\n", g_case, d);
                g_free(d);
            }
            g_error_free(e);
        } else {
            fprintf(stderr, "[%s] EOS\n", g_case);
        }
        gst_message_unref(msg);
    } else {
        fprintf(stderr, "[%s] TIMEOUT (no EOS/error in 10s)\n", g_case);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    fprintf(stderr, "[%s] frames=%d glmem_frames=%d\n", g_case, g_frames, g_glmem);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return 0;
}

/* Q1 analyze: pull one frame from the "ref" and "rt" appsinks and compare.
 * Reports max/mean per-pixel channel error in the negotiated (8 or 16 bit)
 * precision. */
static int drive_analyze(GstElement *pipeline, int is_16bit) {
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, on_bus_sync, NULL, NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    /* The dual-gldownload tee does not reliably forward EOS (frames flow, the
     * EOS event stalls on the tee's second pad), so poll the two appsinks for
     * samples instead of waiting for EOS. */
    GstElement *ref = gst_bin_get_by_name(GST_BIN(pipeline), "ref");
    GstElement *rt = gst_bin_get_by_name(GST_BIN(pipeline), "rt");
    int rc = 0;
    if (ref && rt) {
        GstSample *s_ref = NULL, *s_rt = NULL;
        gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        while (g_get_monotonic_time() < deadline && (!s_ref || !s_rt)) {
            if (!s_ref)
                s_ref = gst_app_sink_try_pull_sample(GST_APP_SINK(ref), 0);
            if (!s_rt)
                s_rt = gst_app_sink_try_pull_sample(GST_APP_SINK(rt), 0);
            if (s_ref && s_rt)
                break;
            g_usleep(5000);
        }
        if (s_ref && s_rt) {
            GstVideoInfo i_ref, i_rt;
            GstVideoFrame f_ref, f_rt;
            if (gst_video_info_from_caps(&i_ref, gst_sample_get_caps(s_ref)) &&
                gst_video_info_from_caps(&i_rt, gst_sample_get_caps(s_rt)) &&
                gst_video_frame_map(&f_ref, &i_ref,
                    gst_buffer_ref(gst_sample_get_buffer(s_ref)), GST_MAP_READ) &&
                gst_video_frame_map(&f_rt, &i_rt,
                    gst_buffer_ref(gst_sample_get_buffer(s_rt)), GST_MAP_READ)) {
                gint w = GST_VIDEO_FRAME_WIDTH(&f_ref);
                gint h = GST_VIDEO_FRAME_HEIGHT(&f_ref);
                guint64 maxerr = 0, sumerr = 0, n = 0;
                gint y, x;
                for (y = 0; y < h; y++) {
                    const guint8 *pr = (const guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&f_ref, 0)
                                       + y * GST_VIDEO_FRAME_PLANE_STRIDE(&f_ref, 0);
                    const guint8 *pt = (const guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&f_rt, 0)
                                       + y * GST_VIDEO_FRAME_PLANE_STRIDE(&f_rt, 0);
                    for (x = 0; x < w; x++) {
                        guint64 e;
                        int c;
                        if (is_16bit) {
                            const guint16 *r16 = (const guint16 *)pr;
                            const guint16 *t16 = (const guint16 *)pt;
                            for (c = 0; c < 3; c++) {
                                e = (r16[c] > t16[c]) ? (r16[c] - t16[c]) : (t16[c] - r16[c]);
                                if (e > maxerr) maxerr = e;
                                sumerr += e;
                                n++;
                            }
                            pr += 8;
                            pt += 8;
                        } else {
                            for (c = 0; c < 3; c++) {
                                e = (pr[c] > pt[c]) ? (pr[c] - pt[c]) : (pt[c] - pr[c]);
                                if (e > maxerr) maxerr = e;
                                sumerr += e;
                                n++;
                            }
                            pr += 4;
                            pt += 4;
                        }
                    }
                }
                double scale = is_16bit ? 65535.0 : 255.0;
                fprintf(stderr, "[%s] roundtrip max_err=%.2f/255 mean_err=%.4f/255 "
                        "(over %llu channel samples)\n", g_case,
                        (double)maxerr * 255.0 / scale, (double)sumerr / n * 255.0 / scale,
                        (unsigned long long)n);
                gst_video_frame_unmap(&f_rt);
                gst_video_frame_unmap(&f_ref);
            } else {
                fprintf(stderr, "[%s] could not map ref/rt frames\n", g_case);
                rc = 1;
            }
            gst_sample_unref(s_rt);
            gst_sample_unref(s_ref);
        } else {
            fprintf(stderr, "[%s] missing ref/rt sample (negotiation failed?)\n", g_case);
            rc = 1;
        }
    } else {
        fprintf(stderr, "[%s] missing ref/rt appsinks\n", g_case);
        rc = 1;
    }
    gst_object_unref(ref);
    gst_object_unref(rt);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return rc;
}

/* Q2 measure: run `seconds` of wall time, count main-branch frames. */
static int drive_measure(GstElement *pipeline, int seconds) {
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, on_bus_sync, NULL, NULL);

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (sink) {
        GstAppSinkCallbacks cb = {0};
        cb.new_sample = on_new_sample;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, NULL, NULL);
        gst_object_unref(sink);
    }
    GstElement *scopes = gst_bin_get_by_name(GST_BIN(pipeline), "scopes");
    if (scopes) {
        GstAppSinkCallbacks cb = {0};
        cb.new_sample = on_scopes_sample;
        gst_app_sink_set_callbacks(GST_APP_SINK(scopes), &cb, NULL, NULL);
        gst_object_unref(scopes);
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    gint64 t0 = g_get_monotonic_time();
    gint64 deadline = t0 + (gint64)seconds * G_TIME_SPAN_SECOND;
    int aborted = 0;
    while (g_get_monotonic_time() < deadline) {
        GstMessage *msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (msg) {
            GError *e = NULL;
            gchar *d = NULL;
            gst_message_parse_error(msg, &e, &d);
            fprintf(stderr, "[%s] ERROR: %s\n", g_case, e ? e->message : "?");
            if (d) g_free(d);
            g_error_free(e);
            gst_message_unref(msg);
            aborted = 1;
            break;
        }
        g_usleep(10000);
    }
    gint64 elapsed = g_get_monotonic_time() - t0;
    gst_element_set_state(pipeline, GST_STATE_NULL);
    double fps = (double)g_frames * (double)G_TIME_SPAN_SECOND / (double)elapsed;
    fprintf(stderr, "[%s] main_frames=%d elapsed_ms=%.1f fps=%.2f scopes_frames=%d\n",
            g_case, g_frames, elapsed / 1000.0, fps, g_scopes_frames);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return aborted;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <case> <probe|capture|analyze|measure> [out-template]\n",
                argv[0]);
        return 2;
    }
    g_case = argv[1];
    const char *mode = argv[2];
    const char *out = argc > 3 ? argv[3] : NULL;

    if (getenv("GLWS_WIDTH")) g_width = atoi(getenv("GLWS_WIDTH"));
    if (getenv("GLWS_HEIGHT")) g_height = atoi(getenv("GLWS_HEIGHT"));
    if (getenv("GLWS_RATE")) g_rate = getenv("GLWS_RATE");
    if (getenv("GLWS_SECONDS")) g_seconds = atoi(getenv("GLWS_SECONDS"));

    if (!gst_element_register(NULL, "glscopes", GST_RANK_NONE,
            gst_gl_scopes_get_type())) {
        fprintf(stderr, "failed to register glscopes\n");
        return 1;
    }

    g_display = (GstGLDisplay *)gst_gl_display_egl_new();
    if (!g_display) {
        fprintf(stderr, "[%s] no EGL display\n", g_case);
        return 1;
    }
    g_glctx = gst_gl_context_new(g_display);
    GError *err = NULL;
    if (!gst_gl_context_create(g_glctx, NULL, &err)) {
        fprintf(stderr, "[%s] GL context failed: %s\n", g_case,
                err ? err->message : "?");
        return 1;
    }
    {
        gint maj = 0, min = 0;
        gst_gl_context_get_gl_version(g_glctx, &maj, &min);
        fprintf(stderr, "[%s] GL context ready (EGL, headless, GL %d.%d)\n",
                g_case, maj, min);
    }

    char desc[8192];
    const char *names[2] = {0};
    const char *frags[2] = {0};
    int nf = 0;
    int nbuf = 30;
    int is_16bit = 0;
    int measure = 0;

    if (strcmp(g_case, "fmt-rgba64") == 0) {
        nbuf = 30;
        snprintf(desc, sizeof(desc),
                 SRC_64 " ! glupload ! glcolorconvert ! "
                 "capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA64_LE %s",
                 nbuf, g_width, g_height, g_rate, PROBE_TAIL);
    } else if (strcmp(g_case, "fmt-rgb16") == 0) {
        nbuf = 30;
        snprintf(desc, sizeof(desc),
                 "videotestsrc pattern=smpte num-buffers=%d ! "
                 "video/x-raw,width=%d,height=%d,framerate=%s ! "
                 "videoconvert ! video/x-raw,format=RGB16 ! glupload ! glcolorconvert ! "
                 "capsfilter caps=video/x-raw(memory:GLMemory),format=RGB16 %s",
                 nbuf, g_width, g_height, g_rate, PROBE_TAIL);
    } else if (strcmp(g_case, "fmt-f16") == 0) {
        /* No float format exists in GStreamer's video format enum; expect
         * negotiation to fail. */
        nbuf = 5;
        snprintf(desc, sizeof(desc),
                 "videotestsrc pattern=smpte num-buffers=%d ! "
                 "video/x-raw,width=%d,height=%d,framerate=%s ! "
                 "videoconvert ! video/x-raw,format=RGBA_F16 %s",
                 nbuf, g_width, g_height, g_rate, PROBE_TAIL);
    } else if (strcmp(g_case, "fmt-glshader64") == 0) {
        /* glshader advertises format=RGBA only; force a 16-bit frame into it
         * and expect it to refuse (not-linked). */
        nbuf = 5;
        snprintf(desc, sizeof(desc),
                 SRC_64 " ! glupload ! glcolorconvert ! "
                 "capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA64_LE "
                 "! glshader name=id %s",
                 nbuf, g_width, g_height, g_rate, PROBE_TAIL);
        names[0] = "id"; frags[0] = SHADER_IDENTITY; nf = 1;
    } else if (strcmp(g_case, "transfer") == 0) {
        /* Does glcolorconvert apply the transfer function (gamma)? ref keeps
         * sRGB; lin forces a linear (gamma 1.0) output. Compare means. */
        nbuf = 1;
        snprintf(desc, sizeof(desc),
                 SRC_8 " ! tee name=s "
                 "s. ! queue ! glupload ! glcolorconvert ! "
                 "  capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA,colorimetry=" CLR_SRGB
                 "  ! gldownload ! video/x-raw,format=RGBA ! appsink name=ref sync=false "
                 "s. ! queue ! glupload ! glcolorconvert ! "
                 "  capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA,colorimetry=" CLR_LINEAR
                 "  ! gldownload ! video/x-raw,format=RGBA ! appsink name=rt sync=false",
                 nbuf, g_width, g_height, g_rate);
    } else if (strcmp(g_case, "rtt8") == 0) {
        nbuf = 1;
        is_16bit = 0;
        snprintf(desc, sizeof(desc),
                 SRC_8 " ! tee name=s "
                 "s. ! queue ! glupload ! glcolorconvert ! "
                 "  capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA,colorimetry=" CLR_SRGB
                 "  ! gldownload ! video/x-raw,format=RGBA ! appsink name=ref sync=false "
                 "s. ! queue ! glupload ! glcolorconvert ! "
                 "  capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA,colorimetry=" CLR_LINEAR
                 "  ! glcolorconvert ! "
                 "  capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA,colorimetry=" CLR_SRGB
                 "  ! gldownload ! video/x-raw,format=RGBA ! appsink name=rt sync=false",
                 nbuf, g_width, g_height, g_rate);
    } else if (strcmp(g_case, "rtt64") == 0) {
        nbuf = 1;
        is_16bit = 1;
        snprintf(desc, sizeof(desc),
                 SRC_64 " ! tee name=s "
                 "s. ! queue ! glupload ! glcolorconvert ! "
                 "  capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA64_LE,colorimetry=" CLR_SRGB
                 "  ! gldownload ! video/x-raw,format=RGBA64_LE ! appsink name=ref sync=false "
                 "s. ! queue ! glupload ! glcolorconvert ! "
                 "  capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA64_LE,colorimetry=" CLR_LINEAR
                 "  ! glcolorconvert ! "
                 "  capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA64_LE,colorimetry=" CLR_SRGB
                 "  ! gldownload ! video/x-raw,format=RGBA64_LE ! appsink name=rt sync=false",
                 nbuf, g_width, g_height, g_rate);
    } else if (strcmp(g_case, "noreadback") == 0) {
        measure = 1;
        snprintf(desc, sizeof(desc),
                 "videotestsrc pattern=smpte ! "
                 "video/x-raw,width=%d,height=%d,framerate=%s ! "
                 "videoconvert ! glupload ! glcolorconvert ! "
                 "capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA ! "
                 "glshader name=id ! appsink name=sink sync=false max-buffers=4 drop=false",
                 g_width, g_height, g_rate);
        names[0] = "id"; frags[0] = SHADER_IDENTITY; nf = 1;
    } else if (strcmp(g_case, "tee-download") == 0) {
        measure = 1;
        snprintf(desc, sizeof(desc),
                 "videotestsrc pattern=smpte ! "
                 "video/x-raw,width=%d,height=%d,framerate=%s ! "
                 "videoconvert ! glupload ! glcolorconvert ! "
                 "capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA ! "
                 "tee name=t "
                 "t. ! queue ! glshader name=id ! appsink name=sink sync=false max-buffers=4 drop=false "
                 "t. ! queue ! gldownload ! videoconvert ! video/x-raw,format=RGBA "
                 "  ! appsink name=scopes sync=false max-buffers=1 drop=true",
                 g_width, g_height, g_rate);
        names[0] = "id"; frags[0] = SHADER_IDENTITY; nf = 1;
    } else if (strcmp(g_case, "pbo") == 0) {
        measure = 1;
        snprintf(desc, sizeof(desc),
                 "videotestsrc pattern=smpte ! "
                 "video/x-raw,width=%d,height=%d,framerate=%s ! "
                 "videoconvert ! glupload ! glcolorconvert ! "
                 "capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA ! "
                 "tee name=t "
                 "t. ! queue ! glshader name=id ! appsink name=sink sync=false max-buffers=4 drop=false "
                 "t. ! queue ! glscopes name=sc ! fakesink sync=false",
                 g_width, g_height, g_rate);
        names[0] = "id"; frags[0] = SHADER_IDENTITY; nf = 1;
    } else {
        fprintf(stderr, "unknown case %s\n", g_case);
        return 2;
    }

    fprintf(stderr, "[%s] DESCRIPTION: %s\n", g_case, desc);

    GstElement *pipeline = parse(desc);
    if (!pipeline)
        return 1;
    for (int i = 0; i < nf; i++)
        set_frag(pipeline, names[i], frags[i]);

    int rc;
    if (measure)
        rc = drive_measure(pipeline, g_seconds);
    else if (strcmp(mode, "analyze") == 0)
        rc = drive_analyze(pipeline, is_16bit);
    else
        rc = drive_probe(pipeline, out, strcmp(mode, "capture") == 0);

    gst_object_unref(g_glctx);
    gst_object_unref(g_display);
    return rc;
}
