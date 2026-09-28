// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/effects/TextureMix.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <graphene-gobject.h>
#include <gst/gst.h>
#include <gst/gl/gstglframebuffer.h>
#include <gst/gl/gstglfuncs.h>
#include <gst/gl/gstglmemory.h>
#include <gst/gl/gstglmixer.h>
#include <gst/gl/gstglshader.h>
#include <gst/gl/gstglslstage.h>

// The element itself lives at file scope: G_DEFINE_TYPE generates the
// get_type/class-init machinery at global scope, so the subclass cannot sit
// inside the namespace. The public engine-neutral surface (register_texture_mix,
// make_texture_mix) wraps it at the bottom.
//
// Why GstGLMixer, not GstGLFilter: a filter has one sink pad and its vfuncs
// take a single input texture, so a second sampler2D would silently alias unit
// 0 (docs/decisions/effects-execution.md §6). GstGLMixer (a GstVideoAggregator,
// since 1.24) has request sink pads, each a GstGLMixerPad carrying a
// current_texture, and process_textures sees every pad's texture at once.

GST_DEBUG_CATEGORY_STATIC(texture_mix_debug);
#define GST_CAT_DEFAULT texture_mix_debug

#define TEXTURE_MIX_MAX_INPUTS 8

typedef struct _TextureMix TextureMix;
typedef struct _TextureMixClass TextureMixClass;

struct _TextureMix
{
    GstGLMixer parent;

    gchar *fragment;
    GstStructure *uniforms;

    // GL resources, created lazily on the GL thread (the render callback).
    GstGLShader *shader;
    gboolean shader_compiled;

    guint vao;
    guint vertex_buffer;
    guint vbo_indices;
    gint attr_position;
    gint attr_texcoord;
    gboolean quad_ready;

    // A 1x1 sentinel texture any missing input samples, so a sampler beyond
    // the connected inputs never aliases unit 0.
    guint sentinel_texture;
    gboolean sentinel_ready;

    // Per-render scratch, valid for one process_textures call.
    GstGLMemory *out_tex;
    gboolean gl_result;
    gboolean error_posted;
};

struct _TextureMixClass
{
    GstGLMixerClass parent_class;
};

G_DEFINE_TYPE(TextureMix, texture_mix, GST_TYPE_GL_MIXER);

// The vertex stage the element injects; it owns the whole pipeline, no stock
// element is involved. Attributes match the fullscreen quad below.
static const gchar kVertexSrc[] =
        "attribute vec4 a_position;"
        "attribute vec2 a_texcoord;"
        "varying vec2 v_texcoord;"
        "void main () { gl_Position = a_position; v_texcoord = a_texcoord; }";

// The fragment prelude the element owns: precision, the varying, and one
// sampler per possible input. A body references tex0, tex1, ... and the
// element binds each to its own unit with glUniform1i. Unused samplers are
// optimized out by the driver, so declaring all eight costs nothing for a
// two-input body.
static const gchar kPrelude[] = "precision mediump float;\n"
                                "varying vec2 v_texcoord;\n"
                                "uniform sampler2D tex0;\n"
                                "uniform sampler2D tex1;\n"
                                "uniform sampler2D tex2;\n"
                                "uniform sampler2D tex3;\n"
                                "uniform sampler2D tex4;\n"
                                "uniform sampler2D tex5;\n"
                                "uniform sampler2D tex6;\n"
                                "uniform sampler2D tex7;\n";

enum { PROP_0, PROP_FRAGMENT, PROP_UNIFORMS };

// The RGBA GLMemory pad templates, declared by hand instead of via
// gst_gl_mixer_class_add_rgba_pad_templates(): that helper's static templates
// carry no texture-target, so a downstream sees GLMemory caps that omit the 2D
// target. The src advertises texture-target=(string)2D, mirroring glstereomix;
// the sink stays exactly as the helper installs it (request pads, no
// texture-target) so input negotiation is unchanged.
static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE(
        "src", GST_PAD_SRC, GST_PAD_ALWAYS,
        GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_GL_MEMORY,
                                                          "RGBA") ", "
                                                                  "texture-target = (string) 2D"));

static GstStaticPadTemplate sink_factory =
        GST_STATIC_PAD_TEMPLATE("sink_%u", GST_PAD_SINK, GST_PAD_REQUEST,
                                GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(
                                        GST_CAPS_FEATURE_MEMORY_GL_MEMORY, "RGBA")));

static void texture_mix_set_property(GObject *object, guint prop_id, const GValue *value,
                                     GParamSpec *pspec);
static void texture_mix_get_property(GObject *object, guint prop_id, GValue *value,
                                     GParamSpec *pspec);
static gboolean texture_mix_process_textures(GstGLMixer *mix, GstGLMemory *out_tex);
static void texture_mix_gl_stop(GstGLBaseMixer *mix);
static void texture_mix_finalize(GObject *object);

static void texture_mix_class_init(TextureMixClass *klass)
{
    GObjectClass *gobject_class = (GObjectClass *)klass;
    GstElementClass *element_class = (GstElementClass *)klass;
    GstGLMixerClass *mixer_class = (GstGLMixerClass *)klass;
    GstGLBaseMixerClass *base_class = (GstGLBaseMixerClass *)klass;

    GST_DEBUG_CATEGORY_INIT(texture_mix_debug, "texturemix", 0, "N-input single-shader GL mixer");

    // The same RGBA GLMemory pad templates as stock GstGLMixer (request sink
    // pads typed GstGLMixerPad, src typed GstAggregatorPad), declared by hand
    // so the src carries texture-target=(string)2D. Same gtypes the helper
    // uses - wrong gtypes break pad creation.
    gst_element_class_add_static_pad_template_with_gtype(element_class, &src_factory,
                                                         GST_TYPE_AGGREGATOR_PAD);
    gst_element_class_add_static_pad_template_with_gtype(element_class, &sink_factory,
                                                         GST_TYPE_GL_MIXER_PAD);
    g_type_class_ref(GST_TYPE_GL_MIXER_PAD);

    gobject_class->set_property = texture_mix_set_property;
    gobject_class->get_property = texture_mix_get_property;
    gobject_class->finalize = texture_mix_finalize;

    g_object_class_install_property(
            gobject_class, PROP_FRAGMENT,
            g_param_spec_string(
                    "fragment", "Fragment", "GLSL fragment body", NULL,
                    static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(
            gobject_class, PROP_UNIFORMS,
            g_param_spec_boxed(
                    "uniforms", "Uniforms", "Uniforms for the shader", GST_TYPE_STRUCTURE,
                    static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    mixer_class->process_textures = texture_mix_process_textures;
    base_class->gl_stop = texture_mix_gl_stop;

    gst_element_class_set_static_metadata(
            element_class, "N-input single-shader GL mixer", "Filter/Effect",
            "Samples N input GL textures in one fragment shader", "Genesis-0 contributors");
}

static void texture_mix_init(TextureMix *self)
{
    self->attr_position = -1;
    self->attr_texcoord = -1;
}

static void texture_mix_set_property(GObject *object, guint prop_id, const GValue *value,
                                     GParamSpec *pspec)
{
    TextureMix *self = (TextureMix *)object;

    switch (prop_id) {
    case PROP_FRAGMENT:
        g_free(self->fragment);
        self->fragment = g_value_dup_string(value);
        self->shader_compiled = FALSE;
        break;
    case PROP_UNIFORMS: {
        GstStructure *structure = (GstStructure *)g_value_dup_boxed(value);
        g_clear_pointer(&self->uniforms, gst_structure_free);
        self->uniforms = structure;
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void texture_mix_get_property(GObject *object, guint prop_id, GValue *value,
                                     GParamSpec *pspec)
{
    TextureMix *self = (TextureMix *)object;

    switch (prop_id) {
    case PROP_FRAGMENT:
        g_value_set_string(value, self->fragment);
        break;
    case PROP_UNIFORMS:
        g_value_set_boxed(value, self->uniforms);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void texture_mix_finalize(GObject *object)
{
    TextureMix *self = (TextureMix *)object;

    g_free(self->fragment);
    self->fragment = NULL;
    g_clear_pointer(&self->uniforms, gst_structure_free);

    G_OBJECT_CLASS(texture_mix_parent_class)->finalize(object);
}

// Compiles the vertex stage and the full fragment (the element's prelude plus
// the body) and links them. Runs on the GL thread, inside the render callback.
static gboolean texture_mix_ensure_shader(TextureMix *self)
{
    GstGLContext *ctx = GST_GL_BASE_MIXER(self)->context;
    GstGLShader *shader;
    GstGLSLStage *stage;
    GError *error = NULL;

    if (self->shader_compiled && self->shader != NULL) {
        return TRUE;
    }
    if (self->fragment == NULL || self->fragment[0] == '\0') {
        GST_ERROR_OBJECT(self, "no fragment source set");
        return FALSE;
    }

    shader = gst_gl_shader_new(ctx);
    if (shader == NULL) {
        return FALSE;
    }

    gchar *fragment = g_strconcat(kPrelude, self->fragment, NULL);
    if (fragment == NULL) {
        gst_object_unref(shader);
        return FALSE;
    }

    stage = gst_glsl_stage_new_with_string(ctx, GL_VERTEX_SHADER, GST_GLSL_VERSION_NONE,
                                           GST_GLSL_PROFILE_NONE, kVertexSrc);
    if (stage == NULL || !gst_gl_shader_compile_attach_stage(shader, stage, &error)) {
        if (stage != NULL) {
            gst_object_unref(stage);
        }
        g_clear_error(&error);
        goto fail;
    }
    // Ownership trap: compile_attach_stage takes the stage reference on
    // success, so unrefing here would be a double-unref (the assertion plus
    // use-after-free documented in docs/decisions/effects-execution.md §6).

    stage = gst_glsl_stage_new_with_string(ctx, GL_FRAGMENT_SHADER, GST_GLSL_VERSION_NONE,
                                           GST_GLSL_PROFILE_NONE, fragment);
    g_free(fragment);
    if (stage == NULL || !gst_gl_shader_compile_attach_stage(shader, stage, &error)) {
        if (stage != NULL) {
            gst_object_unref(stage);
        }
        g_clear_error(&error);
        goto fail;
    }

    if (!gst_gl_shader_link(shader, &error)) {
        goto fail;
    }

    if (self->shader != NULL) {
        gst_object_unref(self->shader);
    }
    self->shader = shader;
    self->shader_compiled = TRUE;
    self->attr_position = gst_gl_shader_get_attribute_location(shader, "a_position");
    self->attr_texcoord = gst_gl_shader_get_attribute_location(shader, "a_texcoord");
    return TRUE;

fail:
    GST_ERROR_OBJECT(self, "shader compile failed: %s", error != NULL ? error->message : "unknown");
    g_clear_error(&error);
    gst_object_unref(shader);
    return FALSE;
}

static gboolean texture_mix_ensure_quad(TextureMix *self)
{
    GstGLContext *ctx = GST_GL_BASE_MIXER(self)->context;
    const GstGLFuncs *gl = ctx->gl_vtable;

    if (self->quad_ready) {
        return TRUE;
    }

    // position (3f) + texcoord (2f) interleaved, matching kVertexSrc.
    static const GLfloat vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f,  -1.0f, 0.0f, 1.0f, 0.0f,
        1.0f,  1.0f,  0.0f, 1.0f, 1.0f, -1.0f, 1.0f,  0.0f, 0.0f, 1.0f,
    };
    static const GLushort indices[] = { 0, 1, 2, 0, 2, 3 };

    if (gl->GenVertexArrays != NULL) {
        gl->GenVertexArrays(1, &self->vao);
        gl->BindVertexArray(self->vao);
    }

    gl->GenBuffers(1, &self->vertex_buffer);
    gl->BindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer);
    gl->BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    gl->GenBuffers(1, &self->vbo_indices);
    gl->BindBuffer(GL_ELEMENT_ARRAY_BUFFER, self->vbo_indices);
    gl->BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    if (gl->GenVertexArrays != NULL) {
        gl->BindVertexArray(0);
    }

    self->quad_ready = TRUE;
    return TRUE;
}

// The 1x1 sentinel texture any missing input samples: a distinguishable opaque
// magenta rather than an aliased earlier input. Guards the case where a body
// references more samplers than the pipeline connected.
static gboolean texture_mix_ensure_sentinel(TextureMix *self)
{
    GstGLContext *ctx = GST_GL_BASE_MIXER(self)->context;
    const GstGLFuncs *gl = ctx->gl_vtable;

    if (self->sentinel_ready) {
        return TRUE;
    }

    static const GLubyte magenta[4] = { 255, 0, 255, 255 };

    gl->GenTextures(1, &self->sentinel_texture);
    gl->BindTexture(GL_TEXTURE_2D, self->sentinel_texture);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, magenta);
    gl->BindTexture(GL_TEXTURE_2D, 0);

    self->sentinel_ready = TRUE;
    return TRUE;
}

static void texture_mix_draw_quad(TextureMix *self)
{
    GstGLContext *ctx = GST_GL_BASE_MIXER(self)->context;
    const GstGLFuncs *gl = ctx->gl_vtable;

    if (gl->GenVertexArrays != NULL) {
        gl->BindVertexArray(self->vao);
    }

    gl->BindBuffer(GL_ELEMENT_ARRAY_BUFFER, self->vbo_indices);
    gl->BindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer);

    gl->VertexAttribPointer(self->attr_position, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                            (void *)0);
    gl->VertexAttribPointer(self->attr_texcoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                            (void *)(3 * sizeof(GLfloat)));

    gl->EnableVertexAttribArray(self->attr_position);
    gl->EnableVertexAttribArray(self->attr_texcoord);

    gl->DrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

    gl->DisableVertexAttribArray(self->attr_position);
    gl->DisableVertexAttribArray(self->attr_texcoord);

    if (gl->GenVertexArrays != NULL) {
        gl->BindVertexArray(0);
    } else {
        gl->BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        gl->BindBuffer(GL_ARRAY_BUFFER, 0);
    }
}

// Uploads one uniform value. Only the forms the shader chain emits are handled
// (ShaderChain.cpp uniforms_for): a scalar float and graphene vec2/vec3/vec4.
// Any other type is skipped - the chain never produces it.
static void texture_mix_apply_uniform(GstGLShader *shader, const gchar *name, const GValue *value)
{
    if (G_VALUE_HOLDS(value, G_TYPE_FLOAT)) {
        gst_gl_shader_set_uniform_1f(shader, name, g_value_get_float(value));
    } else if (G_VALUE_HOLDS(value, G_TYPE_DOUBLE)) {
        gst_gl_shader_set_uniform_1f(shader, name, (gfloat)g_value_get_double(value));
    } else if (G_VALUE_HOLDS(value, GRAPHENE_TYPE_VEC2)) {
        const graphene_vec2_t *v = (const graphene_vec2_t *)g_value_get_boxed(value);
        gst_gl_shader_set_uniform_2f(shader, name, graphene_vec2_get_x(v), graphene_vec2_get_y(v));
    } else if (G_VALUE_HOLDS(value, GRAPHENE_TYPE_VEC3)) {
        const graphene_vec3_t *v = (const graphene_vec3_t *)g_value_get_boxed(value);
        gst_gl_shader_set_uniform_3f(shader, name, graphene_vec3_get_x(v), graphene_vec3_get_y(v),
                                     graphene_vec3_get_z(v));
    } else if (G_VALUE_HOLDS(value, GRAPHENE_TYPE_VEC4)) {
        const graphene_vec4_t *v = (const graphene_vec4_t *)g_value_get_boxed(value);
        gst_gl_shader_set_uniform_4f(shader, name, graphene_vec4_get_x(v), graphene_vec4_get_y(v),
                                     graphene_vec4_get_z(v), graphene_vec4_get_w(v));
    }
}

static void texture_mix_apply_uniforms(TextureMix *self, GstGLShader *shader)
{
    if (self->uniforms == NULL) {
        return;
    }
    const gint count = gst_structure_n_fields(self->uniforms);
    for (gint i = 0; i < count; ++i) {
        const gchar *name = gst_structure_nth_field_name(self->uniforms, i);
        const GValue *value = gst_structure_get_value(self->uniforms, name);
        if (value != NULL) {
            texture_mix_apply_uniform(shader, name, value);
        }
    }
}

// One connected input: its sink index (from the sink_N pad name) and its
// current texture id.
struct TexSlot
{
    gint index;
    guint tex;
};

static gboolean texture_mix_render(gpointer data)
{
    TextureMix *self = (TextureMix *)data;
    GstGLContext *ctx = GST_GL_BASE_MIXER(self)->context;
    const GstGLFuncs *gl = ctx->gl_vtable;

    if (!texture_mix_ensure_shader(self) || !texture_mix_ensure_quad(self)
        || !texture_mix_ensure_sentinel(self)) {
        return FALSE;
    }

    // Collect every connected sink pad's input texture, keyed by its numeric
    // suffix so tex0/tex1/... map deterministically to sink_0/sink_1/...
    std::vector<TexSlot> slots;
    slots.reserve(TEXTURE_MIX_MAX_INPUTS);
    GstIterator *it = gst_element_iterate_sink_pads(GST_ELEMENT(self));
    gboolean done = FALSE;
    while (!done && slots.size() < TEXTURE_MIX_MAX_INPUTS) {
        GValue value = G_VALUE_INIT;
        switch (gst_iterator_next(it, &value)) {
        case GST_ITERATOR_OK: {
            GstPad *pad = (GstPad *)g_value_get_object(&value);
            TexSlot slot;
            slot.index = (gint)slots.size();
            slot.tex = GST_GL_MIXER_PAD(pad)->current_texture;
            const gchar *name = GST_PAD_NAME(pad);
            if (std::sscanf(name, "sink_%d", &slot.index) != 1) {
                slot.index = (gint)slots.size();
            }
            slots.push_back(slot);
            g_value_unset(&value);
            break;
        }
        case GST_ITERATOR_RESYNC:
            slots.clear();
            g_value_unset(&value);
            break;
        default:
            done = TRUE;
            break;
        }
    }
    gst_iterator_free(it);

    const gint count = (gint)slots.size();
    if (count < 2) {
        // A single input has nothing to combine; refuse rather than silently
        // rendering a partial result.
        if (!self->error_posted) {
            GST_ELEMENT_ERROR(self, CORE, FAILED,
                              ("texturemix needs at least two input textures, "
                               "got %d",
                               count),
                              (NULL));
            self->error_posted = TRUE;
        }
        return FALSE;
    }

    std::sort(slots.begin(), slots.end(),
              [](const TexSlot &a, const TexSlot &b) { return a.index < b.index; });

    gst_gl_shader_use(self->shader);

    // Bind each connected input to its own unit, with an explicit glUniform1i
    // per sampler so the silent-aliasing trap can never mask a failure.
    gint i;
    for (i = 0; i < count; ++i) {
        gchar name[16];
        gl->ActiveTexture(GL_TEXTURE0 + i);
        gl->BindTexture(GL_TEXTURE_2D, slots[i].tex);
        g_snprintf(name, sizeof(name), "tex%d", i);
        gst_gl_shader_set_uniform_1i(self->shader, name, i);
    }
    // Every unit beyond the connected inputs samples the sentinel, never an
    // aliased earlier input.
    for (; i < TEXTURE_MIX_MAX_INPUTS; ++i) {
        gchar name[16];
        gl->ActiveTexture(GL_TEXTURE0 + i);
        gl->BindTexture(GL_TEXTURE_2D, self->sentinel_texture);
        g_snprintf(name, sizeof(name), "tex%d", i);
        gst_gl_shader_set_uniform_1i(self->shader, name, i);
    }

    texture_mix_apply_uniforms(self, self->shader);

    texture_mix_draw_quad(self);

    return TRUE;
}

static void texture_mix_process_gl(GstGLContext *ctx, TextureMix *self)
{
    (void)ctx;
    GstGLMixer *mix = GST_GL_MIXER(self);
    GstGLFramebuffer *fbo = gst_gl_mixer_get_framebuffer(mix);

    self->gl_result =
            gst_gl_framebuffer_draw_to_texture(fbo, self->out_tex, texture_mix_render, self);
}

static gboolean texture_mix_process_textures(GstGLMixer *mix, GstGLMemory *out_tex)
{
    TextureMix *self = (TextureMix *)mix;
    GstGLContext *ctx = GST_GL_BASE_MIXER(mix)->context;

    self->out_tex = out_tex;
    gst_gl_context_thread_add(ctx, (GstGLContextThreadFunc)texture_mix_process_gl, self);
    return self->gl_result;
}

static void texture_mix_gl_stop(GstGLBaseMixer *mix)
{
    TextureMix *self = (TextureMix *)mix;
    GstGLContext *ctx = mix->context;
    const GstGLFuncs *gl = ctx->gl_vtable;

    if (self->shader != NULL) {
        gst_object_unref(self->shader);
        self->shader = NULL;
    }
    self->shader_compiled = FALSE;

    if (self->vao != 0 && gl->GenVertexArrays != NULL) {
        gl->DeleteVertexArrays(1, &self->vao);
        self->vao = 0;
    }
    if (self->vertex_buffer != 0) {
        gl->DeleteBuffers(1, &self->vertex_buffer);
        self->vertex_buffer = 0;
    }
    if (self->vbo_indices != 0) {
        gl->DeleteBuffers(1, &self->vbo_indices);
        self->vbo_indices = 0;
    }
    self->quad_ready = FALSE;

    if (self->sentinel_texture != 0) {
        gl->DeleteTextures(1, &self->sentinel_texture);
        self->sentinel_texture = 0;
    }
    self->sentinel_ready = FALSE;

    GST_GL_BASE_MIXER_CLASS(texture_mix_parent_class)->gl_stop(mix);
}

namespace genesis::adapters::engine::ges {

bool register_texture_mix()
{
    // Idempotent: gst_element_register returns true on a second call with the
    // same type, but the local once-guard keeps re-entry cheap and explicit.
    static const bool registered = [] {
        return gst_element_register(NULL, "texturemix", GST_RANK_NONE, texture_mix_get_type())
                != FALSE;
    }();
    return registered;
}

GstElement *make_texture_mix(const std::string &fragment, GstStructure *uniforms)
{
    if (!register_texture_mix()) {
        return nullptr;
    }
    GstElement *element = gst_element_factory_make("texturemix", nullptr);
    if (element == nullptr) {
        return nullptr;
    }
    g_object_set(element, "fragment", fragment.c_str(), nullptr);
    if (uniforms != nullptr) {
        g_object_set(element, "uniforms", uniforms, nullptr);
    }
    return element;
}

} // namespace genesis::adapters::engine::ges
