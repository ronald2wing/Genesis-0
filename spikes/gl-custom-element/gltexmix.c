// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// gltexmix: a GstGLMixer subclass that samples N input textures in one
// fragment shader, each bound to a distinct texture unit with
// glUniform1i. This is the custom-element escape hatch that
// docs/decisions/effects-execution.md section 6 leaves open: whether a
// custom element can bind named intermediates (N textures into one
// shader) where stock glshader cannot (glshader binds exactly one texture
// per element and a second sampler2D silently aliases unit 0).
//
// Why GstGLMixer and not GstGLFilter:
//   - GstGLFilter (gstglfilter.h) is a GstBaseTransform: one sink pad, one
//     src pad, and its vfuncs take a single input texture
//     (filter_texture(GstGLFilter*, GstGLMemory *input, GstGLMemory *output);
//      filter(GstGLFilter*, GstBuffer *inbuf, GstBuffer *outbuf)).
//   - glfilterapp's client-draw signal passes exactly one `guint texture`.
//   - The framework's multi-input GL element is GstGLMixer (since 1.24): a
//     GstVideoAggregator with request sink pads, each a GstGLMixerPad that
//     carries `current_texture` (the mapped input texture id for that pad).
//     process_textures(mix, out_tex) is invoked once per aggregated frame
//     with every pad's texture available simultaneously.

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/base/gstaggregator.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
#include <gst/gl/gstglmixer.h>
#include <gst/gl/gstglshader.h>
#include <gst/gl/gstglslstage.h>
#include <gst/gl/gstglmemory.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gltexmix_debug);
#define GST_CAT_DEFAULT gltexmix_debug

#define MAX_INPUTS 8

typedef struct _GstGLTexMix GstGLTexMix;
typedef struct _GstGLTexMixClass GstGLTexMixClass;

struct _GstGLTexMix
{
  GstGLMixer parent;

  gchar *fragment;

  /* GL resources, created lazily on the GL thread. */
  GstGLShader *shader;
  gboolean shader_compiled;

  guint vao;
  guint vertex_buffer;
  guint vbo_indices;
  gint attr_position;
  gint attr_texcoord;
  gboolean quad_ready;

  /* Per-render scratch (valid for the duration of one process_textures). */
  GstGLMemory *out_tex;
  gboolean gl_result;

  gboolean logged;
};

struct _GstGLTexMixClass
{
  GstGLMixerClass parent_class;
};

#define GST_TYPE_GL_TEX_MIX (gst_gl_tex_mix_get_type ())
GType gst_gl_tex_mix_get_type (void);

G_DEFINE_TYPE (GstGLTexMix, gst_gl_tex_mix, GST_TYPE_GL_MIXER);

enum
{
  PROP_0,
  PROP_FRAGMENT,
};

/* Vertex shader the element injects (it owns the whole pipeline; no stock
 * element is involved). Attributes match the fullscreen quad drawn below. */
static const gchar VERTEX_SRC[] =
    "attribute vec4 a_position;"
    "attribute vec2 a_texcoord;"
    "varying vec2 v_texcoord;"
    "void main () { gl_Position = a_position; v_texcoord = a_texcoord; }";

static void gst_gl_tex_mix_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_gl_tex_mix_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean gst_gl_tex_mix_process_textures (GstGLMixer * mix,
    GstGLMemory * out_tex);
static void gst_gl_tex_mix_gl_stop (GstGLBaseMixer * mix);
static void gst_gl_tex_mix_finalize (GObject * object);

static void
gst_gl_tex_mix_class_init (GstGLTexMixClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = (GstElementClass *) klass;
  GstGLMixerClass *mixer_class = (GstGLMixerClass *) klass;
  GstGLBaseMixerClass *base_class = (GstGLBaseMixerClass *) klass;

  GST_DEBUG_CATEGORY_INIT (gltexmix_debug, "gltexmix", 0,
      "N-input single-shader GL mixer");

  /* Same RGBA GLMemory pad templates as stock GstGLMixer (sink_%u request
   * pads typed GstGLMixerPad, src typed GstAggregatorPad). */
  gst_gl_mixer_class_add_rgba_pad_templates (mixer_class);
  g_type_class_ref (GST_TYPE_GL_MIXER_PAD);

  gobject_class->set_property = gst_gl_tex_mix_set_property;
  gobject_class->get_property = gst_gl_tex_mix_get_property;
  gobject_class->finalize = gst_gl_tex_mix_finalize;

  g_object_class_install_property (gobject_class, PROP_FRAGMENT,
      g_param_spec_string ("fragment", "Fragment", "GLSL fragment source",
          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  mixer_class->process_textures = gst_gl_tex_mix_process_textures;
  base_class->gl_stop = gst_gl_tex_mix_gl_stop;

  gst_element_class_set_static_metadata (element_class,
      "N-input single-shader GL mixer", "Filter/Effect",
      "Samples N input GL textures in one fragment shader",
      "Genesis-0 contributors");
}

static void
gst_gl_tex_mix_init (GstGLTexMix * self)
{
  self->attr_position = -1;
  self->attr_texcoord = -1;
}

static void
gst_gl_tex_mix_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstGLTexMix *self = (GstGLTexMix *) object;

  switch (prop_id) {
    case PROP_FRAGMENT:
      g_free (self->fragment);
      self->fragment = g_value_dup_string (value);
      self->shader_compiled = FALSE;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_tex_mix_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstGLTexMix *self = (GstGLTexMix *) object;

  switch (prop_id) {
    case PROP_FRAGMENT:
      g_value_set_string (value, self->fragment);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_tex_mix_finalize (GObject * object)
{
  GstGLTexMix *self = (GstGLTexMix *) object;

  g_free (self->fragment);
  self->fragment = NULL;

  G_OBJECT_CLASS (gst_gl_tex_mix_parent_class)->finalize (object);
}

static gboolean
gst_gl_tex_mix_ensure_shader (GstGLTexMix * self)
{
  GstGLContext *ctx = GST_GL_BASE_MIXER (self)->context;
  GstGLShader *shader;
  GstGLSLStage *stage;
  GError *error = NULL;

  if (self->shader_compiled && self->shader)
    return TRUE;

  if (self->fragment == NULL) {
    GST_ERROR_OBJECT (self, "no fragment source set");
    return FALSE;
  }

  shader = gst_gl_shader_new (ctx);
  if (!shader)
    return FALSE;

  stage = gst_glsl_stage_new_with_string (ctx, GL_VERTEX_SHADER,
      GST_GLSL_VERSION_NONE, GST_GLSL_PROFILE_NONE, VERTEX_SRC);
  if (!stage || !gst_gl_shader_compile_attach_stage (shader, stage, &error)) {
    if (stage)
      gst_object_unref (stage);
    goto fail;
  }
  // On success the shader owns the stage reference (gst_gl_shader_attach_unlocked
  // takes it); unrefing here would leave the shader holding a freed stage.

  stage = gst_glsl_stage_new_with_string (ctx, GL_FRAGMENT_SHADER,
      GST_GLSL_VERSION_NONE, GST_GLSL_PROFILE_NONE, self->fragment);
  if (!stage || !gst_gl_shader_compile_attach_stage (shader, stage, &error)) {
    if (stage)
      gst_object_unref (stage);
    goto fail;
  }

  if (!gst_gl_shader_link (shader, &error))
    goto fail;

  if (self->shader)
    gst_object_unref (self->shader);
  self->shader = shader;
  self->shader_compiled = TRUE;
  self->attr_position =
      gst_gl_shader_get_attribute_location (shader, "a_position");
  self->attr_texcoord =
      gst_gl_shader_get_attribute_location (shader, "a_texcoord");
  return TRUE;

fail:
  GST_ERROR_OBJECT (self, "shader compile failed: %s",
      error ? error->message : "unknown");
  g_clear_error (&error);
  if (shader)
    gst_object_unref (shader);
  return FALSE;
}

static gboolean
gst_gl_tex_mix_ensure_quad (GstGLTexMix * self)
{
  GstGLContext *ctx = GST_GL_BASE_MIXER (self)->context;
  const GstGLFuncs *gl = ctx->gl_vtable;

  if (self->quad_ready)
    return TRUE;

  /* position (3f) + texcoord (2f) interleaved, matching VERTEX_SRC. */
  static const GLfloat vertices[] = {
    -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
     1.0f,  1.0f, 0.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f, 1.0f,
  };
  static const GLushort indices[] = { 0, 1, 2, 0, 2, 3 };

  if (gl->GenVertexArrays) {
    gl->GenVertexArrays (1, &self->vao);
    gl->BindVertexArray (self->vao);
  }

  gl->GenBuffers (1, &self->vertex_buffer);
  gl->BindBuffer (GL_ARRAY_BUFFER, self->vertex_buffer);
  gl->BufferData (GL_ARRAY_BUFFER, sizeof (vertices), vertices, GL_STATIC_DRAW);

  gl->GenBuffers (1, &self->vbo_indices);
  gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, self->vbo_indices);
  gl->BufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof (indices), indices,
      GL_STATIC_DRAW);

  if (gl->GenVertexArrays)
    gl->BindVertexArray (0);

  self->quad_ready = TRUE;
  return TRUE;
}

static void
gst_gl_tex_mix_draw_quad (GstGLTexMix * self)
{
  GstGLContext *ctx = GST_GL_BASE_MIXER (self)->context;
  const GstGLFuncs *gl = ctx->gl_vtable;

  if (gl->GenVertexArrays)
    gl->BindVertexArray (self->vao);

  gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, self->vbo_indices);
  gl->BindBuffer (GL_ARRAY_BUFFER, self->vertex_buffer);

  gl->VertexAttribPointer (self->attr_position, 3, GL_FLOAT, GL_FALSE,
      5 * sizeof (GLfloat), (void *) 0);
  gl->VertexAttribPointer (self->attr_texcoord, 2, GL_FLOAT, GL_FALSE,
      5 * sizeof (GLfloat), (void *) (3 * sizeof (GLfloat)));

  gl->EnableVertexAttribArray (self->attr_position);
  gl->EnableVertexAttribArray (self->attr_texcoord);

  gl->DrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

  gl->DisableVertexAttribArray (self->attr_position);
  gl->DisableVertexAttribArray (self->attr_texcoord);

  if (gl->GenVertexArrays) {
    gl->BindVertexArray (0);
  } else {
    gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
    gl->BindBuffer (GL_ARRAY_BUFFER, 0);
  }
}

typedef struct
{
  gint idx;
  guint tex;
} TexSlot;

static gint
slot_cmp (const void *a, const void *b)
{
  const TexSlot *sa = a;
  const TexSlot *sb = b;
  return sa->idx - sb->idx;
}

static gboolean
gst_gl_tex_mix_render (gpointer data)
{
  GstGLTexMix *self = data;
  GstGLContext *ctx = GST_GL_BASE_MIXER (self)->context;
  const GstGLFuncs *gl = ctx->gl_vtable;
  TexSlot slots[MAX_INPUTS];
  gint n = 0;
  GstIterator *it;
  gboolean done = FALSE;
  gint i;

  if (!gst_gl_tex_mix_ensure_shader (self))
    return FALSE;
  if (!gst_gl_tex_mix_ensure_quad (self))
    return FALSE;

  /* Collect every sink pad's current input texture, keyed by its numeric
   * suffix so tex0/tex1/... map deterministically to sink_0/sink_1/... */
  it = gst_element_iterate_sink_pads (GST_ELEMENT (self));
  while (!done && n < MAX_INPUTS) {
    GValue val = G_VALUE_INIT;
    switch (gst_iterator_next (it, &val)) {
      case GST_ITERATOR_OK:{
        GstPad *pad = g_value_get_object (&val);
        gint idx = 0;
        const gchar *name = GST_PAD_NAME (pad);
        if (sscanf (name, "sink_%d", &idx) != 1)
          idx = n;
        slots[n].idx = idx;
        slots[n].tex = GST_GL_MIXER_PAD (pad)->current_texture;
        n++;
        g_value_unset (&val);
        break;
      }
      case GST_ITERATOR_RESYNC:
        n = 0;
        g_value_unset (&val);
        break;
      default:
        done = TRUE;
        break;
    }
  }
  gst_iterator_free (it);

  qsort (slots, n, sizeof (TexSlot), slot_cmp);

  gst_gl_shader_use (self->shader);
  for (i = 0; i < n; i++) {
    gchar name[16];
    gl->ActiveTexture (GL_TEXTURE0 + i);
    gl->BindTexture (GL_TEXTURE_2D, slots[i].tex);
    g_snprintf (name, sizeof (name), "tex%d", i);
    gst_gl_shader_set_uniform_1i (self->shader, name, i);
  }

  if (!self->logged) {
    gchar *ids = g_strdup ("");
    for (i = 0; i < n; i++) {
      gchar *t = g_strdup_printf ("%s%s%d", ids, i ? "," : "", slots[i].tex);
      g_free (ids);
      ids = t;
    }
    fprintf (stderr, "[gltexmix] inputs=%d texture-ids=[%s]\n", n, ids);
    g_free (ids);
    self->logged = TRUE;
  }

  gst_gl_tex_mix_draw_quad (self);

  return TRUE;
}

static void
gst_gl_tex_mix_process_gl (GstGLContext * ctx, GstGLTexMix * self)
{
  GstGLMixer *mix = GST_GL_MIXER (self);
  GstGLFramebuffer *fbo = gst_gl_mixer_get_framebuffer (mix);

  self->gl_result = gst_gl_framebuffer_draw_to_texture (fbo, self->out_tex,
      gst_gl_tex_mix_render, self);
}

static gboolean
gst_gl_tex_mix_process_textures (GstGLMixer * mix, GstGLMemory * out_tex)
{
  GstGLTexMix *self = (GstGLTexMix *) mix;
  GstGLContext *ctx = GST_GL_BASE_MIXER (mix)->context;

  self->out_tex = out_tex;
  gst_gl_context_thread_add (ctx,
      (GstGLContextThreadFunc) gst_gl_tex_mix_process_gl, self);
  return self->gl_result;
}

static void
gst_gl_tex_mix_gl_stop (GstGLBaseMixer * mix)
{
  GstGLTexMix *self = (GstGLTexMix *) mix;
  GstGLContext *ctx = mix->context;
  const GstGLFuncs *gl = ctx->gl_vtable;

  if (self->shader) {
    gst_object_unref (self->shader);
    self->shader = NULL;
  }
  self->shader_compiled = FALSE;

  if (self->vao && gl->GenVertexArrays) {
    gl->DeleteVertexArrays (1, &self->vao);
    self->vao = 0;
  }
  if (self->vertex_buffer) {
    gl->DeleteBuffers (1, &self->vertex_buffer);
    self->vertex_buffer = 0;
  }
  if (self->vbo_indices) {
    gl->DeleteBuffers (1, &self->vbo_indices);
    self->vbo_indices = 0;
  }
  self->quad_ready = FALSE;

  GST_GL_BASE_MIXER_CLASS (gst_gl_tex_mix_parent_class)->gl_stop (mix);
}
