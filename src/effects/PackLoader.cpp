// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "effects/PackLoader.h"

#include <toml++/toml.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/TomlRead.h"

namespace genesis::effects {

namespace {

// Accumulates a Pack and the problems found while reading it. A problem
// recorded anywhere leaves the Pack unusable; the loader keeps reading so a
// human sees every problem at once rather than only the first.
struct Builder : core::TomlErrorSink
{
    Pack pack;
    std::vector<LoadError> problems;

    void fail(std::string where, std::string what) override
    {
        problems.push_back(LoadError{ std::move(where), std::move(what) });
    }
};

// Prepends a dotted prefix when non-empty, so scoped and standalone forms
// share the same table reader bodies with different error-path prefixes.
std::string scoped(std::string_view prefix, std::string_view tail)
{
    if (prefix.empty()) {
        return std::string(tail);
    }
    std::string out;
    out.reserve(prefix.size() + 1 + tail.size());
    out.append(prefix);
    out.push_back('.');
    out.append(tail);
    return out;
}

// --- table readers ------------------------------------------------------

void read_params(const toml::table &root, std::string_view prefix, Builder &builder)
{
    const toml::node *params_node = root.get("param");
    if (params_node == nullptr) {
        return;
    }
    const toml::array *params = params_node->as_array();
    if (params == nullptr) {
        builder.fail(core::where_at(scoped(prefix, "param"), *params_node),
                     "`param` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *params) {
        const std::string path = scoped(prefix, "param[" + std::to_string(index) + "]");
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(core::where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        Parameter parameter;
        core::require_string(*table, "key", path + ".key", parameter.key, builder);
        core::require_string(*table, "label", path + ".label", parameter.label, builder);
        core::read_string(*table, "group", path + ".group", parameter.group, builder);
        core::read_string(*table, "unit", path + ".unit", parameter.unit, builder);

        const toml::node *type_node = table->get("type");
        if (type_node != nullptr) {
            const auto type = type_node->value<std::string>();
            if (!type) {
                builder.fail(core::where_at(path + ".type", *type_node), "`type` must be a string");
            } else if (const auto mapped = from_name(*type)) {
                parameter.type = *mapped;
            } else {
                builder.fail(core::where_at(path + ".type", *type_node),
                             "unknown parameter type `" + *type + "`");
            }
        }

        core::read_double(*table, "min", path + ".min", parameter.min, builder);
        core::read_double(*table, "max", path + ".max", parameter.max, builder);
        core::read_double(*table, "default", path + ".default", parameter.default_value, builder);
        core::read_double(*table, "step", path + ".step", parameter.step, builder);
        core::read_bool(*table, "animate", path + ".animate", parameter.animate, builder);

        const toml::node *values_node = table->get("values");
        if (values_node != nullptr) {
            const toml::array *values = values_node->as_array();
            if (values == nullptr) {
                builder.fail(core::where_at(path + ".values", *values_node),
                             "`values` must be an array");
            } else {
                std::size_t i = 0;
                for (const toml::node &value : *values) {
                    const auto number = value.value<double>();
                    if (!number) {
                        builder.fail(
                                core::where_at(path + ".values[" + std::to_string(i) + "]", value),
                                "must be a number");
                    } else {
                        parameter.values.push_back(*number);
                    }
                    ++i;
                }
            }
        }

        const toml::node *labels_node = table->get("labels");
        if (labels_node != nullptr) {
            const toml::array *labels = labels_node->as_array();
            if (labels == nullptr) {
                builder.fail(core::where_at(path + ".labels", *labels_node),
                             "`labels` must be an array");
            } else {
                std::size_t i = 0;
                for (const toml::node &label : *labels) {
                    const auto text = label.value<std::string>();
                    if (!text) {
                        builder.fail(
                                core::where_at(path + ".labels[" + std::to_string(i) + "]", label),
                                "must be a string");
                    } else {
                        parameter.labels.push_back(*text);
                    }
                    ++i;
                }
            }
        }

        builder.pack.parameters.push_back(std::move(parameter));
        ++index;
    }
}

void read_uniforms(const toml::table &root, std::string_view prefix, Builder &builder)
{
    const toml::node *uniforms_node = root.get("uniform");
    if (uniforms_node == nullptr) {
        return;
    }
    const toml::array *uniforms = uniforms_node->as_array();
    if (uniforms == nullptr) {
        builder.fail(core::where_at(scoped(prefix, "uniform"), *uniforms_node),
                     "`uniform` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *uniforms) {
        const std::string path = scoped(prefix, "uniform[" + std::to_string(index) + "]");
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(core::where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        Uniform uniform;
        core::require_string(*table, "key", path + ".key", uniform.key, builder);
        core::require_string(*table, "expr", path + ".expr", uniform.expr, builder);
        builder.pack.uniforms.push_back(std::move(uniform));
        ++index;
    }
}

void read_passes(const toml::table &root, std::string_view prefix, Builder &builder)
{
    const toml::node *wgsl_node = root.get("wgsl");
    if (wgsl_node == nullptr) {
        return;
    }
    // The shader body named by `[wgsl].entry` (effect.wgsl / GLSL) is not this
    // loader's business: the manifest is declarative data, and the execution
    // path resolves the body later (docs/decisions/effect-execution.md §3).
    // `entry` and `space` are therefore left unread here; only the passes the
    // schema carries are mapped.
    const toml::table *wgsl = wgsl_node->as_table();
    if (wgsl == nullptr) {
        builder.fail(core::where_at(scoped(prefix, "wgsl"), *wgsl_node), "[wgsl] must be a table");
        return;
    }
    const toml::node *passes_node = wgsl->get("pass");
    if (passes_node == nullptr) {
        return;
    }
    const toml::array *passes = passes_node->as_array();
    if (passes == nullptr) {
        builder.fail(core::where_at(scoped(prefix, "wgsl.pass"), *passes_node),
                     "`pass` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *passes) {
        const std::string path = scoped(prefix, "wgsl.pass[" + std::to_string(index) + "]");
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(core::where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        Pass pass;
        core::require_string(*table, "target", path + ".target", pass.target, builder);

        const toml::node *shrink_node = table->get("shrink");
        if (shrink_node != nullptr) {
            const toml::array *shrink = shrink_node->as_array();
            if (shrink == nullptr || shrink->size() != 2) {
                builder.fail(core::where_at(path + ".shrink", *shrink_node),
                             "`shrink` must be two expressions: across and down");
            } else {
                std::array<std::string, 2> pair{ };
                bool ok = true;
                for (std::size_t i = 0; i < 2; ++i) {
                    const toml::node &expression = (*shrink)[i];
                    const auto text = expression.value<std::string>();
                    if (!text) {
                        builder.fail(core::where_at(path + ".shrink[" + std::to_string(i) + "]",
                                                    expression),
                                     "must be a string expression");
                        ok = false;
                    } else {
                        pair[i] = *text;
                    }
                }
                if (ok) {
                    pass.shrink = pair;
                }
            }
        }

        builder.pack.passes.push_back(std::move(pass));
        ++index;
    }
}

void read_transition(const toml::table &root, std::string_view prefix, Builder &builder)
{
    const toml::node *transition_node = root.get("transition");
    if (transition_node == nullptr) {
        return;
    }
    const toml::table *transition = transition_node->as_table();
    if (transition == nullptr) {
        builder.fail(core::where_at(scoped(prefix, "transition"), *transition_node),
                     "[transition] must be a table");
        return;
    }

    TransitionForm form;
    core::require_string(*transition, "entry", scoped(prefix, "transition.entry"), form.entry,
                         builder);
    core::read_optional_string(*transition, "xfade", scoped(prefix, "transition.xfade"), form.xfade,
                               builder);
    builder.pack.transition = std::move(form);
}

// The CPU fallback table: `[cpu] element = "..."` plus one `[[cpu.param]]` per
// element property, each a `key` (the property) and an `expr` over the pack's
// parameters - the same array-of-tables shape as `[[uniform]]`, so the
// manifest reads the same way.
void read_cpu(const toml::table &root, std::string_view prefix, Builder &builder)
{
    const toml::node *cpu_node = root.get("cpu");
    if (cpu_node == nullptr) {
        return;
    }
    const toml::table *cpu = cpu_node->as_table();
    if (cpu == nullptr) {
        builder.fail(core::where_at(scoped(prefix, "cpu"), *cpu_node), "[cpu] must be a table");
        return;
    }

    CpuFallback fallback;
    core::require_string(*cpu, "element", scoped(prefix, "cpu.element"), fallback.element, builder);

    const toml::node *params_node = cpu->get("param");
    if (params_node == nullptr) {
        return;
    }
    const toml::array *params = params_node->as_array();
    if (params == nullptr) {
        builder.fail(core::where_at(scoped(prefix, "cpu.param"), *params_node),
                     "`param` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *params) {
        const std::string path = scoped(prefix, "cpu.param[" + std::to_string(index) + "]");
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(core::where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        CpuProperty property;
        core::require_string(*table, "key", path + ".key", property.key, builder);
        core::require_string(*table, "expr", path + ".expr", property.expr, builder);
        fallback.properties.push_back(std::move(property));
        ++index;
    }

    builder.pack.cpu = std::move(fallback);
}

// The audio backend table: `[audio] element = "..."` plus one
// `[[audio.param]]` per element property, each a `key` (the property) and an
// `expr` over the pack's parameters - the same shape as `[cpu]`, so the
// manifest reads the same way and the two share one validation path.
void read_audio(const toml::table &root, std::string_view prefix, Builder &builder)
{
    const toml::node *audio_node = root.get("audio");
    if (audio_node == nullptr) {
        return;
    }
    const toml::table *audio = audio_node->as_table();
    if (audio == nullptr) {
        builder.fail(core::where_at(scoped(prefix, "audio"), *audio_node),
                     "[audio] must be a table");
        return;
    }

    AudioBackend backend;
    core::require_string(*audio, "element", scoped(prefix, "audio.element"), backend.element,
                         builder);

    const toml::node *params_node = audio->get("param");
    if (params_node == nullptr) {
        return;
    }
    const toml::array *params = params_node->as_array();
    if (params == nullptr) {
        builder.fail(core::where_at(scoped(prefix, "audio.param"), *params_node),
                     "`param` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *params) {
        const std::string path = scoped(prefix, "audio.param[" + std::to_string(index) + "]");
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(core::where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        CpuProperty property;
        core::require_string(*table, "key", path + ".key", property.key, builder);
        core::require_string(*table, "expr", path + ".expr", property.expr, builder);
        backend.properties.push_back(std::move(property));
        ++index;
    }

    builder.pack.audio = std::move(backend);
}

// Reads the effect identity fields (id, name, kind, etc.) from an effect
// table — the `[effect]` table in standalone form, or the element table of
// a `[[effect]]` block in scoped form. `prefix` is the diagnostic prefix:
// "effect" for standalone, e.g. "effect[0]" for scoped.
void read_effect_fields(const toml::table &table, std::string_view prefix, Builder &builder)
{
    core::require_string(table, "id", scoped(prefix, "id"), builder.pack.id, builder);
    core::require_string(table, "name", scoped(prefix, "name"), builder.pack.name, builder);

    const toml::node *kind_node = table.get("kind");
    if (kind_node == nullptr) {
        builder.fail(scoped(prefix, "kind"), "missing `kind`");
    } else {
        const auto kind = kind_node->value<std::string>();
        if (!kind) {
            builder.fail(core::where_at(scoped(prefix, "kind"), *kind_node),
                         "`kind` must be a string");
        } else if (const auto mapped = kind_from_name(*kind)) {
            builder.pack.kind = *mapped;
        } else {
            builder.fail(core::where_at(scoped(prefix, "kind"), *kind_node),
                         "unknown kind `" + *kind + "`");
        }
    }

    core::read_string(table, "category", scoped(prefix, "category"), builder.pack.category,
                      builder);
    core::read_string(table, "description", scoped(prefix, "description"), builder.pack.description,
                      builder);
    core::read_uint32(table, "version", scoped(prefix, "version"), builder.pack.version, builder);
    core::read_int32(table, "order", scoped(prefix, "order"), builder.pack.order, builder);
    core::read_optional_string(table, "intensity", scoped(prefix, "intensity"),
                               builder.pack.intensity, builder);
    std::optional<std::string> opt_body;
    core::read_optional_string(table, "body", scoped(prefix, "body"), opt_body, builder);
    if (opt_body)
        builder.pack.body = std::move(*opt_body);
}

// Reads the standalone `[effect]` table from root, then delegates to the
// parametrized reader. Sub-tables are NOT read here — they come from root
// (fill_pack passes the document root directly to each sub-table reader).
void read_effect(const toml::table &root, Builder &builder)
{
    const toml::node *effect_node = root.get("effect");
    if (effect_node == nullptr) {
        builder.fail("effect", "missing [effect] table");
        return;
    }
    const toml::table *effect = effect_node->as_table();
    if (effect == nullptr) {
        builder.fail(core::where_at("effect", *effect_node), "[effect] must be a table");
        return;
    }
    read_effect_fields(*effect, "effect", builder);
}

// Reads every table the schema carries. There is exactly one Pack manifest
// format, so no version gate is read; an unknown top-level key (for example an
// obsolete `format` left over from an older manifest) is ignored, the reader's
// normal look-up-only policy. What separates a picture Pack from the older
// sound-chain shape is the required `[effect]` table itself (a sound chain had
// only `[ffmpeg]` and no `[effect]`), and within it `kind` carries the
// picture/audio/transition discrimination - both already enforced by
// read_effect, so no further gate is needed.
void fill_pack(const toml::table &root, Builder &builder)
{
    read_effect(root, builder);
    // Sub-tables come from the document root with no scoping prefix — the
    // standalone effect.toml form places them at top level.
    read_params(root, "" /* prefix */, builder);
    read_uniforms(root, "" /* prefix */, builder);
    read_passes(root, "" /* prefix */, builder);
    read_transition(root, "" /* prefix */, builder);
    read_cpu(root, "" /* prefix */, builder);
    read_audio(root, "" /* prefix */, builder);
}

// Folds the schema's own validation into the problems (rather than re-reading
// its rules here), then decides whether the Pack survives: any problem leaves
// `pack` empty.
LoadResult finish(Builder builder)
{
    if (builder.problems.empty()) {
        const std::string prefix = builder.pack.id + ": ";
        for (const std::string &message : validate(builder.pack)) {
            LoadError problem;
            problem.where = builder.pack.id;
            problem.what = message;
            if (message.starts_with(prefix)) {
                problem.what = message.substr(prefix.size());
            }
            builder.problems.push_back(std::move(problem));
        }
    }
    LoadResult result;
    if (builder.problems.empty()) {
        result.pack = std::move(builder.pack);
    }
    result.problems = std::move(builder.problems);
    return result;
}

} // namespace

LoadResult load_effect_table(const toml::table &table, std::string_view path)
{
    Builder builder;
    try {
        // `table` is the effect element — its fields become the Pack's
        // identity, and its sub-keys (param, uniform, wgsl, etc.) become the
        // scoped sub-tables. The same `read_effect_fields` + sub-table readers
        // serve both the standalone and scoped forms; `path` distinguishes the
        // error-path prefixes.
        read_effect_fields(table, path, builder);
        read_params(table, path, builder);
        read_uniforms(table, path, builder);
        read_passes(table, path, builder);
        read_transition(table, path, builder);
        read_cpu(table, path, builder);
        read_audio(table, path, builder);
    } catch (const std::exception &error) {
        builder.fail(std::string(path), error.what());
    }
    return finish(std::move(builder));
}

LoadResult load_pack(std::string_view toml_text)
{
    Builder builder;
    try {
        const toml::table root = toml::parse(toml_text);
        fill_pack(root, builder);
    } catch (const toml::parse_error &error) {
        builder.fail(core::where_of(error.source()), std::string(error.description()));
    } catch (const std::exception &error) {
        builder.fail("<manifest>", error.what());
    }
    return finish(std::move(builder));
}

LoadResult load_pack_file(const std::filesystem::path &path)
{
    Builder builder;
    try {
        const std::string path_text = path.string();
        const toml::table root = toml::parse_file(path_text);
        fill_pack(root, builder);
    } catch (const toml::parse_error &error) {
        builder.fail(core::where_of(error.source()), std::string(error.description()));
    } catch (const std::exception &error) {
        builder.fail(path.string(), error.what());
    }
    return finish(std::move(builder));
}

} // namespace genesis::effects
