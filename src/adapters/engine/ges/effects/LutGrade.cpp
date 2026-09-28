// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/effects/LutGrade.h"

#include <cstdio>

#include <graphene-gobject.h>
#include <gst/gst.h>
#include <gst/gl/gstglfilter.h>
#include <gst/gl/gstglformat.h>
#include <gst/gl/gstglfuncs.h>
#include <gst/gl/gstglmemory.h>
#include <gst/gl/gstglshader.h>
#include <gst/gl/gstglslstage.h>

// The element itself lives at file scope: G_DEFINE_TYPE generates the
// get_type/class-init machinery at global scope, so the subclass cannot sit
// inside the namespace. The public engine-neutral surface (register_lut_grade,
// make_lut_grade) wraps it at the bottom.
//
// Why GstGLFilter, not GstGLMixer: a colour-look pass has exactly one input -
// the picture - and one extra table to sample, so a single-input filter is the
// right shape. The second sampler (`lut3d`) is not a second input pad; it is a
// 3D texture the element uploads itself from `lut-data` and binds to unit 1
// with an explicit glUniform1i, so it can never silently alias the picture's
// unit 0 the way a second glshader sampler2D would
// (docs/decisions/effect-execution.md §6).

GST_DEBUG_CATEGORY_STATIC(lut_grade_debug);
#define GST_CAT_DEFAULT lut_grade_debug

typedef struct _LutGrade LutGrade;
typedef struct _LutGradeClass LutGradeClass;

struct _LutGrade
{
    GstGLFilter parent;

    gchar *fragment;
    GstStructure *uniforms;

    guint lut_size;
    GBytes *lut_data;

    // GL resources, created lazily on the GL thread (the render callback).
    GstGLShader *shader;
    gboolean shader_compiled;

    guint lut_texture;
    gboolean lut_uploaded;

    // Per-render scratch, valid for one filter_texture call.
    gboolean gl_result;
    gboolean error_posted;
};

struct _LutGradeClass
{
    GstGLFilterClass parent_class;
};

G_DEFINE_TYPE(LutGrade, lut_grade, GST_TYPE_GL_FILTER);

enum { PROP_0, PROP_FRAGMENT, PROP_UNIFORMS, PROP_LUT_SIZE, PROP_LUT_DATA };

static void lut_grade_set_property(GObject *object, guint prop_id, const GValue *value,
                                   GParamSpec *pspec);
static void lut_grade_get_property(GObject *object, guint prop_id, GValue *value,
                                   GParamSpec *pspec);
static gboolean lut_grade_filter_texture(GstGLFilter *filter, GstGLMemory *in_tex,
                                         GstGLMemory *out_tex);
static void lut_grade_gl_stop(GstGLBaseFilter *filter);
static void lut_grade_finalize(GObject *object);

static void lut_grade_class_init(LutGradeClass *klass)
{
    GObjectClass *gobject_class = (GObjectClass *)klass;
    GstElementClass *element_class = (GstElementClass *)klass;
    GstGLFilterClass *filter_class = (GstGLFilterClass *)klass;
    GstGLBaseFilterClass *base_class = (GstGLBaseFilterClass *)klass;

    GST_DEBUG_CATEGORY_INIT(lut_grade_debug, "lutgrade", 0,
                            "picture + 3D LUT single-shader GL filter");

    // The same RGBA GLMemory pad templates glshader installs: the element sits
    // in the glupload ! glcolorconvert ! ... ! gldownload sandwich exactly like
    // glshader does.
    gst_gl_filter_add_rgba_pad_templates(filter_class);

    gobject_class->set_property = lut_grade_set_property;
    gobject_class->get_property = lut_grade_get_property;
    gobject_class->finalize = lut_grade_finalize;

    g_object_class_install_property(
            gobject_class, PROP_FRAGMENT,
            g_param_spec_string(
                    "fragment", "Fragment", "GLSL fragment source", NULL,
                    static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(
            gobject_class, PROP_UNIFORMS,
            g_param_spec_boxed(
                    "uniforms", "Uniforms", "Uniforms for the shader", GST_TYPE_STRUCTURE,
                    static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(
            gobject_class, PROP_LUT_SIZE,
            g_param_spec_uint(
                    "lut-size", "LUT size", "The 3D LUT's edge length", 0, G_MAXUINT32, 0,
                    static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(
            gobject_class, PROP_LUT_DATA,
            g_param_spec_boxed(
                    "lut-data", "LUT data", "size^3 x 4 float RGBA table", G_TYPE_BYTES,
                    static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    filter_class->filter_texture = lut_grade_filter_texture;
    base_class->gl_stop = lut_grade_gl_stop;

    // Same surface as glshader: desktop GL and GLES2/GLES3. The 3D texture is
    // guarded at run time on gl->TexImage3D (GLES2 needs OES_texture_3D).
    base_class->supported_gl_api =
            static_cast<GstGLAPI>(GST_GL_API_OPENGL | GST_GL_API_GLES2 | GST_GL_API_OPENGL3);

    gst_element_class_set_static_metadata(
            element_class, "Picture + 3D LUT single-shader GL filter", "Filter/Effect",
            "Samples one 2D texture and one 3D LUT in one shader", "Genesis-0 contributors");
}

static void lut_grade_init(LutGrade *self)
{
    self->lut_size = 0;
    self->shader_compiled = FALSE;
    self->lut_uploaded = FALSE;
}

static void lut_grade_set_property(GObject *object, guint prop_id, const GValue *value,
                                   GParamSpec *pspec)
{
    LutGrade *self = (LutGrade *)object;

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
    case PROP_LUT_SIZE:
        self->lut_size = g_value_get_uint(value);
        // A size change invalidates the uploaded texture; the next render
        // re-uploads from the (new) data.
        self->lut_uploaded = FALSE;
        break;
    case PROP_LUT_DATA: {
        GBytes *bytes = (GBytes *)g_value_dup_boxed(value);
        g_clear_pointer(&self->lut_data, g_bytes_unref);
        self->lut_data = bytes;
        self->lut_uploaded = FALSE;
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void lut_grade_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    LutGrade *self = (LutGrade *)object;

    switch (prop_id) {
    case PROP_FRAGMENT:
        g_value_set_string(value, self->fragment);
        break;
    case PROP_UNIFORMS:
        g_value_set_boxed(value, self->uniforms);
        break;
    case PROP_LUT_SIZE:
        g_value_set_uint(value, self->lut_size);
        break;
    case PROP_LUT_DATA:
        g_value_set_boxed(value, self->lut_data);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void lut_grade_finalize(GObject *object)
{
    LutGrade *self = (LutGrade *)object;

    g_free(self->fragment);
    self->fragment = NULL;
    g_clear_pointer(&self->uniforms, gst_structure_free);
    g_clear_pointer(&self->lut_data, g_bytes_unref);

    G_OBJECT_CLASS(lut_grade_parent_class)->finalize(object);
}

// Compiles the full fragment (the source is self-contained: the prelude with
// the samplers is already in `fragment`, exactly like glshader's convention)
// against the canonical default vertex stage, and links them. Runs on the GL
// thread, inside the render callback.
static gboolean lut_grade_ensure_shader(LutGrade *self)
{
    GstGLContext *ctx = GST_GL_BASE_FILTER(self)->context;
    GstGLShader *shader;
    GstGLSLStage *stage;
    GError *error = NULL;
    GstGLFilter *filter = GST_GL_FILTER(self);

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

    // The vertex stage GstGLFilter's fullscreen quad expects, byte-for-byte
    // gst_gl_shader_string_vertex_default: its a_position/a_texcoord attribute
    // locations are what draw_fullscreen_quad uses.
    stage = gst_glsl_stage_new_default_vertex(ctx);
    if (stage == NULL || !gst_gl_shader_compile_attach_stage(shader, stage, &error)) {
        if (stage != NULL) {
            gst_object_unref(stage);
        }
        g_clear_error(&error);
        goto fail;
    }
    // Ownership trap: compile_attach_stage takes the stage reference on
    // success, so unrefing here would be a double-unref.

    stage = gst_glsl_stage_new_with_string(ctx, GL_FRAGMENT_SHADER, GST_GLSL_VERSION_NONE,
                                           GST_GLSL_PROFILE_NONE, self->fragment);
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

    // The glshader element sets these directly rather than via default_shader,
    // leaving default_shader NULL so gst_gl_filter_get_attributes early-returns
    // and the locations we set here persist (gstglfiltershader.c pattern).
    filter->draw_attr_position_loc = gst_gl_shader_get_attribute_location(shader, "a_position");
    filter->draw_attr_texture_loc = gst_gl_shader_get_attribute_location(shader, "a_texcoord");
    return TRUE;

fail:
    GST_ERROR_OBJECT(self, "shader compile failed: %s", error != NULL ? error->message : "unknown");
    g_clear_error(&error);
    gst_object_unref(shader);
    return FALSE;
}

// Uploads the table as a GL_TEXTURE_3D, once. The caller (make_lut_grade)
// already validated size and byte count; here the GL-side capability is
// checked and the texture built. Linear filtering + clamp-to-edge matches
// core::Lut::sample's trilinear interpolation over the [0,1] domain.
static gboolean lut_grade_ensure_lut(LutGrade *self)
{
    GstGLContext *ctx = GST_GL_BASE_FILTER(self)->context;
    const GstGLFuncs *gl = ctx->gl_vtable;

    if (self->lut_uploaded && self->lut_texture != 0) {
        return TRUE;
    }
    if (self->lut_data == NULL) {
        return FALSE;
    }
    // GLES2 without OES_texture_3D leaves TexImage3D NULL; refuse rather than
    // crash or render garbage. Desktop GL and GLES3 always have it.
    if (gl->TexImage3D == NULL) {
        if (!self->error_posted) {
            GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
                              ("3D textures are not supported by this GL "
                               "context; the LUT cannot be sampled"),
                              (NULL));
            self->error_posted = TRUE;
        }
        return FALSE;
    }

    const guint size = self->lut_size;
    gsize data_size = 0;
    gconstpointer data = g_bytes_get_data(self->lut_data, &data_size);
    if (size < 2 || data == NULL
        || data_size < static_cast<gsize>(size) * size * size * 4 * sizeof(float)) {
        return FALSE;
    }

    gl->GenTextures(1, &self->lut_texture);
    gl->BindTexture(GL_TEXTURE_3D, self->lut_texture);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    gl->TexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, size, size, size, 0, GL_RGBA, GL_FLOAT, data);
    gl->BindTexture(GL_TEXTURE_3D, 0);

    self->lut_uploaded = TRUE;
    return TRUE;
}

// Uploads one uniform value. Only the forms the shader chain emits are handled
// (ShaderChain.cpp uniforms_for): a scalar float and graphene vec2/vec3/vec4 -
// the same set TextureMix uploads. Any other type is skipped.
static void lut_grade_apply_uniform(GstGLShader *shader, const gchar *name, const GValue *value)
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

static void lut_grade_apply_uniforms(LutGrade *self, GstGLShader *shader)
{
    if (self->uniforms == NULL) {
        return;
    }
    const gint count = gst_structure_n_fields(self->uniforms);
    for (gint i = 0; i < count; ++i) {
        const gchar *name = gst_structure_nth_field_name(self->uniforms, i);
        const GValue *value = gst_structure_get_value(self->uniforms, name);
        if (value != NULL) {
            lut_grade_apply_uniform(shader, name, value);
        }
    }
}

// The render callback gst_gl_filter_render_to_target invokes inline on the GL
// thread with the FBO bound to `out_tex`. Binds the picture to unit 0 and the
// LUT to unit 1, applies the uniforms, and draws the fullscreen quad. The
// render func receives `in_tex` from the FBO helper, so no scratch pass-through
// is needed.
static gboolean lut_grade_render(GstGLFilter *filter, GstGLMemory *in_tex, gpointer user_data)
{
    LutGrade *self = (LutGrade *)user_data;
    GstGLContext *ctx = GST_GL_BASE_FILTER(self)->context;
    const GstGLFuncs *gl = ctx->gl_vtable;

    if (!lut_grade_ensure_shader(self) || !lut_grade_ensure_lut(self)) {
        return FALSE;
    }

    gst_gl_shader_use(self->shader);

    // The picture on unit 0, the table on unit 1, each named with an explicit
    // glUniform1i so the samplers can never silently alias.
    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(gst_gl_texture_target_to_gl(in_tex->tex_target),
                    gst_gl_memory_get_texture_id(in_tex));
    gst_gl_shader_set_uniform_1i(self->shader, "tex", 0);

    gl->ActiveTexture(GL_TEXTURE1);
    gl->BindTexture(GL_TEXTURE_3D, self->lut_texture);
    gst_gl_shader_set_uniform_1i(self->shader, "lut3d", 1);

    lut_grade_apply_uniforms(self, self->shader);

    gst_gl_filter_draw_fullscreen_quad(filter);

    return TRUE;
}

// The filter_texture vfunc: gst_gl_filter_transform dispatches it on the GL
// thread, and render_to_target draws into `out_tex` through the FBO.
static gboolean lut_grade_filter_texture(GstGLFilter *filter, GstGLMemory *in_tex,
                                         GstGLMemory *out_tex)
{
    LutGrade *self = (LutGrade *)filter;
    return gst_gl_filter_render_to_target(filter, in_tex, out_tex, lut_grade_render, self);
}

static void lut_grade_gl_stop(GstGLBaseFilter *filter)
{
    LutGrade *self = (LutGrade *)filter;
    GstGLContext *ctx = filter->context;

    if (self->shader != NULL) {
        gst_object_unref(self->shader);
        self->shader = NULL;
    }
    self->shader_compiled = FALSE;

    if (ctx != NULL && self->lut_texture != 0) {
        ctx->gl_vtable->DeleteTextures(1, &self->lut_texture);
        self->lut_texture = 0;
    }
    self->lut_uploaded = FALSE;

    GST_GL_BASE_FILTER_CLASS(lut_grade_parent_class)->gl_stop(filter);
}

namespace genesis::adapters::engine::ges {

bool register_lut_grade()
{
    // Idempotent: gst_element_register returns true on a second call with the
    // same type, but the local once-guard keeps re-entry cheap and explicit.
    static const bool registered = [] {
        return gst_element_register(NULL, "lutgrade", GST_RANK_NONE, lut_grade_get_type()) != FALSE;
    }();
    return registered;
}

GstElement *make_lut_grade(const std::string &fragment, GstStructure *uniforms,
                           const genesis::core::Lut &lut)
{
    // Refuse a table whose data does not match its declared size up front; a
    // mismatched upload would read past the table or leave texels unset. The
    // resolver would never hand one back, so this is a last line of defense
    // for a hand-built call site.
    const std::size_t size = lut.size;
    if (size < 2 || !lut.rgba || lut.rgba->size() != size * size * size * 4) {
        return nullptr;
    }
    if (!register_lut_grade()) {
        return nullptr;
    }
    GstElement *element = gst_element_factory_make("lutgrade", nullptr);
    if (element == nullptr) {
        return nullptr;
    }
    g_object_set(element, "fragment", fragment.c_str(), nullptr);
    if (uniforms != nullptr) {
        g_object_set(element, "uniforms", uniforms, nullptr);
    }
    g_object_set(element, "lut-size", static_cast<guint>(size), nullptr);
    // g_bytes_new copies; the property's setter dups it, so both references are
    // ours to release here.
    GBytes *data = g_bytes_new(lut.rgba->data(), size * size * size * 4 * sizeof(float));
    g_object_set(element, "lut-data", data, nullptr);
    g_bytes_unref(data);
    return element;
}

} // namespace genesis::adapters::engine::ges
