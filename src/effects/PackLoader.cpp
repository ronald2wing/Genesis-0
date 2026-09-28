// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-effects/src/manifest.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

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

namespace genesis::effects {

namespace {

// The manifest format this build reads: the shape of a picture Pack. Format 1
// is a sound (FFmpeg) chain, which the host control schema does not model, and
// anything above 2 is a newer build's manifest - both are rejected, never
// half-loaded.
inline constexpr std::uint32_t FORMAT = 2;

// "line L, column C", or empty where the position is unknown (a missing file).
std::string at(toml::source_position pos)
{
    if (!pos) {
        return { };
    }
    return "line " + std::to_string(pos.line) + ", column " + std::to_string(pos.column);
}

// A TOML path with the node's source location appended, e.g.
// `param[2].default (line 14, column 11)`. The path is the part a human greps
// for; the location is where toml++ saw it.
std::string where_at(std::string_view path, const toml::node &node)
{
    const std::string location = at(node.source().begin);
    if (location.empty()) {
        return std::string(path);
    }
    std::string out(path);
    out += " (";
    out += location;
    out += ")";
    return out;
}

// The `where` for a parse failure: the source file when there is one, then the
// position toml++ reports.
std::string where_of(const toml::source_region &region)
{
    std::string out;
    if (region.path && !region.path->empty()) {
        out = *region.path;
    }
    const std::string location = at(region.begin);
    if (!location.empty()) {
        if (!out.empty()) {
            out += ": ";
        }
        out += location;
    }
    if (out.empty()) {
        out = "<manifest>";
    }
    return out;
}

// The `kind` a manifest string spells, or nullopt for a kind a Pack never is.
std::optional<Kind> kind_from_name(std::string_view name)
{
    if (name == "effect")
        return Kind::Effect;
    if (name == "filter")
        return Kind::Filter;
    if (name == "audio")
        return Kind::Audio;
    if (name == "transition")
        return Kind::Transition;
    if (name == "generator")
        return Kind::Generator;
    return std::nullopt;
}

// Accumulates a Pack and the problems found while reading it. A problem
// recorded anywhere leaves the Pack unusable; the loader keeps reading so a
// human sees every problem at once rather than only the first.
struct Builder
{
    Pack pack;
    std::vector<LoadError> problems;

    void fail(std::string where, std::string what)
    {
        problems.push_back(LoadError{ std::move(where), std::move(what) });
    }
};

// --- typed field readers ------------------------------------------------
//
// Each reads one key of a table into a schema field, records a problem on a
// wrong type, and leaves the field untouched when the key is absent (so the
// schema's own defaults stand). `require_string` is the required variant.

void require_string(const toml::table &table, std::string_view key, std::string_view path,
                    std::string &out, Builder &builder)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        builder.fail(std::string(path), "missing `" + std::string(key) + "`");
        return;
    }
    const auto value = node->value<std::string>();
    if (!value) {
        builder.fail(where_at(path, *node), "`" + std::string(key) + "` must be a string");
        return;
    }
    out = *value;
}

void read_string(const toml::table &table, std::string_view key, std::string_view path,
                 std::string &out, Builder &builder)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value<std::string>();
    if (!value) {
        builder.fail(where_at(path, *node), "`" + std::string(key) + "` must be a string");
        return;
    }
    out = *value;
}

void read_optional_string(const toml::table &table, std::string_view key, std::string_view path,
                          std::optional<std::string> &out, Builder &builder)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value<std::string>();
    if (!value) {
        builder.fail(where_at(path, *node), "`" + std::string(key) + "` must be a string");
        return;
    }
    out = *value;
}

void read_uint32(const toml::table &table, std::string_view key, std::string_view path,
                 std::uint32_t &out, Builder &builder)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value_exact<std::int64_t>();
    if (!value || *value < 0
        || static_cast<std::uint64_t>(*value)
                > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        builder.fail(where_at(path, *node), "`" + std::string(key) + "` must be a whole number");
        return;
    }
    out = static_cast<std::uint32_t>(*value);
}

void read_int32(const toml::table &table, std::string_view key, std::string_view path,
                std::int32_t &out, Builder &builder)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value_exact<std::int64_t>();
    if (!value || *value < std::numeric_limits<std::int32_t>::min()
        || *value > std::numeric_limits<std::int32_t>::max()) {
        builder.fail(where_at(path, *node), "`" + std::string(key) + "` must be a whole number");
        return;
    }
    out = static_cast<std::int32_t>(*value);
}

void read_double(const toml::table &table, std::string_view key, std::string_view path, double &out,
                 Builder &builder)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value<double>();
    if (!value) {
        builder.fail(where_at(path, *node), "`" + std::string(key) + "` must be a number");
        return;
    }
    out = *value;
}

void read_bool(const toml::table &table, std::string_view key, std::string_view path, bool &out,
               Builder &builder)
{
    const toml::node *node = table.get(key);
    if (node == nullptr) {
        return;
    }
    const auto value = node->value_exact<bool>();
    if (!value) {
        builder.fail(where_at(path, *node), "`" + std::string(key) + "` must be true or false");
        return;
    }
    out = *value;
}

// --- table readers ------------------------------------------------------

void read_params(const toml::table &root, Builder &builder)
{
    const toml::node *params_node = root.get("param");
    if (params_node == nullptr) {
        return;
    }
    const toml::array *params = params_node->as_array();
    if (params == nullptr) {
        builder.fail(where_at("param", *params_node), "`param` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *params) {
        const std::string path = "param[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        Parameter parameter;
        require_string(*table, "key", path + ".key", parameter.key, builder);
        require_string(*table, "label", path + ".label", parameter.label, builder);
        read_string(*table, "group", path + ".group", parameter.group, builder);
        read_string(*table, "unit", path + ".unit", parameter.unit, builder);

        const toml::node *type_node = table->get("type");
        if (type_node != nullptr) {
            const auto type = type_node->value<std::string>();
            if (!type) {
                builder.fail(where_at(path + ".type", *type_node), "`type` must be a string");
            } else if (const auto mapped = from_name(*type)) {
                parameter.type = *mapped;
            } else {
                builder.fail(where_at(path + ".type", *type_node),
                             "unknown parameter type `" + *type + "`");
            }
        }

        read_double(*table, "min", path + ".min", parameter.min, builder);
        read_double(*table, "max", path + ".max", parameter.max, builder);
        read_double(*table, "default", path + ".default", parameter.default_value, builder);
        read_double(*table, "step", path + ".step", parameter.step, builder);
        read_bool(*table, "animate", path + ".animate", parameter.animate, builder);

        const toml::node *values_node = table->get("values");
        if (values_node != nullptr) {
            const toml::array *values = values_node->as_array();
            if (values == nullptr) {
                builder.fail(where_at(path + ".values", *values_node), "`values` must be an array");
            } else {
                std::size_t i = 0;
                for (const toml::node &value : *values) {
                    const auto number = value.value<double>();
                    if (!number) {
                        builder.fail(where_at(path + ".values[" + std::to_string(i) + "]", value),
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
                builder.fail(where_at(path + ".labels", *labels_node), "`labels` must be an array");
            } else {
                std::size_t i = 0;
                for (const toml::node &label : *labels) {
                    const auto text = label.value<std::string>();
                    if (!text) {
                        builder.fail(where_at(path + ".labels[" + std::to_string(i) + "]", label),
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

void read_uniforms(const toml::table &root, Builder &builder)
{
    const toml::node *uniforms_node = root.get("uniform");
    if (uniforms_node == nullptr) {
        return;
    }
    const toml::array *uniforms = uniforms_node->as_array();
    if (uniforms == nullptr) {
        builder.fail(where_at("uniform", *uniforms_node), "`uniform` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *uniforms) {
        const std::string path = "uniform[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        Uniform uniform;
        require_string(*table, "key", path + ".key", uniform.key, builder);
        require_string(*table, "expr", path + ".expr", uniform.expr, builder);
        builder.pack.uniforms.push_back(std::move(uniform));
        ++index;
    }
}

void read_passes(const toml::table &root, Builder &builder)
{
    const toml::node *wgsl_node = root.get("wgsl");
    if (wgsl_node == nullptr) {
        return;
    }
    // The shader body named by `[wgsl].entry` (effect.wgsl / GLSL) is not this
    // loader's business: the manifest is declarative data, and the execution
    // path resolves the body later (docs/decisions/effects-execution.md §3).
    // `entry` and `space` are therefore left unread here; only the passes the
    // schema carries are mapped.
    const toml::table *wgsl = wgsl_node->as_table();
    if (wgsl == nullptr) {
        builder.fail(where_at("wgsl", *wgsl_node), "[wgsl] must be a table");
        return;
    }
    const toml::node *passes_node = wgsl->get("pass");
    if (passes_node == nullptr) {
        return;
    }
    const toml::array *passes = passes_node->as_array();
    if (passes == nullptr) {
        builder.fail(where_at("wgsl.pass", *passes_node), "`pass` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *passes) {
        const std::string path = "wgsl.pass[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        Pass pass;
        require_string(*table, "target", path + ".target", pass.target, builder);

        const toml::node *shrink_node = table->get("shrink");
        if (shrink_node != nullptr) {
            const toml::array *shrink = shrink_node->as_array();
            if (shrink == nullptr || shrink->size() != 2) {
                builder.fail(where_at(path + ".shrink", *shrink_node),
                             "`shrink` must be two expressions: across and down");
            } else {
                std::array<std::string, 2> pair{ };
                bool ok = true;
                for (std::size_t i = 0; i < 2; ++i) {
                    const toml::node &expression = (*shrink)[i];
                    const auto text = expression.value<std::string>();
                    if (!text) {
                        builder.fail(
                                where_at(path + ".shrink[" + std::to_string(i) + "]", expression),
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

void read_transition(const toml::table &root, Builder &builder)
{
    const toml::node *transition_node = root.get("transition");
    if (transition_node == nullptr) {
        return;
    }
    const toml::table *transition = transition_node->as_table();
    if (transition == nullptr) {
        builder.fail(where_at("transition", *transition_node), "[transition] must be a table");
        return;
    }

    TransitionForm form;
    require_string(*transition, "entry", "transition.entry", form.entry, builder);
    read_optional_string(*transition, "xfade", "transition.xfade", form.xfade, builder);
    read_optional_string(*transition, "fallback", "transition.fallback", form.fallback, builder);
    builder.pack.transition = std::move(form);
}

// The CPU fallback table: `[cpu] element = "..."` plus one `[[cpu.param]]` per
// element property, each a `key` (the property) and an `expr` over the pack's
// parameters - the same array-of-tables shape as `[[uniform]]`, so the
// manifest reads the same way.
void read_cpu(const toml::table &root, Builder &builder)
{
    const toml::node *cpu_node = root.get("cpu");
    if (cpu_node == nullptr) {
        return;
    }
    const toml::table *cpu = cpu_node->as_table();
    if (cpu == nullptr) {
        builder.fail(where_at("cpu", *cpu_node), "[cpu] must be a table");
        return;
    }

    CpuFallback fallback;
    require_string(*cpu, "element", "cpu.element", fallback.element, builder);

    const toml::node *params_node = cpu->get("param");
    if (params_node == nullptr) {
        return;
    }
    const toml::array *params = params_node->as_array();
    if (params == nullptr) {
        builder.fail(where_at("cpu.param", *params_node), "`param` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *params) {
        const std::string path = "cpu.param[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        CpuProperty property;
        require_string(*table, "key", path + ".key", property.key, builder);
        require_string(*table, "expr", path + ".expr", property.expr, builder);
        fallback.properties.push_back(std::move(property));
        ++index;
    }

    builder.pack.cpu = std::move(fallback);
}

// The audio backend table: `[audio] element = "..."` plus one
// `[[audio.param]]` per element property, each a `key` (the property) and an
// `expr` over the pack's parameters - the same shape as `[cpu]`, so the
// manifest reads the same way and the two share one validation path.
void read_audio(const toml::table &root, Builder &builder)
{
    const toml::node *audio_node = root.get("audio");
    if (audio_node == nullptr) {
        return;
    }
    const toml::table *audio = audio_node->as_table();
    if (audio == nullptr) {
        builder.fail(where_at("audio", *audio_node), "[audio] must be a table");
        return;
    }

    AudioBackend backend;
    require_string(*audio, "element", "audio.element", backend.element, builder);

    const toml::node *params_node = audio->get("param");
    if (params_node == nullptr) {
        return;
    }
    const toml::array *params = params_node->as_array();
    if (params == nullptr) {
        builder.fail(where_at("audio.param", *params_node), "`param` must be an array of tables");
        return;
    }
    std::size_t index = 0;
    for (const toml::node &element : *params) {
        const std::string path = "audio.param[" + std::to_string(index) + "]";
        const toml::table *table = element.as_table();
        if (table == nullptr) {
            builder.fail(where_at(path, element), "must be a table");
            ++index;
            continue;
        }

        CpuProperty property;
        require_string(*table, "key", path + ".key", property.key, builder);
        require_string(*table, "expr", path + ".expr", property.expr, builder);
        backend.properties.push_back(std::move(property));
        ++index;
    }

    builder.pack.audio = std::move(backend);
}

void read_effect(const toml::table &root, Builder &builder)
{
    const toml::node *effect_node = root.get("effect");
    if (effect_node == nullptr) {
        builder.fail("effect", "missing [effect] table");
        return;
    }
    const toml::table *effect = effect_node->as_table();
    if (effect == nullptr) {
        builder.fail(where_at("effect", *effect_node), "[effect] must be a table");
        return;
    }

    require_string(*effect, "id", "effect.id", builder.pack.id, builder);
    require_string(*effect, "name", "effect.name", builder.pack.name, builder);

    const toml::node *kind_node = effect->get("kind");
    if (kind_node == nullptr) {
        builder.fail("effect.kind", "missing `kind`");
    } else {
        const auto kind = kind_node->value<std::string>();
        if (!kind) {
            builder.fail(where_at("effect.kind", *kind_node), "`kind` must be a string");
        } else if (const auto mapped = kind_from_name(*kind)) {
            builder.pack.kind = *mapped;
        } else {
            builder.fail(where_at("effect.kind", *kind_node), "unknown kind `" + *kind + "`");
        }
    }

    read_string(*effect, "category", "effect.category", builder.pack.category, builder);
    read_string(*effect, "description", "effect.description", builder.pack.description, builder);
    read_uint32(*effect, "version", "effect.version", builder.pack.version, builder);
    read_int32(*effect, "order", "effect.order", builder.pack.order, builder);
    read_optional_string(*effect, "intensity", "effect.intensity", builder.pack.intensity, builder);

    const toml::node *aliases_node = effect->get("aliases");
    if (aliases_node != nullptr) {
        const toml::array *aliases = aliases_node->as_array();
        if (aliases == nullptr) {
            builder.fail(where_at("effect.aliases", *aliases_node), "`aliases` must be an array");
        } else {
            std::size_t i = 0;
            for (const toml::node &alias : *aliases) {
                const auto text = alias.value<std::string>();
                if (!text) {
                    builder.fail(where_at("effect.aliases[" + std::to_string(i) + "]", alias),
                                 "must be a string");
                } else {
                    builder.pack.aliases.push_back(*text);
                }
                ++i;
            }
        }
    }
}

// Reads the format gate, then every table the schema carries. A rejected
// format stops here; the pack is not half-built.
void fill_pack(const toml::table &root, Builder &builder)
{
    std::uint32_t format = 1; // absent is the first format, not a picture Pack
    const toml::node *format_node = root.get("format");
    if (format_node != nullptr) {
        const auto value = format_node->value_exact<std::int64_t>();
        if (!value || *value < 0
            || static_cast<std::uint64_t>(*value)
                    > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            builder.fail(where_at("format", *format_node), "`format` must be a whole number");
            return;
        }
        format = static_cast<std::uint32_t>(*value);
    }
    if (format == 0) {
        builder.fail("format", "format is 0; the first package format is 1");
        return;
    }
    if (format > FORMAT) {
        builder.fail("format",
                     "written for package format " + std::to_string(format)
                             + ", and this build reads up to " + std::to_string(FORMAT));
        return;
    }
    if (format < FORMAT) {
        builder.fail("format",
                     "format " + std::to_string(format)
                             + " is a sound chain, not a picture Pack; "
                               "this build reads format "
                             + std::to_string(FORMAT));
        return;
    }

    read_effect(root, builder);
    read_params(root, builder);
    read_uniforms(root, builder);
    read_passes(root, builder);
    read_transition(root, builder);
    read_cpu(root, builder);
    read_audio(root, builder);
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

LoadResult load_pack(std::string_view toml_text)
{
    Builder builder;
    try {
        const toml::table root = toml::parse(toml_text);
        fill_pack(root, builder);
    } catch (const toml::parse_error &error) {
        builder.fail(where_of(error.source()), std::string(error.description()));
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
        builder.fail(where_of(error.source()), std::string(error.description()));
    } catch (const std::exception &error) {
        builder.fail(path.string(), error.what());
    }
    return finish(std::move(builder));
}

} // namespace genesis::effects
