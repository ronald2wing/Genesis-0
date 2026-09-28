// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "effects/Pack.h"

#include <algorithm>
#include <array>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/TomlRead.h"
#include "effects/Expr.h"

namespace genesis::effects {

// The FFmpeg `xfade` transition names a transition form may name as its
// export fallback. Validated at load so a typo is a load error, not a silent
// hard cut at export.
inline constexpr std::array<std::string_view, 58> XFADE_NAMES = {
    "fade",       "fadeblack",   "fadewhite",  "fadegrays",   "fadefast",   "fadeslow",
    "dissolve",   "pixelize",    "distance",   "radial",      "smoothleft", "smoothright",
    "smoothup",   "smoothdown",  "circleopen", "circleclose", "circlecrop", "rectcrop",
    "wipeleft",   "wiperight",   "wipeup",     "wipedown",    "wipetl",     "wipetr",
    "wipebl",     "wipebr",      "slideleft",  "slideright",  "slideup",    "slidedown",
    "vertopen",   "vertclose",   "horzopen",   "horzclose",   "diagtl",     "diagtr",
    "diagbl",     "diagbr",      "hlslice",    "hrslice",     "vuslice",    "vdslice",
    "hblur",      "squeezeh",    "squeezev",   "zoomin",      "hlwind",     "hrwind",
    "vuwind",     "vdwind",      "coverleft",  "coverright",  "coverup",    "coverdown",
    "revealleft", "revealright", "revealup",   "revealdown",
};

// The largest parameter block a Pack may declare. naga's load-time binding
// budget was not carried to the host (docs/decisions/effect-execution.md §5),
// so the schema owns this bound instead: 16 KiB, the GL ES 3.0 floor for a
// uniform block, is far more than any real Pack's knobs.
inline constexpr std::uint32_t MAX_PARAMS_BLOCK = 16384;

// Names a pass's picture may not have: the last pass's function, and the
// entry point that draws it.
inline constexpr std::array<std::string_view, 2> TAKEN_TARGETS = { "effect", "main" };

namespace {

bool is_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// A shader identifier: lower-case first, then lower-case letters, digits and
// underscores.
bool is_ident(std::string_view text)
{
    if (text.empty() || !(is_lower(text.front()) || text.front() == '_')) {
        return false;
    }
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        if (!(is_lower(c) || is_digit(c) || c == '_')) {
            return false;
        }
    }
    return true;
}

// Empty but for ASCII whitespace, matching the manifest's `.trim()`.
bool is_blank(std::string_view text)
{
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return false;
        }
    }
    return true;
}

// The size and alignment, in bytes, of a parameter's uniform field - what
// the host lays out in place of an offset table read back from
// naga. A float/int/bool/enum is one f32; a point a vec2, a wheel a vec3, a
// color a vec4, and a curve an array<vec4, MAX_CURVE_POINTS>.
struct FieldLayout
{
    std::uint32_t size;
    std::uint32_t align;
};

FieldLayout field_layout(ParamType type)
{
    switch (type) {
    case ParamType::Point:
        return { 8, 8 };
    case ParamType::Wheel:
        return { 12, 16 };
    case ParamType::Color:
        return { 16, 16 };
    case ParamType::Curve:
        return { static_cast<std::uint32_t>(MAX_CURVE_POINTS) * 16, 16 };
    case ParamType::Float:
    case ParamType::Int:
    case ParamType::Bool:
    case ParamType::Enum:
        return { 4, 4 };
    }
    return { 4, 4 };
}

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

bool is_known_xfade(std::string_view name)
{
    return std::find(XFADE_NAMES.begin(), XFADE_NAMES.end(), name) != XFADE_NAMES.end();
}

} // namespace

std::uint32_t lay_out(std::vector<Parameter> &parameters)
{
    std::uint32_t offset = 0;
    for (Parameter &parameter : parameters) {
        const FieldLayout layout = field_layout(parameter.type);
        offset = align_up(offset, layout.align);
        parameter.offset = offset;
        offset += layout.size;
    }
    return align_up(std::max(offset, MIN_PARAMS), 16);
}

std::uint32_t span_with_uniforms(const Pack &pack)
{
    std::uint32_t offset = 0;
    for (const Parameter &parameter : pack.parameters) {
        const FieldLayout layout = field_layout(parameter.type);
        offset = align_up(offset, layout.align) + layout.size;
    }
    // Each derived uniform is one float field, laid out after the parameters.
    for (std::size_t i = 0; i < pack.uniforms.size(); ++i) {
        offset = align_up(offset, 4) + 4;
    }
    return align_up(std::max(offset, MIN_PARAMS), 16);
}

std::vector<std::string> validate(const Pack &pack)
{
    std::vector<std::string> problems;
    const auto invalid = [&](std::string message) {
        problems.push_back(pack.id + ": " + std::move(message));
    };

    if (!core::is_namespaced_id(pack.id)) {
        invalid("id must be `author.name`, lower-case letters, digits and hyphens");
    }
    if (is_blank(pack.name)) {
        invalid("name is empty");
    }

    std::set<std::string_view> seen;
    for (const Parameter &parameter : pack.parameters) {
        if (!is_ident(parameter.key)) {
            invalid("parameter key `" + parameter.key
                    + "` must be lower-case letters, digits and underscores");
        }
        if (parameter.key == "index") {
            invalid("`index` is reserved");
        }
        // The key every filter answers to without declaring it: how much of
        // the look is applied, read by the catalogue as a percent. A
        // parameter under that name would be read twice, once as itself and
        // once as the mix, and the two would disagree.
        if (parameter.key == "intensity") {
            invalid("`intensity` is reserved: it is the mix every filter takes");
        }
        if (!seen.insert(parameter.key).second) {
            invalid("parameter `" + parameter.key + "` is declared twice");
        }
        if (is_blank(parameter.label)) {
            invalid("parameter `" + parameter.key + "` has no label");
        }
        if (parameter.min > parameter.max) {
            invalid("parameter `" + parameter.key + "`: min is above max");
        }
        if (parameter.default_value < parameter.min || parameter.default_value > parameter.max) {
            invalid("parameter `" + parameter.key + "`: default "
                    + std::to_string(parameter.default_value) + " is outside "
                    + std::to_string(parameter.min) + ".." + std::to_string(parameter.max));
        }
        if (parameter.type == ParamType::Curve && parameter.animate) {
            invalid("curve `" + parameter.key + "` holds for the clip: it cannot be animated");
        }
        if (parameter.type == ParamType::Enum) {
            if (parameter.values.empty()) {
                invalid("enum `" + parameter.key + "` lists no values");
            }
            if (parameter.values.size() != parameter.labels.size()) {
                invalid("enum `" + parameter.key + "` has "
                        + std::to_string(parameter.values.size()) + " values and "
                        + std::to_string(parameter.labels.size()) + " labels");
            }
        }
    }

    if (pack.intensity
        && std::none_of(
                pack.parameters.begin(), pack.parameters.end(),
                [&](const Parameter &parameter) { return parameter.key == *pack.intensity; })) {
        invalid("intensity names `" + *pack.intensity + "`, which is not a parameter");
    }

    // A derived uniform may reference parameters only, never other uniforms,
    // so the name snapshot for the expression check is the parameter keys
    // alone - a uniform's value is laid out after all of them, and reading
    // another uniform from inside one would be a use before its value exists.
    std::set<std::string_view> param_keys;
    for (const Parameter &parameter : pack.parameters) {
        param_keys.insert(parameter.key);
    }
    for (const Uniform &uniform : pack.uniforms) {
        if (!is_ident(uniform.key)) {
            invalid("uniform key `" + uniform.key
                    + "` must be lower-case letters, digits and underscores");
        }
        if (uniform.key == "index") {
            invalid("`index` is reserved");
        }
        if (uniform.key == "intensity") {
            invalid("`intensity` is reserved: it is the mix every filter takes");
        }
        if (!seen.insert(uniform.key).second) {
            invalid("uniform `" + uniform.key + "` collides with a parameter or another uniform");
        }
        std::string error;
        if (!expr::check(
                    uniform.expr,
                    [&](std::string_view name) {
                        return param_keys.find(name) != param_keys.end();
                    },
                    &error)) {
            invalid("uniform `" + uniform.key + "`: " + error);
        }
    }

    // A backend table - `[cpu]` for a picture Pack, `[audio]` for a sound Pack
    // - names a stock element and maps each element property to an expression
    // over the pack's parameters only, exactly like a derived uniform. The
    // element's existence is a host concern the adapter checks, not the
    // loader's; the loader only rejects a manifest that spells its mapping
    // wrong (a typo an availability check could never catch).
    const auto check_backend = [&](std::string_view table, const std::string &element,
                                   const std::vector<CpuProperty> &properties) {
        const std::string prefix = "[" + std::string(table) + "]";
        if (is_blank(element)) {
            invalid(prefix + " element is empty");
        }
        std::set<std::string_view> property_keys;
        for (const CpuProperty &property : properties) {
            if (is_blank(property.key)) {
                invalid(prefix + " property key is empty");
            }
            if (!property_keys.insert(property.key).second) {
                invalid(prefix + " property `" + property.key + "` is set twice");
            }
            std::string error;
            if (!expr::check(
                        property.expr,
                        [&](std::string_view name) {
                            return param_keys.find(name) != param_keys.end();
                        },
                        &error)) {
                invalid(prefix + " property `" + property.key + "`: " + error);
            }
        }
    };

    if (pack.cpu) {
        check_backend("cpu", pack.cpu->element, pack.cpu->properties);
    }
    if (pack.audio) {
        check_backend("audio", pack.audio->element, pack.audio->properties);
    }

    if (pack.kind == Kind::Transition) {
        if (!pack.transition) {
            invalid("a transition needs a [transition] table");
        }
    } else if (pack.transition) {
        invalid("[transition] is only for a transition package");
    }
    if (pack.kind == Kind::Audio) {
        if (!pack.audio) {
            invalid("an audio package needs an [audio] table");
        }
    } else if (pack.audio) {
        invalid("[audio] is only for an audio package");
    }
    if (pack.transition && pack.transition->xfade && !is_known_xfade(*pack.transition->xfade)) {
        invalid("[transition] xfade `" + *pack.transition->xfade
                + "` is not a known FFmpeg xfade name");
    }

    std::set<std::string_view> targets;
    for (const Pass &pass : pack.passes) {
        if (pass.target.empty() || !is_ident(pass.target) || !is_lower(pass.target.front())) {
            invalid("pass target `" + pass.target
                    + "` must be a lower-case letter, then letters, digits and "
                      "underscores");
        }
        if (std::find(TAKEN_TARGETS.begin(), TAKEN_TARGETS.end(), pass.target)
            != TAKEN_TARGETS.end()) {
            invalid("pass target `" + pass.target + "` is taken: `effect` is the last pass");
        }
        if (!targets.insert(pass.target).second) {
            invalid("pass target `" + pass.target + "` is drawn twice");
        }
    }
    if (pack.passes.size() > MAX_PASSES) {
        invalid(std::to_string(pack.passes.size()) + " passes before `effect`; a package may draw "
                + std::to_string(MAX_PASSES));
    }

    const std::uint32_t span = span_with_uniforms(pack);
    if (span > MAX_PARAMS_BLOCK) {
        invalid("the declared parameter block is " + std::to_string(span)
                + " bytes; a package may declare up to " + std::to_string(MAX_PARAMS_BLOCK));
    }

    return problems;
}

} // namespace genesis::effects
