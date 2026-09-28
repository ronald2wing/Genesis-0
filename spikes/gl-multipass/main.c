// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Phase 4 gate spike: can GStreamer's GL elements express a non-linear
// multi-pass effect (a named intermediate written by one pass and read by a
// later one)? This is a headless EGL probe. Each case builds a videotestsrc
// pipeline, answers the GL NEED_CONTEXT on the bus, classifies the frame
// memory at the appsink (probe mode), and in capture mode writes PNGs through
// gldownload + pngenc + multifilesink so a later pass can compare pixels.
//
// Usage: gl-multipass <case> <probe|capture> [out-template]
//   <case>         one of: baseline, chain, named-linear,
//                          named-extra-sampler, fanout, approx-2pass,
//                          approx-fold, shrink
//   <out-template> printf template for multifilesink (capture mode only),
//                  e.g. /tmp/opencode/gl-multipass/chain-%02d.png

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/gl/gl.h>
#include <gst/gl/egl/gstgldisplay_egl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shaders.h"

#define SRC_HEAD \
    "videotestsrc pattern=smpte num-buffers=%d ! " \
    "video/x-raw,width=320,height=240,framerate=30/1 ! " \
    "videoconvert ! glupload ! glcolorconvert ! " \
    "capsfilter caps=video/x-raw(memory:GLMemory),format=RGBA"

// Every pipeline ends in a tee fanning out to (a) an appsink that proves GL
// residency, and (b) a download branch that materializes PNGs.
#define PROBE_TAIL \
    " ! tee name=t t. ! queue ! appsink name=sink sync=false max-buffers=1 drop=true " \
    "t. ! queue ! fakesink"

#define CAPTURE_TAIL \
    " ! tee name=t t. ! queue ! appsink name=sink sync=false max-buffers=1 drop=true " \
    "t. ! queue ! gldownload ! videoconvert ! pngenc ! multifilesink name=f"

static GstGLDisplay *g_display = NULL;
static GstGLContext *g_glctx = NULL;
static int g_frames = 0;
static int g_glmem = 0;
static const char *g_case = "";

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

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer data) {
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
            const char *mem = gst_caps_features_contains(feat, "memory:GLMemory")
                                  ? "GLMemory"
                              : gst_caps_features_contains(feat, "memory:DMABuf")
                                  ? "DMABuf"
                              : gst_caps_features_contains(feat,
                                                           "memory:SystemMemory")
                                  ? "SystemMemory"
                                  : "other";
            fprintf(stderr, "[%s] FIRST FRAME caps: %s\n", g_case, s);
            fprintf(stderr, "[%s] FIRST FRAME memory: %s\n", g_case, mem);
            g_free(s);
        }
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

// Plays a fully-built pipeline to EOS or error and reports the memory verdict.
static int drive(GstElement *pipeline, const char *out, int is_capture) {
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
    fprintf(stderr, "[%s] frames=%d glmem_frames=%d\n", g_case, g_frames,
            g_glmem);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return 0;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <case> <probe|capture> [out-template]\n",
                argv[0]);
        return 2;
    }
    g_case = argv[1];
    const int is_capture = strcmp(argv[2], "capture") == 0;
    const char *out = argc > 3 ? argv[3] : NULL;
    const char *tail = is_capture ? CAPTURE_TAIL : PROBE_TAIL;
    const int nbuf = is_capture ? 5 : 30;

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
    fprintf(stderr, "[%s] GL context ready (EGL, headless)\n", g_case);

    char desc[4096];
    const char *names[3] = {0};
    const char *frags[3] = {0};
    int nf = 0;
    int is_fanout = 0;

    if (strcmp(g_case, "baseline") == 0) {
        snprintf(desc, sizeof(desc), SRC_HEAD "%s", nbuf, tail);
    } else if (strcmp(g_case, "chain") == 0) {
        snprintf(desc, sizeof(desc),
                 SRC_HEAD " ! glshader name=passA ! glshader name=passB%s",
                 nbuf, tail);
        names[0] = "passA"; frags[0] = SHADER_IDENTITY; nf = 1;
        names[1] = "passB"; frags[1] = SHADER_INVERT; nf = 2;
    } else if (strcmp(g_case, "named-linear") == 0) {
        snprintf(desc, sizeof(desc),
                 SRC_HEAD " ! glshader name=passA ! glshader name=passB"
                          " ! glshader name=passC%s",
                 nbuf, tail);
        names[0] = "passA"; frags[0] = SHADER_RED; nf = 1;
        names[1] = "passB"; frags[1] = SHADER_GREEN; nf = 2;
        names[2] = "passC"; frags[2] = SHADER_IDENTITY; nf = 3;
    } else if (strcmp(g_case, "named-extra-sampler") == 0) {
        snprintf(desc, sizeof(desc), SRC_HEAD " ! glshader name=passA%s", nbuf,
                 tail);
        names[0] = "passA"; frags[0] = SHADER_SECOND_SAMPLER; nf = 1;
    } else if (strcmp(g_case, "fanout") == 0) {
        snprintf(desc, sizeof(desc),
                 SRC_HEAD
                 " ! tee name=src src. ! queue ! glshader name=branchA ! mix.sink_0"
                 " src. ! queue ! glshader name=branchB ! mix.sink_1"
                 " glvideomixer name=mix background=1 mix. %s",
                 nbuf, tail);
        names[0] = "branchA"; frags[0] = SHADER_RED; nf = 1;
        names[1] = "branchB"; frags[1] = SHADER_GREEN; nf = 2;
        is_fanout = 1;
    } else if (strcmp(g_case, "approx-2pass") == 0) {
        snprintf(desc, sizeof(desc),
                 SRC_HEAD " ! glshader name=passA ! glshader name=passB%s",
                 nbuf, tail);
        names[0] = "passA"; frags[0] = SHADER_HBLUR; nf = 1;
        names[1] = "passB"; frags[1] = SHADER_VBLUR; nf = 2;
    } else if (strcmp(g_case, "approx-fold") == 0) {
        snprintf(desc, sizeof(desc), SRC_HEAD " ! glshader name=passA%s", nbuf,
                 tail);
        names[0] = "passA"; frags[0] = SHADER_FOLD_BLUR; nf = 1;
    } else if (strcmp(g_case, "shrink") == 0) {
        snprintf(desc, sizeof(desc),
                 SRC_HEAD
                 " ! glshader name=passA ! video/x-raw,width=160,height=120"
                 " ! glshader name=passB%s",
                 nbuf, tail);
        names[0] = "passA"; frags[0] = SHADER_HBLUR; nf = 1;
        names[1] = "passB"; frags[1] = SHADER_IDENTITY; nf = 2;
    } else {
        fprintf(stderr, "unknown case %s\n", g_case);
        return 2;
    }

    fprintf(stderr, "[%s] DESCRIPTION: %s\n", g_case, desc);

    GError *perr = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &perr);
    if (!pipeline) {
        fprintf(stderr, "[%s] PARSE FAILED: %s\n", g_case,
                perr ? perr->message : "?");
        if (perr)
            g_error_free(perr);
        return 1;
    }
    for (int i = 0; i < nf; i++)
        set_frag(pipeline, names[i], frags[i]);

    if (is_fanout) {
        GstElement *mix = gst_bin_get_by_name(GST_BIN(pipeline), "mix");
        GstPad *p0 = gst_element_get_static_pad(mix, "sink_0");
        GstPad *p1 = gst_element_get_static_pad(mix, "sink_1");
        if (!p0 || !p1) {
            fprintf(stderr, "[%s] mixer request pads missing\n", g_case);
            return 1;
        }
        g_object_set(p0, "xpos", 0, "ypos", 0, "width", 160, "height", 240,
                     NULL);
        g_object_set(p1, "xpos", 160, "ypos", 0, "width", 160, "height", 240,
                     NULL);
        gst_object_unref(p0);
        gst_object_unref(p1);
        gst_object_unref(mix);
    }

    int rc = drive(pipeline, out, is_capture);

    gst_object_unref(g_glctx);
    gst_object_unref(g_display);
    return rc;
}
