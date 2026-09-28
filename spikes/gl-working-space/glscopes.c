// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// glscopes implementation. See glscopes.h for the non-blocking design.

#include "glscopes.h"

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
#include <gst/gl/gstglfilter.h>
#include <gst/gl/gstglmemory.h>
#include <gst/gl/gstglframebuffer.h>

#include <stdio.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (glscopes_debug);
#define GST_CAT_DEFAULT glscopes_debug

typedef struct _GstGLScopes GstGLScopes;
typedef struct _GstGLScopesClass GstGLScopesClass;

struct _GstGLScopes
{
  GstGLFilter parent;

  /* Double-buffered pixel-pack PBOs (allocated lazily on the GL thread). */
  guint pbo[2];
  gsize pbo_size;
  gint pbo_idx;           /* PBO to write this frame; the other holds last frame */
  GLsync fence[2];

  /* Identity pass-through shader (created lazily on the GL thread). */
  GstGLShader *shader;

  /* Accumulated 256-bin luma histogram (last completed frame). */
  guint hist[256];
  guint frames_read;
  guint total_px;
  gboolean logged;
};

struct _GstGLScopesClass
{
  GstGLFilterClass parent_class;
};

G_DEFINE_TYPE (GstGLScopes, gst_gl_scopes, GST_TYPE_GL_FILTER);

static gboolean gst_gl_scopes_filter_texture (GstGLFilter * filter,
    GstGLMemory * input, GstGLMemory * output);
static void gst_gl_scopes_gl_stop (GstGLBaseFilter * base);

static void
gst_gl_scopes_class_init (GstGLScopesClass * klass)
{
  GstElementClass *element_class = (GstElementClass *) klass;
  GstGLFilterClass *filter_class = (GstGLFilterClass *) klass;
  GstGLBaseFilterClass *base_class = (GstGLBaseFilterClass *) klass;

  GST_DEBUG_CATEGORY_INIT (glscopes_debug, "glscopes", 0,
      "non-blocking PBO scopes readback");

  /* Same RGBA GLMemory pad templates as stock GstGLFilter (glshader etc.). */
  gst_gl_filter_add_rgba_pad_templates (filter_class);

  filter_class->filter_texture = gst_gl_scopes_filter_texture;
  base_class->gl_stop = gst_gl_scopes_gl_stop;

  gst_element_class_set_static_metadata (element_class,
      "Non-blocking scopes readback", "Filter/Video",
      "Passes frames through and reads a luma histogram via double-buffered PBO",
      "Genesis-0 contributors");
}

static void
gst_gl_scopes_init (GstGLScopes * self)
{
  self->pbo_idx = 0;
}

/* Computes a 256-bin luma histogram from w*h RGBA bytes. */
static void
compute_histogram (GstGLScopes * self, const guint8 * data, gint w, gint h)
{
  guint hist[256] = { 0, };
  gint x, y;

  for (y = 0; y < h; y++) {
    const guint8 *row = data + (gsize) y * w * 4;
    for (x = 0; x < w; x++) {
      guint luma = (row[0] * 299 + row[1] * 587 + row[2] * 114) / 1000;
      hist[luma]++;
      row += 4;
    }
  }
  memcpy (self->hist, hist, sizeof (hist));
  self->total_px = (guint) w * (guint) h;
}

static gboolean
gst_gl_scopes_filter_texture (GstGLFilter * filter, GstGLMemory * input,
    GstGLMemory * output)
{
  GstGLScopes *self = (GstGLScopes *) filter;
  GstGLContext *ctx = GST_GL_BASE_FILTER (filter)->context;
  const GstGLFuncs *gl = ctx->gl_vtable;
  gint w = GST_VIDEO_INFO_WIDTH (&filter->out_info);
  gint h = GST_VIDEO_INFO_HEIGHT (&filter->out_info);
  gsize size = (gsize) w * h * 4;
  gint cur, prev, i;

  /* 1. Visual identity: copy input -> output via the default pass-through
   * shader (NULL shader is not valid here: render_to_target_with_shader calls
   * gst_gl_shader_use on it). */
  if (!self->shader) {
    GError *err = NULL;
    self->shader = gst_gl_shader_new_default (ctx, &err);
    if (!self->shader) {
      GST_ERROR_OBJECT (filter, "default shader failed: %s",
          err ? err->message : "?");
      g_clear_error (&err);
      return FALSE;
    }
  }
  gst_gl_filter_render_to_target_with_shader (filter, input, output,
      self->shader);

  /* 2. Lazy-allocate the two pixel-pack PBOs. */
  if (self->pbo[0] == 0 && gl->GenBuffers) {
    gl->GenBuffers (2, self->pbo);
    for (i = 0; i < 2; i++) {
      gl->BindBuffer (GL_PIXEL_PACK_BUFFER, self->pbo[i]);
      gl->BufferData (GL_PIXEL_PACK_BUFFER, size, NULL, GL_STREAM_READ);
    }
    gl->BindBuffer (GL_PIXEL_PACK_BUFFER, 0);
    self->pbo_size = size;
  }

  if (self->pbo[0] == 0)
    return TRUE;            /* no PBO support; still pass frames through */

  cur = self->pbo_idx;
  prev = cur ^ 1;

  /* 3. Read the current frame into the CURRENT PBO. With a PBO bound, the
   * NULL pointer is an offset into the PBO and glReadPixels returns after
   * queueing the DMA — it does not wait for the transfer. */
  gst_gl_framebuffer_bind (filter->fbo);
  gl->BindBuffer (GL_PIXEL_PACK_BUFFER, self->pbo[cur]);
  gl->PixelStorei (GL_PACK_ALIGNMENT, 1);
  gl->ReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  gl->BindBuffer (GL_PIXEL_PACK_BUFFER, 0);
  if (gl->FenceSync) {
    /* Reusing a PBO whose previous fence never got consumed (its data was
     * dropped): delete the stale sync before overwriting, else it leaks. */
    if (self->fence[cur])
      gl->DeleteSync (self->fence[cur]);
    self->fence[cur] = gl->FenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  }

  /* 4. Map the PREVIOUS frame's PBO only when its fence has already
   * signaled, so glMapBufferRange never blocks the GL thread. On the very
   * first frame there is no previous PBO, so nothing is mapped. */
  if (self->fence[prev]) {
    GLenum res = gl->ClientWaitSync
        ? gl->ClientWaitSync (self->fence[prev], 0, 0) : GL_ALREADY_SIGNALED;
    if (res == GL_ALREADY_SIGNALED || res == GL_CONDITION_SATISFIED) {
      gl->DeleteSync (self->fence[prev]);
      self->fence[prev] = 0;
      gl->BindBuffer (GL_PIXEL_PACK_BUFFER, self->pbo[prev]);
      guint8 *ptr = gl->MapBufferRange (GL_PIXEL_PACK_BUFFER, 0, size,
          GL_MAP_READ_BIT);
      if (ptr) {
        compute_histogram (self, ptr, w, h);
        self->frames_read++;
        gl->UnmapBuffer (GL_PIXEL_PACK_BUFFER);
      }
      gl->BindBuffer (GL_PIXEL_PACK_BUFFER, 0);
    }
  }

  if (!self->logged) {
    gint maj = 0, min = 0;
    gst_gl_context_get_gl_version (ctx, &maj, &min);
    fprintf (stderr,
        "[glscopes] PBO readback active api=%d.%d size=%dx%d pbo=%u,%u\n",
        maj, min, w, h, self->pbo[0], self->pbo[1]);
    self->logged = TRUE;
  }

  self->pbo_idx = prev;     /* alternate for next frame */
  return TRUE;
}

static void
gst_gl_scopes_gl_stop (GstGLBaseFilter * base)
{
  GstGLScopes *self = (GstGLScopes *) base;
  GstGLContext *ctx = base->context;
  const GstGLFuncs *gl = ctx->gl_vtable;

  if (self->frames_read > 0) {
    guint nonzero = 0, mode = 0, mode_count = 0, i;
    guint64 sum = 0;
    for (i = 0; i < 256; i++) {
      if (self->hist[i])
        nonzero++;
      if (self->hist[i] > mode_count) {
        mode_count = self->hist[i];
        mode = i;
      }
      sum += (guint64) self->hist[i] * i;
    }
    fprintf (stderr,
        "[glscopes] frames_read=%u total_px=%u nonzero_bins=%u mode_bin=%u mean_luma=%.1f\n",
        self->frames_read, self->total_px, nonzero, mode,
        self->total_px ? (double) sum / self->total_px : 0.0);
  } else {
    fprintf (stderr, "[glscopes] frames_read=0 (no completed PBO readback)\n");
  }

  if (self->pbo[0] && gl->DeleteBuffers) {
    gl->DeleteBuffers (2, self->pbo);
    self->pbo[0] = self->pbo[1] = 0;
  }
  if (self->fence[0] && gl->DeleteSync)
    gl->DeleteSync (self->fence[0]);
  if (self->fence[1] && gl->DeleteSync)
    gl->DeleteSync (self->fence[1]);
  self->fence[0] = self->fence[1] = 0;
  if (self->shader) {
    gst_object_unref (self->shader);
    self->shader = NULL;
  }

  GST_GL_BASE_FILTER_CLASS (gst_gl_scopes_parent_class)->gl_stop (base);
}
