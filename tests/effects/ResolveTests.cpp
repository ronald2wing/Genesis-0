// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "effects/Expr.h"
#include "effects/Pack.h"
#include "effects/Resolve.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"
#include "project/model/Speed.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

using genesis::effects::AudioBackend;
using genesis::effects::CpuFallback;
using genesis::effects::is_expressible;
using genesis::effects::Kind;
using genesis::effects::Pack;
using genesis::effects::Parameter;
using genesis::effects::ParamType;
using genesis::effects::Pass;
using genesis::effects::resolve_audio;
using genesis::effects::resolve_cpu_fallback;
using genesis::effects::resolve_pass;
using genesis::effects::resolve_transition;
using genesis::effects::TransitionForm;
using genesis::effects::Uniform;
using genesis::project::AppliedFilter;
using genesis::project::KeyEase;

// The float at `offset` of a Params buffer, read back the way the shader
// will: a native little-endian f32.
float read_float(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    float value = 0.0f;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

// The declared field with this name, or null.
const genesis::core::ParamField *field_of(const genesis::core::ShaderPass &pass,
                                          const std::string &name)
{
    for (const auto &field : pass.fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

// A single-slider filter: one float knob with a keyable ride.
Pack filter_pack()
{
    Parameter amount;
    amount.key = "amount";
    amount.label = "Amount";
    amount.min = 0.0;
    amount.max = 1.0;
    amount.default_value = 0.5;
    amount.animate = true;

    Pack pack;
    pack.id = "concat.glow";
    pack.name = "Glow";
    pack.kind = Kind::Filter;
    pack.parameters = { amount };
    return pack;
}

// A transition Pack with an xfade fallback.
Pack transition_pack()
{
    TransitionForm transition;
    transition.entry = "effect.wgsl";
    transition.xfade = "fade";

    Pack pack;
    pack.id = "concat.dissolve";
    pack.name = "Dissolve";
    pack.kind = Kind::Transition;
    pack.transition = transition;
    return pack;
}

// A cpu-fallback pack: two knobs mapped 1:1 onto a stock element.
Pack cpu_pack()
{
    Parameter brightness;
    brightness.key = "brightness";
    brightness.label = "Brightness";
    brightness.min = -1.0;
    brightness.max = 1.0;
    brightness.default_value = 0.0;
    Parameter contrast;
    contrast.key = "contrast";
    contrast.label = "Contrast";
    contrast.min = 0.0;
    contrast.max = 2.0;
    contrast.default_value = 1.0;

    CpuFallback cpu;
    cpu.element = "videobalance";
    cpu.properties = { { "brightness", "brightness" }, { "contrast", "contrast" } };

    Pack pack;
    pack.id = "concat.video-balance";
    pack.name = "Video Balance";
    pack.kind = Kind::Effect;
    pack.parameters = { brightness, contrast };
    pack.cpu = cpu;
    return pack;
}

// An audio pack: one knob mapped onto a stock audio element's property.
Pack audio_pack()
{
    Parameter boost;
    boost.key = "boost";
    boost.label = "Boost";
    boost.min = -60.0;
    boost.max = 60.0;
    boost.default_value = 0.0;

    AudioBackend audio;
    audio.element = "rgvolume";
    audio.properties = { { "pre-amp", "boost" } };

    Pack pack;
    pack.id = "genesis.loudness";
    pack.name = "Loudness";
    pack.kind = Kind::Audio;
    pack.parameters = { boost };
    pack.audio = audio;
    return pack;
}

void test_an_expressible_filter_resolves_every_field()
{
    const Pack pack = filter_pack();
    const auto pass = resolve_pass(pack, AppliedFilter::create("concat.glow"), 0.0);
    check(pass.has_value(), "an expressible filter resolves");
    check(pass->package == "concat.glow", "package id");
    check(pass->key.rfind("concat.glow@1#", 0) == 0, "key carries id and version");
    check(pass->key.size() == std::string("concat.glow@1#").size() + 16,
          "key ends in a 16-digit fingerprint");
    check(pass->params.size() == 16, "a lone scalar spans the 16-byte floor");
    check(read_float(pass->params, 0) == 0.5f, "the amount default is laid out");
    check(pass->values.size() == 1 && pass->values.at("amount") == 0.5,
          "values carries the declared parameter");
    check(pass->intensity == 1.0f, "a filter at full intensity");
    check(pass->stages.empty(), "an expressible pack has no stages");
    check(pass->source == nullptr, "the resolver leaves the source to the engine");
    check(!pass->lut.has_value() && !pass->reveal_map.has_value(),
          "the resolver leaves lut and reveal_map unset");
    check(pass->fields.size() == 1 && pass->fields[0].name == "amount"
                  && pass->fields[0].type == "float" && pass->fields[0].offset == 0
                  && pass->fields[0].size == 4,
          "the one declared parameter is a field: a float at offset 0");
    check(read_float(pass->params, pass->fields[0].offset)
                  == static_cast<float>(pass->values.at("amount")),
          "the field's offset is where its value's bytes land");
}

void test_an_animated_parameter_rides()
{
    const Pack pack = filter_pack();
    AppliedFilter instance = AppliedFilter::create("concat.glow");
    instance.set_key("amount", 0.0, 0.0, KeyEase::linear());
    instance.set_key("amount", 1.0, 1.0, KeyEase::linear());
    const auto at_zero = resolve_pass(pack, instance, 0.0);
    const auto at_half = resolve_pass(pack, instance, 0.5);
    const auto at_one = resolve_pass(pack, instance, 1.0);
    check(at_zero.has_value() && at_half.has_value() && at_one.has_value(),
          "an animated filter resolves at every instant");
    check(read_float(at_zero->params, 0) == 0.0f, "the ride at 0");
    check(read_float(at_half->params, 0) == 0.5f, "the ride at 0.5");
    check(read_float(at_one->params, 0) == 1.0f, "the ride at 1");
    check(at_zero->values.at("amount") == 0.0, "values rides too");
}

void test_an_undeclared_parameter_is_refused()
{
    const Pack pack = filter_pack();
    AppliedFilter stray = AppliedFilter::create("concat.glow");
    stray.params["bogus"] = 3.0;
    check(!resolve_pass(pack, stray, 0.0).has_value(), "an undeclared parameter refuses the pass");

    AppliedFilter compound = AppliedFilter::create("concat.glow");
    compound.params["amount.z"] = 0.5;
    check(!resolve_pass(pack, compound, 0.0).has_value(), "a scalar has no dotted keys");
}

void test_a_named_intermediate_is_not_expressible()
{
    Pack pack = filter_pack();
    Pass pass;
    pass.target = "blur";
    pack.passes = { pass };
    check(!is_expressible(pack), "a pass is not expressible");
    check(!resolve_pass(pack, AppliedFilter::create("concat.glow"), 0.0).has_value(),
          "an inexpressible pack does not resolve");
    check(is_expressible(filter_pack()), "no passes is expressible");
}

void test_a_transition_resolves_progress_and_xfade()
{
    const Pack pack = transition_pack();
    const auto pass = resolve_transition(pack, AppliedFilter::create("concat.dissolve"), 0.25);
    check(pass.has_value(), "a transition resolves");
    check(pass->key.rfind("concat.dissolve@1#", 0) == 0,
          "the transition key carries id and version");
    check(pass->progress == 0.25f, "progress through the cut");
    check(pass->xfade.has_value() && *pass->xfade == "fade", "the xfade fallback is carried");

    check(resolve_transition(pack, AppliedFilter::create("concat.dissolve"), -1.0)->progress
                  == 0.0f,
          "progress is floored at 0");
    check(resolve_transition(pack, AppliedFilter::create("concat.dissolve"), 2.0)->progress == 1.0f,
          "progress is capped at 1");
}

void test_a_transition_and_an_effect_do_not_cross()
{
    check(!resolve_pass(transition_pack(), AppliedFilter::create("concat.dissolve"), 0.0)
                   .has_value(),
          "a transition has no effect pass");
    check(!resolve_transition(filter_pack(), AppliedFilter::create("concat.glow"), 0.5).has_value(),
          "an effect has no transition");
}

void test_intensity_is_the_reserved_mix()
{
    const Pack pack = filter_pack();
    AppliedFilter instance = AppliedFilter::create("concat.glow");
    instance.params["intensity"] = 40.0;
    const auto pass = resolve_pass(pack, instance, 0.0);
    check(pass.has_value(), "the reserved intensity resolves");
    check(pass->intensity == 0.4f, "40 percent is 0.4");
    check(pass->values.count("intensity") == 0, "intensity is the mix, not a value");
}

void test_a_version_bump_changes_the_key()
{
    const Pack one = filter_pack();
    Pack two = filter_pack();
    two.version = 2;
    const auto a = resolve_pass(one, AppliedFilter::create("concat.glow"), 0.0);
    const auto b = resolve_pass(two, AppliedFilter::create("concat.glow"), 0.0);
    check(a->key != b->key, "a version bump changes the key");
}

// The pipeline key is bound to the pack's schema, not an instance's values, so
// the resolver's per-pack key memo (computed once, reused across frames) is
// valid: two instances of the same pack at different knob settings share a key
// while their resolved values differ.
void test_the_cache_key_is_schema_bound_not_value_bound()
{
    const Pack pack = filter_pack();
    AppliedFilter quiet = AppliedFilter::create("concat.glow");
    quiet.params["amount"] = 0.1;
    AppliedFilter loud = AppliedFilter::create("concat.glow");
    loud.params["amount"] = 0.9;
    const auto a = resolve_pass(pack, quiet, 0.0);
    const auto b = resolve_pass(pack, loud, 0.0);
    check(a.has_value() && b.has_value(), "both instances resolve");
    check(a->key == b->key, "the key is the schema, not the values (the memo is valid)");
    check(a->values.at("amount") != b->values.at("amount"),
          "the resolved values still differ per instance");
}

void test_a_curve_and_a_wheel_lay_out_like_concat()
{
    Parameter lift;
    lift.key = "lift";
    lift.label = "Lift";
    lift.type = ParamType::Wheel;
    lift.min = -1.0;
    lift.max = 1.0;
    lift.default_value = 0.0;
    Parameter luma;
    luma.key = "luma";
    luma.label = "Luma";
    luma.type = ParamType::Curve;

    Pack pack;
    pack.id = "concat.grade";
    pack.name = "Grade";
    pack.kind = Kind::Effect;
    pack.parameters = { lift, luma };
    const auto pass = resolve_pass(pack, AppliedFilter::create("concat.grade"), 0.0);
    check(pass->params.size() == 144, "a wheel then a curve span 144");
    // The wheel: x and y default to the origin, the master to the default.
    check(read_float(pass->params, 0) == 0.0f, "wheel x at the origin");
    check(read_float(pass->params, 4) == 0.0f, "wheel y at the origin");
    check(read_float(pass->params, 8) == 0.0f, "wheel master at the default");
    // The curve: two default points across the square, one slope each.
    check(read_float(pass->params, 16) == 0.0f, "curve point 0 x");
    check(read_float(pass->params, 20) == 0.0f, "curve point 0 y");
    check(read_float(pass->params, 24) == 1.0f, "curve point 0 slope");
    check(read_float(pass->params, 28) == 2.0f, "curve point 0 count");
    check(read_float(pass->params, 32) == 1.0f, "curve point 1 x");
    check(read_float(pass->params, 36) == 1.0f, "curve point 1 y");
}

void test_a_color_and_a_point_lay_out()
{
    Parameter tint;
    tint.key = "tint";
    tint.label = "Tint";
    tint.type = ParamType::Color;
    // 0xff0000ff: opaque red, packed as the document stores a colour.
    tint.default_value = 4278190335.0;
    Parameter origin;
    origin.key = "origin";
    origin.label = "Origin";
    origin.type = ParamType::Point;

    Pack pack;
    pack.id = "concat.grade";
    pack.name = "Grade";
    pack.kind = Kind::Effect;
    pack.parameters = { tint, origin };
    const auto pass = resolve_pass(pack, AppliedFilter::create("concat.grade"), 0.0);
    check(pass->params.size() == 32, "a colour then a point span 32");
    check(read_float(pass->params, 0) == 1.0f, "colour red");
    check(read_float(pass->params, 4) == 0.0f, "colour green");
    check(read_float(pass->params, 8) == 0.0f, "colour blue");
    check(read_float(pass->params, 12) == 1.0f, "colour alpha");
    check(read_float(pass->params, 16) == 0.5f, "point x centres");
    check(read_float(pass->params, 20) == 0.5f, "point y centres");
}

// Every declared parameter is a field carrying its GLSL type, byte offset and
// size, in declaration order - so the engine can declare a uniform block that
// matches the uploaded bytes.
void test_fields_declare_each_parameter_its_type_offset_and_size()
{
    Parameter amount;
    amount.key = "amount";
    amount.label = "Amount";
    amount.default_value = 0.25;
    Parameter origin;
    origin.key = "origin";
    origin.label = "Origin";
    origin.type = ParamType::Point;
    Parameter lift;
    lift.key = "lift";
    lift.label = "Lift";
    lift.type = ParamType::Wheel;
    Parameter luma;
    luma.key = "luma";
    luma.label = "Luma";
    luma.type = ParamType::Curve;
    Parameter tint;
    tint.key = "tint";
    tint.label = "Tint";
    tint.type = ParamType::Color;
    Parameter steps;
    steps.key = "steps";
    steps.label = "Steps";
    steps.type = ParamType::Int;

    Pack pack;
    pack.id = "concat.grade";
    pack.name = "Grade";
    pack.kind = Kind::Effect;
    pack.parameters = { amount, origin, lift, luma, tint, steps };
    const auto pass = resolve_pass(pack, AppliedFilter::create("concat.grade"), 0.0);
    check(pass.has_value(), "the pack resolves");
    check(pass->fields.size() == 6, "every declared parameter is a field");

    const auto &f = pass->fields;
    check(f[0].name == "amount" && f[0].type == "float" && f[0].offset == 0 && f[0].size == 4,
          "amount: float at 0, size 4");
    check(f[1].name == "origin" && f[1].type == "vec2" && f[1].offset == 8 && f[1].size == 8,
          "origin: vec2 at 8 (aligned past the scalar), size 8");
    check(f[2].name == "lift" && f[2].type == "vec3" && f[2].offset == 16 && f[2].size == 12,
          "lift: vec3 at 16, size 12");
    check(f[3].name == "luma" && f[3].type == "vec4[8]" && f[3].offset == 32 && f[3].size == 128,
          "luma: vec4[8] at 32 (aligned past the wheel), size 128");
    check(f[4].name == "tint" && f[4].type == "vec4" && f[4].offset == 160 && f[4].size == 16,
          "tint: vec4 at 160, size 16");
    check(f[5].name == "steps" && f[5].type == "float" && f[5].offset == 176 && f[5].size == 4,
          "steps: an int stored as a float at 176, size 4");
    check(pass->params.size() == 192, "the block spans 192, padded to 16");
}

// A field's offset is where its value's bytes actually land: reading a value
// back at the declared offset gives the resolved value, for a scalar and for
// a compound knob's components alike. This is the check that would have
// caught the erased-layout gap.
void test_field_offsets_agree_with_where_the_values_land()
{
    Parameter amount;
    amount.key = "amount";
    amount.label = "Amount";
    amount.default_value = 0.5;
    Parameter origin;
    origin.key = "origin";
    origin.label = "Origin";
    origin.type = ParamType::Point;

    Pack pack;
    pack.id = "concat.grade";
    pack.name = "Grade";
    pack.kind = Kind::Effect;
    pack.parameters = { amount, origin };

    AppliedFilter instance = AppliedFilter::create("concat.grade");
    instance.params["amount"] = 0.25;
    instance.params["origin.x"] = 0.3;
    instance.params["origin.y"] = 0.7;
    const auto pass = resolve_pass(pack, instance, 0.0);
    check(pass.has_value(), "the pack resolves");

    const genesis::core::ParamField *amount_field = field_of(*pass, "amount");
    const genesis::core::ParamField *origin_field = field_of(*pass, "origin");
    check(amount_field != nullptr && origin_field != nullptr, "both parameters have a field");
    if (amount_field == nullptr || origin_field == nullptr) {
        return;
    }
    check(read_float(pass->params, amount_field->offset)
                  == static_cast<float>(pass->values.at("amount")),
          "the scalar's bytes sit at its field's offset");
    check(read_float(pass->params, origin_field->offset)
                  == static_cast<float>(pass->values.at("origin.x")),
          "origin.x sits at its field's offset");
    check(read_float(pass->params, origin_field->offset + 4)
                  == static_cast<float>(pass->values.at("origin.y")),
          "origin.y sits four bytes into its field");
    check(pass->values.at("amount") == 0.25, "the resolved amount is the ride");
    check(pass->values.at("origin.x") == 0.3 && pass->values.at("origin.y") == 0.7,
          "the resolved point is the ride");
}

// The expression evaluator: arithmetic over a couple of known names.
std::optional<double> eval(std::string_view source, std::string *error)
{
    return genesis::effects::expr::eval(
            source,
            [](std::string_view name) -> std::optional<double> {
                if (name == "amount")
                    return 0.5;
                if (name == "radius")
                    return 2.0;
                return std::nullopt;
            },
            error);
}

void test_the_expression_evaluator_computes()
{
    std::string error;
    const auto value = [&](std::string_view source) { return eval(source, &error); };
    check(value("radius * 2.0") == std::optional(4.0), "multiplication");
    check(value("clamp(amount * 2.0, 0.0, 1.0)") == std::optional(1.0), "clamp caps at 1");
    check(value("min(3.0, 2.0)") == std::optional(2.0), "min");
    check(value("max(3.0, 2.0)") == std::optional(3.0), "max");
    check(value("abs(-2.0)") == std::optional(2.0), "abs");
    check(value("floor(2.7)") == std::optional(2.0), "floor");
    check(value("ceil(2.1)") == std::optional(3.0), "ceil");
    check(value("round(2.5)") == std::optional(3.0), "round");
    check(value("1.0 + 2.0 * 3.0") == std::optional(7.0),
          "multiplication binds tighter than addition");
    check(value("(1.0 + 2.0) * 3.0") == std::optional(9.0), "parentheses override precedence");
    check(value("-amount") == std::optional(-0.5), "unary minus");
    check(value("1.0 - 2.0 - 3.0") == std::optional(-4.0), "subtraction is left-associative");
    check(value("16.0 / 2.0 / 2.0") == std::optional(4.0), "division is left-associative");
}

void test_the_expression_evaluator_reports_errors()
{
    std::string error;
    check(!eval("bogus * 2.0", &error).has_value(), "an unknown parameter fails");
    check(error.find("bogus") != std::string::npos, "and names it");

    check(!eval("foo(1.0)", &error).has_value(), "an unknown function fails");
    check(error.find("foo") != std::string::npos, "and names it");

    check(!eval("1.0 / (amount - amount)", &error).has_value(), "a division by zero fails");
    check(error.find("division by zero") != std::string::npos, "and says so");

    check(!eval("amount +", &error).has_value(), "a syntax error fails");
    check(!eval("", &error).has_value(), "an empty expression fails");
}

// The compiled evaluator must agree with the parser, value for value and error
// for error, on every expression the compiler accepts - including the failures
// that only surface at eval time (an unknown name, a division by zero). The
// parse-once path is what the resolver runs per frame, so this pins that the
// two cannot drift.
void test_a_compiled_expression_matches_the_source_eval()
{
    const auto lookup = [](std::string_view name) -> std::optional<double> {
        if (name == "amount")
            return 0.5;
        if (name == "radius")
            return 2.0;
        return std::nullopt;
    };
    const std::vector<std::string_view> exprs = {
        "radius * 2.0",
        "clamp(amount * 2.0, 0.0, 1.0)",
        "min(3.0, 2.0)",
        "max(3.0, 2.0)",
        "abs(-2.0)",
        "floor(2.7)",
        "ceil(2.1)",
        "round(2.5)",
        "1.0 + 2.0 * 3.0",
        "(1.0 + 2.0) * 3.0",
        "-amount",
        "1.0 - 2.0 - 3.0",
        "16.0 / 2.0 / 2.0",
        "bogus * 2.0", // unknown name, found at eval time
        "1.0 / (amount - amount)", // division by zero, found at eval time
    };
    for (const std::string_view expr : exprs) {
        std::string source_error;
        std::string compiled_error;
        const auto source = eval(expr, &source_error);
        const std::optional<genesis::effects::expr::Compiled> compiled =
                genesis::effects::expr::compile(expr, &compiled_error);
        check(compiled.has_value(), "the compiler accepts `" + std::string(expr) + "`");
        if (!compiled.has_value()) {
            continue;
        }
        const auto value = genesis::effects::expr::eval(*compiled, lookup, &compiled_error);
        check(value == source, "the compiled value matches for `" + std::string(expr) + "`");
        check(compiled_error == source_error,
              "the compiled error matches for `" + std::string(expr) + "`");
    }
}

void test_a_derived_uniform_resolves_to_a_field_and_bytes()
{
    Parameter strength;
    strength.key = "strength";
    strength.label = "Strength";
    strength.min = 10.0;
    strength.max = 100.0;
    strength.default_value = 50.0;

    Uniform normalized;
    normalized.key = "strength_norm";
    normalized.expr = "clamp(strength / 100.0, 0.0, 1.0)";

    Pack pack;
    pack.id = "concat.vignette";
    pack.name = "Vignette";
    pack.kind = Kind::Effect;
    pack.parameters = { strength };
    pack.uniforms = { normalized };

    const auto pass = resolve_pass(pack, AppliedFilter::create("concat.vignette"), 0.0);
    check(pass.has_value(), "a pack with a derived uniform resolves");
    check(pass->fields.size() == 2, "the parameter and the uniform are both fields");
    const genesis::core::ParamField *field = field_of(*pass, "strength_norm");
    check(field != nullptr, "the uniform has a field");
    if (field == nullptr) {
        return;
    }
    check(field->type == "float" && field->size == 4, "a uniform is a float field");
    check(field->offset == 4, "laid out after the strength parameter");
    check(read_float(pass->params, field->offset) == 0.5f, "50 percent clamps to 0.5");
}

void test_an_animated_parameter_drives_a_derived_uniform()
{
    Parameter strength;
    strength.key = "strength";
    strength.label = "Strength";
    strength.min = 0.0;
    strength.max = 100.0;
    strength.default_value = 50.0;
    strength.animate = true;

    Uniform normalized;
    normalized.key = "strength_norm";
    normalized.expr = "clamp(strength / 100.0, 0.0, 1.0)";

    Pack pack;
    pack.id = "concat.vignette";
    pack.name = "Vignette";
    pack.kind = Kind::Effect;
    pack.parameters = { strength };
    pack.uniforms = { normalized };

    AppliedFilter instance = AppliedFilter::create("concat.vignette");
    instance.set_key("strength", 0.0, 0.0, KeyEase::linear());
    instance.set_key("strength", 1.0, 100.0, KeyEase::linear());
    const auto at_zero = resolve_pass(pack, instance, 0.0);
    const auto at_one = resolve_pass(pack, instance, 1.0);
    check(at_zero.has_value() && at_one.has_value(), "the pack resolves");
    check(read_float(at_zero->params, 4) == 0.0f, "the uniform rides to 0");
    check(read_float(at_one->params, 4) == 1.0f, "and to 1");
}

void test_a_derived_uniform_division_by_zero_refuses_the_pass()
{
    Parameter amount;
    amount.key = "amount";
    amount.label = "Amount";
    amount.default_value = 0.5;

    Uniform broken;
    broken.key = "inverse";
    broken.expr = "1.0 / (amount - amount)";

    Pack pack;
    pack.id = "concat.demo";
    pack.name = "Demo";
    pack.kind = Kind::Effect;
    pack.parameters = { amount };
    pack.uniforms = { broken };

    check(!resolve_pass(pack, AppliedFilter::create("concat.demo"), 0.0).has_value(),
          "a uniform that divides by zero refuses the pass");
}

void test_a_cpu_fallback_resolves_defaults()
{
    const Pack pack = cpu_pack();
    const auto resolved =
            resolve_cpu_fallback(pack, AppliedFilter::create("concat.video-balance"), 0.0);
    check(resolved.has_value(), "a declared cpu fallback resolves");
    check(resolved->element == "videobalance", "the element name is carried");
    check(resolved->values.size() == 2, "two property values");
    if (resolved->values.size() != 2) {
        return;
    }
    check(resolved->values[0].key == "brightness" && resolved->values[0].value == 0.0,
          "brightness maps to its default");
    check(resolved->values[1].key == "contrast" && resolved->values[1].value == 1.0,
          "contrast maps to its default");
}

void test_a_cpu_fallback_resolves_a_set_instance()
{
    const Pack pack = cpu_pack();
    AppliedFilter instance = AppliedFilter::create("concat.video-balance");
    instance.params["brightness"] = 0.5;
    instance.params["contrast"] = 1.5;
    const auto resolved = resolve_cpu_fallback(pack, instance, 0.0);
    check(resolved.has_value(), "a cpu fallback resolves a set instance");
    check(resolved->values[0].value == 0.5, "brightness rides to 0.5");
    check(resolved->values[1].value == 1.5, "contrast rides to 1.5");
}

void test_a_pack_without_a_cpu_fallback_resolves_none()
{
    check(!resolve_cpu_fallback(filter_pack(), AppliedFilter::create("concat.glow"), 0.0)
                   .has_value(),
          "a pack with no cpu fallback resolves none");
}

void test_a_cpu_fallback_refuses_an_undeclared_instance()
{
    const Pack pack = cpu_pack();
    AppliedFilter stray = AppliedFilter::create("concat.video-balance");
    stray.params["bogus"] = 3.0;
    check(!resolve_cpu_fallback(pack, stray, 0.0).has_value(),
          "an undeclared parameter refuses the fallback");
}

void test_a_cpu_fallback_division_by_zero_refuses()
{
    Pack pack = cpu_pack();
    pack.cpu->properties[0].expr = "1.0 / (brightness - brightness)";
    check(!resolve_cpu_fallback(pack, AppliedFilter::create("concat.video-balance"), 0.0)
                   .has_value(),
          "a cpu expr dividing by zero refuses the fallback");
}

void test_an_audio_backend_resolves_defaults()
{
    const Pack pack = audio_pack();
    const auto resolved = resolve_audio(pack, AppliedFilter::create("genesis.loudness"), 0.0);
    check(resolved.has_value(), "a declared audio backend resolves");
    check(resolved->element == "rgvolume", "the element name is carried");
    check(resolved->values.size() == 1, "one property value");
    if (resolved->values.size() != 1) {
        return;
    }
    check(resolved->values[0].key == "pre-amp" && resolved->values[0].value == 0.0,
          "pre-amp maps to its default");
}

void test_an_audio_backend_resolves_a_set_instance()
{
    const Pack pack = audio_pack();
    AppliedFilter instance = AppliedFilter::create("genesis.loudness");
    instance.params["boost"] = 6.0;
    const auto resolved = resolve_audio(pack, instance, 0.0);
    check(resolved.has_value(), "an audio backend resolves a set instance");
    check(resolved->values[0].value == 6.0, "pre-amp rides to 6");
}

void test_a_pack_without_an_audio_backend_resolves_none()
{
    check(!resolve_audio(filter_pack(), AppliedFilter::create("concat.glow"), 0.0).has_value(),
          "a pack with no audio backend resolves none");
}

void test_an_audio_backend_refuses_an_undeclared_instance()
{
    const Pack pack = audio_pack();
    AppliedFilter stray = AppliedFilter::create("genesis.loudness");
    stray.params["bogus"] = 3.0;
    check(!resolve_audio(pack, stray, 0.0).has_value(),
          "an undeclared parameter refuses the audio backend");
}

void test_an_audio_backend_division_by_zero_refuses()
{
    Pack pack = audio_pack();
    pack.audio->properties[0].expr = "1.0 / (boost - boost)";
    check(!resolve_audio(pack, AppliedFilter::create("genesis.loudness"), 0.0).has_value(),
          "an audio expr dividing by zero refuses the backend");
}

} // namespace

int main()
{
    test_an_expressible_filter_resolves_every_field();
    test_an_animated_parameter_rides();
    test_an_undeclared_parameter_is_refused();
    test_a_named_intermediate_is_not_expressible();
    test_a_transition_resolves_progress_and_xfade();
    test_a_transition_and_an_effect_do_not_cross();
    test_intensity_is_the_reserved_mix();
    test_a_version_bump_changes_the_key();
    test_the_cache_key_is_schema_bound_not_value_bound();
    test_a_curve_and_a_wheel_lay_out_like_concat();
    test_a_color_and_a_point_lay_out();
    test_fields_declare_each_parameter_its_type_offset_and_size();
    test_field_offsets_agree_with_where_the_values_land();
    test_the_expression_evaluator_computes();
    test_the_expression_evaluator_reports_errors();
    test_a_compiled_expression_matches_the_source_eval();
    test_a_derived_uniform_resolves_to_a_field_and_bytes();
    test_an_animated_parameter_drives_a_derived_uniform();
    test_a_derived_uniform_division_by_zero_refuses_the_pass();
    test_a_cpu_fallback_resolves_defaults();
    test_a_cpu_fallback_resolves_a_set_instance();
    test_a_pack_without_a_cpu_fallback_resolves_none();
    test_a_cpu_fallback_refuses_an_undeclared_instance();
    test_a_cpu_fallback_division_by_zero_refuses();
    test_an_audio_backend_resolves_defaults();
    test_an_audio_backend_resolves_a_set_instance();
    test_a_pack_without_an_audio_backend_resolves_none();
    test_an_audio_backend_refuses_an_undeclared_instance();
    test_an_audio_backend_division_by_zero_refuses();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
