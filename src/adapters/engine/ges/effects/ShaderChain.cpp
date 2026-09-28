// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/effects/ShaderChain.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <graphene-gobject.h>
#include <gst/gst.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace genesis::adapters::engine::ges {

namespace {

// The three declarations every fragment shader must carry itself, because
// glshader injects none of them (docs/dependencies.md "Phase 0 spike result").
// Byte-for-byte the prelude the multipass spike compiled
// (spikes/gl-multipass/shaders.h GL_PRELUDE): glshader binds one texture and
// names it `tex`, so no other declaration may name a second sampler.
constexpr std::string_view kPrelude =
        "precision mediump float; varying vec2 v_texcoord; uniform sampler2D tex; ";

// The pass-through body the spike proved compiles: reads the one texture
// glshader binds and writes it unchanged. Emitted when the resolver leaves
// `source` empty, so a pass without a body is a visible identity rather than
// a shader that does not compile.
constexpr std::string_view kIdentityBody =
        "void main () { gl_FragColor = texture2D (tex, v_texcoord); }";

// The ES-1.00 prelude the lutgrade element compiles verbatim, validated to
// compile+link on the headless llvmpipe GLES2 context (see the probe notes).
// The `#extension` must precede the precisions, and `precision mediump
// sampler3D;` is required - GLSL ES 1.00 has no default precision for a 3D
// sampler, and the driver errors without it. glslstage prepends nothing on a
// GLES2 context with NONE version/profile, so the driver defaults to ES 1.00
// and `texture3D`/`texture2D` are the only texture lookups available.
constexpr std::string_view kLutPrelude = "#extension GL_OES_texture_3D : enable\n"
                                         "precision mediump float;\n"
                                         "precision mediump sampler3D;\n"
                                         "varying vec2 v_texcoord;\n"
                                         "uniform sampler2D tex;\n"
                                         "uniform sampler3D lut3d;\n";

// The fixed body a LUT pass runs: sample the table at the picture's RGB and
// mix the grade into the picture by `strength / 100.0`, preserving alpha.
// Byte-for-byte parity with the pack's GLSL fallback (effect.glsl), which
// mixes `strength / 100.0` too; the `lut` enum knob is not read here because
// the table the element binds was already chosen by the resolver.
constexpr std::string_view kLutBody =
        "void main () {\n"
        "    vec4 c = texture2D (tex, v_texcoord);\n"
        "    vec3 graded = texture3D (lut3d, c.rgb).rgb;\n"
        "    gl_FragColor = vec4 (mix (c.rgb, graded, strength / 100.0), c.a);\n"
        "}\n";

// Whether a body declares a sampler of its own. The prelude already declares
// the one texture glshader binds; a body naming `sampler2D` again is a second
// texture unit, which glshader silently aliases to unit 0
// (docs/decisions/effect-execution.md §6) - the pass reads its input again
// and no error is raised. Refused rather than emitted.
bool declares_sampler(std::string_view body)
{
    return body.find("sampler2D") != std::string_view::npos;
}

// Whether glshader's `uniforms` structure can carry a field's value. The
// structure is read by gstglfiltershader's `_set_uniform`, which understands a
// scalar float and graphene vec2/vec3/vec4 - but no array, so a curve's
// `vec4[8]` field cannot be uploaded. Refusing it keeps the declarations and
// the upload in agreement; uploading nothing for a declared uniform would
// leave the curve silently at its default.
bool field_is_uploadable(const genesis::core::ParamField &field)
{
    return field.type == "float" || field.type == "vec2" || field.type == "vec3"
            || field.type == "vec4";
}

// The float at `index` within a field's bytes: native little-endian f32s, the
// layout `fields` and `params` were produced together
// (src/effects/Resolve.cpp `params_block`).
float read_float(const std::vector<std::uint8_t> &params, std::uint32_t offset, std::uint32_t index)
{
    float value = 0.0F;
    std::memcpy(&value, params.data() + offset + (index * sizeof(float)), sizeof(value));
    return value;
}

} // namespace

GstStructure *uniforms_for(const genesis::core::ShaderPass &pass)
{
    GstStructure *structure = gst_structure_new_empty("uniforms");
    for (const genesis::core::ParamField &field : pass.fields) {
        const auto at = [&](std::uint32_t index) {
            return read_float(pass.params, field.offset, index);
        };
        if (field.type == "float") {
            gst_structure_set(structure, field.name.c_str(), G_TYPE_FLOAT,
                              static_cast<gfloat>(at(0)), nullptr);
        } else if (field.type == "vec2") {
            graphene_vec2_t value;
            graphene_vec2_init(&value, at(0), at(1));
            gst_structure_set(structure, field.name.c_str(), GRAPHENE_TYPE_VEC2, &value, nullptr);
        } else if (field.type == "vec3") {
            graphene_vec3_t value;
            graphene_vec3_init(&value, at(0), at(1), at(2));
            gst_structure_set(structure, field.name.c_str(), GRAPHENE_TYPE_VEC3, &value, nullptr);
        } else {
            graphene_vec4_t value;
            graphene_vec4_init(&value, at(0), at(1), at(2), at(3));
            gst_structure_set(structure, field.name.c_str(), GRAPHENE_TYPE_VEC4, &value, nullptr);
        }
    }
    return structure;
}

std::optional<std::string> fragment_source_for(const genesis::core::ShaderPass &pass)
{
    const bool has_body = pass.source && !pass.source->empty();
    const std::string body = has_body ? *pass.source : std::string(kIdentityBody);

    if (declares_sampler(body)) {
        return std::nullopt;
    }

    std::string source;
    source.reserve(kPrelude.size() + body.size() + (pass.fields.size() * 32) + 64);
    source += kPrelude;

    // The pass's uniform block: one declaration per field, from the field's
    // GLSL type spelling and name, so the declared uniforms match the values
    // append_shader_chain uploads from the same `fields`.
    for (const genesis::core::ParamField &field : pass.fields) {
        source += "uniform ";
        source += field.type;
        source.push_back(' ');
        source += field.name;
        source += ";\n";
    }

    if (!has_body) {
        // The resolver leaves `source` empty for the engine to fill; without a
        // body the pass is a documented identity, so a pass with no body is
        // visible rather than silently wrong.
        source += "// identity: the resolver left the source empty\n";
    }
    source += body;
    source.push_back('\n');
    return source;
}

std::optional<std::string> fragment_source_lut_for(const genesis::core::ShaderPass &pass)
{
    // A LUT pass still runs one fragment: it cannot carry pre-stages (the
    // named-intermediate model) or a field the `uniforms` structure cannot
    // upload, for the same reasons the glshader path refuses them.
    if (!pass.stages.empty()) {
        return std::nullopt;
    }
    for (const genesis::core::ParamField &field : pass.fields) {
        if (!field_is_uploadable(field)) {
            return std::nullopt;
        }
    }
    // The body mixes by `strength`, the declared intensity knob the shipped
    // LUT pack names; without the field the body's reference is undeclared.
    const bool has_strength = std::any_of(
            pass.fields.begin(), pass.fields.end(),
            [](const genesis::core::ParamField &field) { return field.name == "strength"; });
    if (!has_strength) {
        return std::nullopt;
    }

    std::string source;
    source.reserve(kLutPrelude.size() + kLutBody.size() + (pass.fields.size() * 32) + 64);
    source += kLutPrelude;

    // The pass's uniform block, one declaration per field - the same loop the
    // glshader path emits, so the declared uniforms match the values the
    // caller uploads from the same `fields`. The `lut` enum field lands here
    // too, declared but unread: the table the element binds was chosen by the
    // resolver, and the driver optimizes the unused uniform out.
    for (const genesis::core::ParamField &field : pass.fields) {
        source += "uniform ";
        source += field.type;
        source.push_back(' ');
        source += field.name;
        source += ";\n";
    }

    source += kLutBody;
    return source;
}

bool pass_is_expressible(const genesis::core::ShaderPass &pass)
{
    // The same three refusals append_shader_chain applies, before any element
    // is created, so the GES builder can decline a treatment identically. The
    // only branch is which element runs: a pass with a resolved table takes
    // the lutgrade path, every other pass stays on glshader.
    if (!pass.stages.empty()) {
        return false;
    }
    for (const genesis::core::ParamField &field : pass.fields) {
        if (!field_is_uploadable(field)) {
            return false;
        }
    }
    return pass.lut.has_value() ? fragment_source_lut_for(pass).has_value()
                                : fragment_source_for(pass).has_value();
}

GstElement *append_shader_chain(GstElement *tail,
                                const std::vector<genesis::core::ShaderPass> &passes)
{
    // Refuse up front, before any element is created: a partially built chain
    // would render something that is not the effect. A pass that declares
    // pre-stages (the named-intermediate model), whose source is refused (a
    // second sampler), or whose uniform block glshader cannot upload (a
    // curve's vec4[8]) aborts the whole chain.
    std::vector<std::string> sources;
    sources.reserve(passes.size());
    for (const genesis::core::ShaderPass &pass : passes) {
        if (!pass.stages.empty()) {
            return nullptr;
        }
        for (const genesis::core::ParamField &field : pass.fields) {
            if (!field_is_uploadable(field)) {
                return nullptr;
            }
        }
        std::optional<std::string> source = fragment_source_for(pass);
        if (!source.has_value()) {
            return nullptr;
        }
        sources.push_back(std::move(*source));
    }

    if (passes.empty()) {
        return tail;
    }
    if (tail == nullptr) {
        return nullptr;
    }

    // The glshaders must share `tail`'s bin for gst_element_link to accept the
    // link; a tail outside a bin has nowhere for the chain to hang.
    GstObject *parent = gst_element_get_parent(tail);
    if (parent == nullptr || !GST_IS_BIN(parent)) {
        gst_clear_object(&parent);
        return nullptr;
    }
    GstBin *bin = GST_BIN(parent);

    GstElement *current = tail;
    std::vector<GstElement *> added;
    added.reserve(passes.size());
    for (std::size_t i = 0; i < passes.size(); ++i) {
        GstElement *shader = gst_element_factory_make("glshader", nullptr);
        if (shader == nullptr) {
            break;
        }
        g_object_set(shader, "fragment", sources[i].c_str(), nullptr);
        GstStructure *uniforms = uniforms_for(passes[i]);
        g_object_set(shader, "uniforms", uniforms, nullptr);
        gst_structure_free(uniforms);

        if (!gst_bin_add(bin, shader)) {
            gst_object_unref(shader);
            break;
        }
        added.push_back(shader);

        if (!gst_element_link(current, shader)) {
            break;
        }
        current = shader;
    }

    // If any pass failed to build or link, undo every element this call added
    // and return nullptr: the caller's tail is untouched and the bin is left
    // as it was.
    if (added.size() != passes.size()) {
        for (GstElement *element : added) {
            gst_object_ref(element);
            gst_bin_remove(bin, element);
            gst_object_unref(element);
        }
        gst_object_unref(parent);
        return nullptr;
    }

    gst_object_unref(parent);
    return current;
}

bool update_uniforms(TreatmentChain &chain, genesis::core::Rational time)
{
    // The re-resolve allocates a pass vector (and reads the pack body) per
    // call - the documented cost of riding keyed knobs. The comparison below
    // is allocation-free, so a frame whose knobs have not moved uploads
    // nothing and only pays that re-resolve.
    std::vector<genesis::core::ShaderPass> next = chain.treatment.passes_at(time);
    if (next == chain.last) {
        return false;
    }

    // A pass-count change between the chain built at build time and this
    // resolution is a structural change a probe cannot rebuild; only the
    // shared prefix maps to a glshader, so the rest is skipped.
    const std::size_t count = std::min(next.size(), chain.shaders.size());
    for (std::size_t i = 0; i < count; ++i) {
        GstStructure *uniforms = uniforms_for(next[i]);
        g_object_set(chain.shaders[i], "uniforms", uniforms, nullptr);
        gst_structure_free(uniforms);
    }
    chain.last = std::move(next);
    return true;
}

namespace {

// The probe glshader runs from: each buffer's PTS is the absolute timeline
// time the treatment's `passes_at` normalises against its own span, so no
// clip-local subtraction is needed - the same absolute-time seam `to_ns` writes
// on the way in. Runs on the streaming thread; it only re-resolves the
// treatment it owns and sets the glshader it already holds, touching nothing
// the UI owns.
GstPadProbeReturn treatment_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad;
    auto *chain = static_cast<TreatmentChain *>(user_data);
    if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer == nullptr || !GST_BUFFER_PTS_IS_VALID(buffer)) {
        return GST_PAD_PROBE_OK;
    }
    // The PTS is a guint64; timelines fit in int64 nanoseconds, so the cast is
    // the same one `to_ns` does in reverse and never overflows in practice.
    const genesis::core::Rational time(static_cast<std::int64_t>(GST_BUFFER_PTS(buffer)),
                                       GST_SECOND);
    update_uniforms(*chain, time);
    return GST_PAD_PROBE_OK;
}

} // namespace

bool attach_treatment_probe(GstElement *shader, TreatmentChain *chain, void (*destroy)(void *))
{
    if (shader == nullptr || chain == nullptr) {
        return false;
    }
    GstPad *pad = gst_element_get_static_pad(shader, "sink");
    if (pad == nullptr) {
        return false;
    }
    const gulong id = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, treatment_buffer_probe,
                                        chain, destroy);
    gst_object_unref(pad);
    return id != 0;
}

} // namespace genesis::adapters::engine::ges
