// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Uploads a resolved uniform structure to a GL shader. Only the forms the
// shader chain emits are handled (ShaderChain.cpp uniforms_for): a scalar float
// and graphene vec2/vec3/vec4; any other type is skipped because the chain
// never produces it. Shared by the effect elements that consume that structure.

#pragma once

#include <graphene-gobject.h>
#include <gst/gst.h>
#include <gst/gl/gl.h>

namespace genesis::adapters::engine::ges {

inline void apply_uniform(GstGLShader *shader, const gchar *name, const GValue *value)
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

inline void apply_uniforms(GstGLShader *shader, GstStructure *uniforms)
{
    if (uniforms == nullptr) {
        return;
    }
    const gint count = gst_structure_n_fields(uniforms);
    for (gint index = 0; index < count; ++index) {
        const gchar *name = gst_structure_nth_field_name(uniforms, index);
        const GValue *value = gst_structure_get_value(uniforms, name);
        if (value != nullptr) {
            apply_uniform(shader, name, value);
        }
    }
}

} // namespace genesis::adapters::engine::ges
