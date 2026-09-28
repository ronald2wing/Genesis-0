// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "effects/Resolve.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "effects/Expr.h"
#include "render/LutLoader.h"
#include "workspace/AssetPaths.h"

namespace genesis::effects {

namespace {

// The key every filter answers to without declaring it: how much of the look
// is applied, as a percent. Always owned by the schema, never a declared
// parameter (Pack::validate refuses one under this name).
constexpr std::string_view INTENSITY = "intensity";

// The enum parameter a colour-look pack declares to pick one of the shipped
// 3D LUTs. Its resolved value is an index into the sorted `.cube` files under
// asset_root()/"luts"; see `resolve_lut`. A pack with no such parameter - or
// one that is not an enum - never triggers LUT binding.
constexpr std::string_view LUT_PARAM = "lut";

// FNV-1a, so a fingerprint is the same on every machine and needs no
// dependency. The same constants ShaderPass.cpp uses for a table's id.
std::uint64_t fnv64(const std::uint8_t *bytes, std::size_t count)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < count; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= 0x00000100000001b3ULL;
    }
    return hash;
}

std::uint64_t fnv64(const std::string &text)
{
    return fnv64(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
}

// A 64-bit fingerprint as sixteen hex digits, zero-padded: the tail of the
// pipeline key.
std::string hex16(std::uint64_t value)
{
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

// The resolved value set: a transparent-comparator map so a string_view probe
// (the resolver's keys are string_views and dotted concatenations) finds a key
// without materialising a std::string per lookup. The pass's `values` - a
// plain map, the resolved-payload contract - is built from this at the resolve
// boundary.
using Values = std::map<std::string, double, std::less<>>;

// The compiled form of an expression, parsed once and shared process-wide.
// compile is pure over its source, so a repeated expression - the same
// manifest uniform evaluated every frame - parses exactly once. A null return
// is a source that does not compile; the caller falls back to `expr::eval` to
// report the real reason (a broken expression is a build-time manifest error,
// not a per-frame cost).
const expr::Compiled *compiled_expr(std::string_view source)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, std::shared_ptr<const expr::Compiled>> cache;
    const std::string key(source);
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second.get();
        }
    }
    std::string error;
    std::optional<expr::Compiled> compiled = expr::compile(source, &error);
    if (!compiled) {
        return nullptr;
    }
    auto shared = std::make_shared<const expr::Compiled>(std::move(*compiled));
    std::lock_guard<std::mutex> lock(mutex);
    return cache.emplace(key, std::move(shared)).first->second.get();
}

// The value of `source` resolved through `lookup`, using the process-wide
// compiled cache so the parse happens once; a source that fails to compile
// falls back to `expr::eval`, which reports the exact failure through `error`.
std::optional<double>
eval_expr(std::string_view source,
          const std::function<std::optional<double>(std::string_view)> &lookup, std::string *error)
{
    const expr::Compiled *compiled = compiled_expr(source);
    if (compiled == nullptr) {
        return expr::eval(source, lookup, error);
    }
    return expr::eval(*compiled, lookup, error);
}

// The size and alignment, in bytes, of a parameter's uniform field - what
// Pack::lay_out computes, repeated here because the resolver holds a const
// Pack whose offsets the loader never wrote.
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

// The GLSL type a parameter's field must be declared as, so the shader's
// uniform block matches the bytes laid down. Every kind is stored as floats:
// a scalar is float, a point vec2, a wheel vec3, a color vec4, and a curve an
// array of MAX_CURVE_POINTS vec4s - the same mapping `field_layout` sizes.
std::string glsl_type(ParamType type)
{
    switch (type) {
    case ParamType::Point:
        return "vec2";
    case ParamType::Wheel:
        return "vec3";
    case ParamType::Color:
        return "vec4";
    case ParamType::Curve:
        return "vec4[" + std::to_string(MAX_CURVE_POINTS) + "]";
    case ParamType::Float:
    case ParamType::Int:
    case ParamType::Bool:
    case ParamType::Enum:
        return "float";
    }
    return "float";
}

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// Whether the knob is stored as several numbers under dotted keys of its
// own, rather than as one number under its key.
bool is_compound(ParamType type)
{
    return type == ParamType::Point || type == ParamType::Wheel || type == ParamType::Curve;
}

// A non-negative whole number, or nullopt for anything that is not one. The
// curve-point index a dotted key spells.
std::optional<std::uint32_t> parse_u32(std::string_view text)
{
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const std::uint32_t digit = static_cast<std::uint32_t>(c - '0');
        if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = (value * 10) + digit;
    }
    return value;
}

// The declared parameter with this key, or null.
const Parameter *lookup(const Pack &pack, std::string_view key)
{
    for (const Parameter &parameter : pack.parameters) {
        if (parameter.key == key) {
            return &parameter;
        }
    }
    return nullptr;
}

// Whether the schema owns this document key: a declared parameter's own, a
// compound knob's for one of its dotted keys - a point's `<key>.x`, a
// wheel's `<key>.m`, a curve's `<key>.<n>.<axis>` - or the reserved
// `intensity`. A key the schema does not own is a stale or hand-edited
// project name, and the resolver refuses the instance rather than guess.
bool is_owned(const Pack &pack, std::string_view key)
{
    if (lookup(pack, key) != nullptr || key == INTENSITY) {
        return true;
    }
    const std::size_t dot = key.find('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    const Parameter *parameter = lookup(pack, key.substr(0, dot));
    if (parameter == nullptr) {
        return false;
    }
    const std::string_view rest = key.substr(dot + 1);
    switch (parameter->type) {
    case ParamType::Point:
        return rest == "x" || rest == "y";
    case ParamType::Wheel:
        return rest == "x" || rest == "y" || rest == "m";
    case ParamType::Curve: {
        const std::size_t inner = rest.find('.');
        if (inner == std::string_view::npos) {
            return false;
        }
        const std::string_view axis = rest.substr(inner + 1);
        if (axis != "x" && axis != "y") {
            return false;
        }
        const std::optional<std::uint32_t> n = parse_u32(rest.substr(0, inner));
        return n.has_value() && *n < MAX_CURVE_POINTS;
    }
    case ParamType::Float:
    case ParamType::Int:
    case ParamType::Bool:
    case ParamType::Enum:
    case ParamType::Color:
        return false;
    }
    return false;
}

// The resolved Params uniform: the bytes and the field layout that names
// them, produced together so a field and its bytes can never disagree.
struct ParamsBlock
{
    std::vector<std::uint8_t> bytes;
    std::vector<genesis::core::ParamField> fields;
    // Set when a derived uniform fails to evaluate; the block is unusable and
    // the resolver refuses the pass rather than upload a partial block.
    std::optional<std::string> error;
};

// A value in the resolved set, or a fallback.
double value_of(const Values &values, std::string_view key, double fallback)
{
    const auto it = values.find(key);
    return it != values.end() ? it->second : fallback;
}

// A curve's points as the document stores them under `key`, in the unit
// square and in order along it, at most MAX_CURVE_POINTS; fewer than two is
// the straight line from black to white. Two points at one place along the
// curve are one, the later.
std::vector<std::pair<double, double>> curve_points(const Values &values, std::string_view key)
{
    std::vector<std::pair<double, double>> points;
    for (std::size_t n = 0; n < MAX_CURVE_POINTS; ++n) {
        const std::string prefix = std::string(key) + "." + std::to_string(n) + ".";
        const auto x = values.find(prefix + "x");
        const auto y = values.find(prefix + "y");
        if (x == values.end() || y == values.end()) {
            continue;
        }
        if (!std::isfinite(x->second) || !std::isfinite(y->second)) {
            continue;
        }
        points.emplace_back(std::clamp(x->second, 0.0, 1.0), std::clamp(y->second, 0.0, 1.0));
    }
    std::stable_sort(points.begin(), points.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::pair<double, double>> kept;
    for (const auto &point : points) {
        if (!kept.empty() && std::abs(point.first - kept.back().first) < 1e-6) {
            kept.back() = point;
        } else {
            kept.push_back(point);
        }
    }
    if (kept.size() < 2) {
        return { { 0.0, 0.0 }, { 1.0, 1.0 } };
    }
    return kept;
}

// The slope a curve leaves each of its points at: Fritsch and Carlson's
// (1980), so a curve through points that climb climbs everywhere between
// them, and one that turns never overshoots its points.
std::vector<double> monotone_slopes(const std::vector<std::pair<double, double>> &points)
{
    const std::size_t n = points.size();
    if (n < 2) {
        return std::vector<double>(n, 1.0);
    }
    std::vector<double> secants;
    secants.reserve(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        secants.push_back((points[i + 1].second - points[i].second)
                          / std::max(points[i + 1].first - points[i].first, 1e-9));
    }
    std::vector<double> slopes(n, 0.0);
    slopes[0] = secants[0];
    slopes[n - 1] = secants[n - 2];
    for (std::size_t k = 1; k + 1 < n; ++k) {
        slopes[k] = secants[k - 1] * secants[k] > 0.0 ? (secants[k - 1] + secants[k]) / 2.0 : 0.0;
    }
    for (std::size_t k = 0; k + 1 < n; ++k) {
        if (secants[k] == 0.0) {
            slopes[k] = 0.0;
            slopes[k + 1] = 0.0;
            continue;
        }
        const double a = slopes[k] / secants[k];
        const double b = slopes[k + 1] / secants[k];
        const double length = std::sqrt((a * a) + (b * b));
        if (length > 3.0) {
            slopes[k] = 3.0 / length * a * secants[k];
            slopes[k + 1] = 3.0 / length * b * secants[k];
        }
    }
    return slopes;
}

// The Params uniform for these resolved values, laid out to the schema's
// offsets over a buffer of the schema's padded span, and the fields that
// declare it. One walk of the schema writes both - a value is written at the
// offset its field declares - so the two can never disagree about where a
// value lives.
ParamsBlock params_block(const Pack &pack, const Values &values)
{
    ParamsBlock block;
    block.fields.reserve(pack.parameters.size() + pack.uniforms.size());
    block.bytes.resize(span_with_uniforms(pack), 0);
    const auto put = [&bytes = block.bytes](std::uint32_t offset, double value) {
        if (offset + 4 <= bytes.size()) {
            const float f = static_cast<float>(value);
            std::memcpy(bytes.data() + offset, &f, sizeof(f));
        }
    };
    std::uint32_t cursor = 0;
    for (const Parameter &parameter : pack.parameters) {
        const FieldLayout layout = field_layout(parameter.type);
        cursor = align_up(cursor, layout.align);
        const std::uint32_t offset = cursor;
        block.fields.push_back({ parameter.key, glsl_type(parameter.type), offset, layout.size });
        cursor += layout.size;
        switch (parameter.type) {
        case ParamType::Wheel: {
            put(offset, value_of(values, parameter.key + ".x", 0.0));
            put(offset + 4, value_of(values, parameter.key + ".y", 0.0));
            put(offset + 8, value_of(values, parameter.key + ".m", parameter.default_value));
            break;
        }
        case ParamType::Curve: {
            const std::vector<std::pair<double, double>> points =
                    curve_points(values, parameter.key);
            const std::vector<double> slopes = monotone_slopes(points);
            for (std::size_t index = 0; index < points.size(); ++index) {
                const std::uint32_t at = offset + (static_cast<std::uint32_t>(index) * 16);
                put(at, points[index].first);
                put(at + 4, points[index].second);
                put(at + 8, slopes[index]);
                put(at + 12, static_cast<double>(points.size()));
            }
            break;
        }
        case ParamType::Point: {
            put(offset, value_of(values, parameter.key + ".x", 0.5));
            put(offset + 4, value_of(values, parameter.key + ".y", 0.5));
            break;
        }
        case ParamType::Color: {
            const double raw = value_of(values, parameter.key, parameter.default_value);
            const std::uint32_t packed = !std::isfinite(raw) || raw <= 0.0
                    ? 0U
                    : (raw >= 4294967295.0 ? 0xffffffffU : static_cast<std::uint32_t>(raw));
            for (std::uint32_t index = 0; index < 4; ++index) {
                const std::uint32_t shift = 24 - (index * 8);
                put(offset + (index * 4), static_cast<double>((packed >> shift) & 0xff) / 255.0);
            }
            break;
        }
        case ParamType::Float:
        case ParamType::Int:
        case ParamType::Bool:
        case ParamType::Enum:
            put(offset, value_of(values, parameter.key, parameter.default_value));
            break;
        }
    }

    // Derived uniforms: one float field each, evaluated over the resolved
    // parameter values. Any failure - an unknown name, a division by zero, a
    // non-finite result - poisons the block, which the resolver refuses.
    const auto lookup = [&values](std::string_view name) -> std::optional<double> {
        const auto it = values.find(name);
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    };
    for (const Uniform &uniform : pack.uniforms) {
        cursor = align_up(cursor, 4);
        const std::uint32_t offset = cursor;
        block.fields.push_back({ uniform.key, "float", offset, 4 });
        cursor += 4;
        std::string error;
        const std::optional<double> value = eval_expr(uniform.expr, lookup, &error);
        if (!value.has_value()) {
            block.error = "uniform `" + uniform.key + "`: " + error;
            return block;
        }
        put(offset, *value);
    }

    return block;
}

// Every declared parameter, at its value in `set` or its default, plus a
// compound knob's dotted keys as `set` has them. Keys the schema does not
// own never reach here: the instance was refused first.
Values resolve_values(const Pack &pack, const std::map<std::string, double> &set)
{
    Values values;
    for (const Parameter &parameter : pack.parameters) {
        const auto set_it = set.find(parameter.key);
        values[parameter.key] = set_it != set.end() ? set_it->second : parameter.default_value;
        if (is_compound(parameter.type)) {
            const std::string prefix = parameter.key + ".";
            for (auto it = set.lower_bound(prefix);
                 it != set.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
                if (is_owned(pack, it->first)) {
                    values[it->first] = it->second;
                }
            }
        }
    }
    return values;
}

// Whether every name the instance sets - a constant in `params` or a ride in
// `keys` - is a key the schema owns.
bool instance_is_declared(const Pack &pack, const genesis::project::AppliedFilter &instance)
{
    const auto owned = [&pack](const auto &entry) { return is_owned(pack, entry.first); };
    return std::all_of(instance.params.begin(), instance.params.end(), owned)
            && std::all_of(instance.keys.begin(), instance.keys.end(), owned);
}

// How much of the look to keep over the untouched layer: a filter's reserved
// `intensity` percent, or the whole of it.
float intensity(const Pack &pack, const std::map<std::string, double> &set)
{
    if (pack.kind != Kind::Filter) {
        return 1.0F;
    }
    const auto it = set.find(std::string(INTENSITY));
    const double percent = it != set.end() ? it->second : 100.0;
    return static_cast<float>(std::clamp(percent / 100.0, 0.0, 1.0));
}

// The real table a colour-look pack binds. A pack declares one by giving it an
// enum parameter named `lut` (see LUT_PARAM); that value is an index into the
// sorted `.cube` files under asset_root()/"luts" - the order
// render::available_luts returns them in, which is the same order the pack's
// manifest lists its enum labels. The resolver loads the file at that index
// and hands the table back; any failure (no asset root, an index past the
// shipped tables, or a file that will not parse) returns nullopt, so the
// pack's GLSL body - a fixed approximation - stays the in-engine fallback and
// the resolve itself never fails.
std::optional<genesis::core::Lut> resolve_lut(const Pack &pack, const Values &values)
{
    const Parameter *parameter = lookup(pack, LUT_PARAM);
    if (parameter == nullptr || parameter->type != ParamType::Enum) {
        return std::nullopt;
    }
    const std::filesystem::path root = genesis::workspace::asset_root();
    if (root.empty()) {
        return std::nullopt;
    }
    const std::vector<std::filesystem::path> tables =
            genesis::render::available_luts(root / "luts");

    // The shader reads `floor(lut + 0.5)`, so an animated ride picks the
    // nearest table; the resolver rounds the same way so the two sides agree.
    const double selection = value_of(values, LUT_PARAM, parameter->default_value);
    const double pick = std::floor(selection + 0.5);
    if (!std::isfinite(pick) || pick < 0.0 || pick >= static_cast<double>(tables.size())) {
        return std::nullopt;
    }
    const genesis::render::LutResult loaded =
            genesis::render::load_lut(tables[static_cast<std::size_t>(pick)]);
    return loaded.lut;
}

// The schema's description as a fingerprint's worth of text: the parameter
// keys and type names in order, then each pass's target and shrink pair. The
// resolver holds no shader body, so this stands in for the source in the
// pipeline key; two packs that agree here but differ in their shader are the
// one the resolver cannot tell apart.
std::string fingerprint(const Pack &pack)
{
    std::string text;
    for (const Parameter &parameter : pack.parameters) {
        text += parameter.key;
        text.push_back(' ');
        text += name(parameter.type);
        text.push_back('\n');
    }
    for (const Uniform &uniform : pack.uniforms) {
        text += uniform.key;
        text.push_back(' ');
        text += uniform.expr;
        text.push_back('\n');
    }
    for (const Pass &pass : pack.passes) {
        text += pass.target;
        text.push_back('\n');
        if (pass.shrink) {
            text += (*pass.shrink)[0];
            text.push_back('\n');
            text += (*pass.shrink)[1];
            text.push_back('\n');
        }
    }
    return text;
}

// What a compiled pipeline is cached under: the package's id and version, and
// a fingerprint of its schema, so a package whose schema changes - version
// bump or not - gets a new pipeline.
std::string cache_key(const Pack &pack)
{
    if (pack.cache_key_memo) {
        return *pack.cache_key_memo;
    }
    const std::string key =
            pack.id + "@" + std::to_string(pack.version) + "#" + hex16(fnv64(fingerprint(pack)));
    pack.cache_key_memo = std::make_shared<const std::string>(key);
    return key;
}

} // namespace

bool is_expressible(const Pack &pack)
{
    return pack.passes.empty();
}

std::optional<genesis::core::ShaderPass>
resolve_pass(const Pack &pack, const genesis::project::AppliedFilter &instance, double at,
             std::optional<genesis::core::RevealMap> reveal_map)
{
    if (!is_expressible(pack) || pack.kind == Kind::Transition || pack.kind == Kind::Audio
        || !instance_is_declared(pack, instance)) {
        return std::nullopt;
    }
    const std::map<std::string, double> set = instance.params_at(at);
    const Values values = resolve_values(pack, set);
    ParamsBlock block = params_block(pack, values);
    if (block.error) {
        return std::nullopt;
    }

    genesis::core::ShaderPass pass;
    pass.package = pack.id;
    pass.key = cache_key(pack);
    pass.params = std::move(block.bytes);
    pass.fields = std::move(block.fields);
    pass.values.insert(values.begin(), values.end());
    pass.intensity = intensity(pack, set);
    pass.reveal_map = std::move(reveal_map);
    pass.lut = resolve_lut(pack, values);
    return pass;
}

std::optional<genesis::core::TransitionPass>
resolve_transition(const Pack &pack, const genesis::project::AppliedFilter &instance,
                   double progress)
{
    if (!pack.transition || !instance_is_declared(pack, instance)) {
        return std::nullopt;
    }
    const Values values = resolve_values(pack, instance.params);
    ParamsBlock block = params_block(pack, values);
    if (block.error) {
        return std::nullopt;
    }

    genesis::core::TransitionPass pass;
    pass.key = cache_key(pack);
    pass.params = std::move(block.bytes);
    pass.progress = static_cast<float>(std::clamp(progress, 0.0, 1.0));
    pass.xfade = pack.transition->xfade;
    return pass;
}

// The property values a backend table - `[cpu]` or `[audio]` - resolves to for
// one instance: each declared property evaluated over the instance's resolved
// parameters. Returns nullopt when any expression fails to evaluate (an
// unknown name, a division by zero, a non-finite result), so the caller
// refuses the instance rather than hand back a partial mapping.
std::optional<std::vector<CpuFallbackValue>>
resolve_properties(const Pack &pack, const std::vector<CpuProperty> &properties,
                   const genesis::project::AppliedFilter &instance, double at)
{
    const Values values = resolve_values(pack, instance.params_at(at));
    const auto lookup = [&values](std::string_view name) -> std::optional<double> {
        const auto it = values.find(name);
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    };

    std::vector<CpuFallbackValue> resolved;
    resolved.reserve(properties.size());
    for (const CpuProperty &property : properties) {
        std::string error;
        const std::optional<double> value = eval_expr(property.expr, lookup, &error);
        if (!value.has_value()) {
            return std::nullopt;
        }
        resolved.push_back(CpuFallbackValue{ property.key, *value });
    }
    return resolved;
}

std::optional<ResolvedCpuFallback>
resolve_cpu_fallback(const Pack &pack, const genesis::project::AppliedFilter &instance, double at)
{
    if (!pack.cpu || !instance_is_declared(pack, instance)) {
        return std::nullopt;
    }
    std::optional<std::vector<CpuFallbackValue>> values =
            resolve_properties(pack, pack.cpu->properties, instance, at);
    if (!values) {
        return std::nullopt;
    }
    return ResolvedCpuFallback{ pack.cpu->element, std::move(*values) };
}

std::optional<ResolvedAudio>
resolve_audio(const Pack &pack, const genesis::project::AppliedFilter &instance, double at)
{
    if (!pack.audio || !instance_is_declared(pack, instance)) {
        return std::nullopt;
    }
    std::optional<std::vector<CpuFallbackValue>> values =
            resolve_properties(pack, pack.audio->properties, instance, at);
    if (!values) {
        return std::nullopt;
    }
    return ResolvedAudio{ pack.audio->element, std::move(*values) };
}

} // namespace genesis::effects
