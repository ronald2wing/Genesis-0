// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <string>
#include <vector>

#include "../support/Checks.h"
#include "effects/Pack.h"

namespace {

using genesis::test::check;
using namespace genesis::effects;

// A well-formed filter Pack at its defaults, in the manifest's shape.
Pack good()
{
    Parameter radius;
    radius.key = "radius";
    radius.label = "Radius";
    radius.min = 1.0;
    radius.max = 50.0;
    radius.default_value = 10.0;
    radius.unit = "px";

    Pack pack;
    pack.id = "example.blur";
    pack.name = "Blur";
    pack.kind = Kind::Effect;
    pack.category = "Blur";
    pack.intensity = "radius";
    pack.parameters = { radius };
    return pack;
}

// A well-formed transition Pack.
Pack good_transition()
{
    TransitionForm transition;
    transition.entry = "effect.wgsl";
    transition.xfade = "fade";

    Pack pack;
    pack.id = "example.dissolve";
    pack.name = "Dissolve";
    pack.kind = Kind::Transition;
    pack.transition = transition;
    return pack;
}

// The problem list flattened for a needle search.
std::string joined(const std::vector<std::string> &problems)
{
    std::string out;
    for (const std::string &problem : problems) {
        out += problem;
        out += '\n';
    }
    return out;
}

// `pack` must be rejected with a reason mentioning `needle`.
void rejects(const Pack &pack, const std::string &needle)
{
    const std::vector<std::string> problems = validate(pack);
    check(!problems.empty(), "rejected: " + needle);
    check(joined(problems).find(needle) != std::string::npos, "reason mentions `" + needle + "`");
}

void test_a_well_formed_pack_validates_clean()
{
    const Pack pack = good();
    check(validate(pack).empty(), "a well-formed Pack has no problems");
    check(pack.version == 1, "version defaults to 1");
    check(pack.parameters[0].step == 0.0, "step defaults to continuous");
    check(pack.parameters[0].type == ParamType::Float, "a float by default");
}

void test_the_parameter_type_set_round_trips()
{
    const ParamType all[] = {
        ParamType::Float, ParamType::Int,   ParamType::Bool,  ParamType::Enum,
        ParamType::Color, ParamType::Point, ParamType::Wheel, ParamType::Curve
    };
    for (const ParamType type : all) {
        check(from_name(name(type)) == type, "a type round-trips its name");
    }
    check(name(ParamType::Float) == "float", "float's spelling");
    check(name(ParamType::Curve) == "curve", "curve's spelling");
    check(!from_name("bogus").has_value(), "an unknown name is nullopt");
}

void test_the_id_must_be_namespaced()
{
    Pack pack = good();
    pack.id = "blur";
    rejects(pack, "author.name");
    pack.id = "Example.Blur";
    rejects(pack, "author.name");
}

void test_the_name_must_not_be_empty()
{
    Pack pack = good();
    pack.name = "   ";
    rejects(pack, "name is empty");
}

void test_a_parameter_key_must_be_an_identifier()
{
    Pack pack = good();
    pack.parameters[0].key = "Not-Ident";
    rejects(pack, "lower-case letters");
    pack.parameters[0].key = "2x";
    rejects(pack, "lower-case letters");
}

void test_the_index_key_is_reserved()
{
    Pack pack = good();
    pack.parameters[0].key = "index";
    rejects(pack, "reserved");
}

void test_the_intensity_key_is_reserved()
{
    Pack pack = good();
    pack.parameters[0].key = "intensity";
    pack.intensity = "intensity";
    rejects(pack, "reserved");
}

void test_parameters_must_be_unique()
{
    Pack pack = good();
    pack.parameters.push_back(pack.parameters[0]);
    rejects(pack, "declared twice");
}

void test_a_parameter_needs_a_label()
{
    Pack pack = good();
    pack.parameters[0].label = "";
    rejects(pack, "no label");
}

void test_min_must_not_exceed_max()
{
    Pack pack = good();
    pack.parameters[0].min = 10.0;
    pack.parameters[0].max = 1.0;
    rejects(pack, "min is above max");
}

void test_a_default_lands_in_its_range()
{
    Pack pack = good();
    check(validate(pack).empty(), "an in-range default is clean");
    pack.parameters[0].default_value = 99.0;
    rejects(pack, "outside");
    pack.parameters[0].default_value = -1.0;
    rejects(pack, "outside");
}

void test_a_curve_cannot_be_animated()
{
    Pack pack = good();
    pack.parameters[0].type = ParamType::Curve;
    pack.parameters[0].animate = true;
    rejects(pack, "cannot be animated");
}

void test_an_enum_lists_values_and_labels()
{
    Pack pack = good();
    pack.parameters[0].type = ParamType::Enum;
    rejects(pack, "lists no values");
    pack.parameters[0].values = { 1.0, 2.0 };
    pack.parameters[0].labels = { "One" };
    rejects(pack, "labels");
}

void test_intensity_names_a_parameter()
{
    Pack pack = good();
    pack.intensity = "sigma";
    rejects(pack, "not a parameter");
}

void test_a_transition_needs_a_transition_form()
{
    const Pack pack = good_transition();
    check(validate(pack).empty(), "a well-formed transition is clean");
    Pack missing = pack;
    missing.transition.reset();
    rejects(missing, "needs a [transition]");
}

void test_a_non_transition_has_no_transition_form()
{
    Pack pack = good();
    pack.transition = good_transition().transition;
    rejects(pack, "only for a transition");
}

void test_a_transition_xfade_must_be_known()
{
    Pack pack = good_transition();
    pack.transition->xfade = "nonsense";
    rejects(pack, "not a known FFmpeg xfade name");
}

// `good()` with one pass of `target`.
Pack with_pass(const std::string &target)
{
    Pack pack = good();
    Pass pass;
    pass.target = target;
    pack.passes = { pass };
    return pack;
}

void test_a_pass_target_must_be_an_identifier()
{
    for (const char *target : { "Across", "2x", "_x", "a-b", "" }) {
        rejects(with_pass(target), "lower-case letter");
    }
}

void test_a_pass_target_must_not_be_taken()
{
    rejects(with_pass("effect"), "is taken");
    rejects(with_pass("main"), "is taken");
}

void test_pass_targets_must_be_unique()
{
    Pack pack = good();
    Pass pass;
    pass.target = "x";
    pack.passes = { pass, pass };
    rejects(pack, "drawn twice");
}

void test_a_pack_draws_at_most_eight_passes()
{
    Pack pack = good();
    for (int i = 0; i <= 8; ++i) {
        Pass pass;
        pass.target = "p" + std::to_string(i);
        pack.passes.push_back(pass);
    }
    rejects(pack, "may draw 8");
}

void test_the_parameter_block_stays_a_sane_size()
{
    Pack pack;
    pack.id = "example.huge";
    pack.name = "Huge";
    // 512 curves is 65536 bytes: past the 16 KiB bound.
    for (int i = 0; i < 512; ++i) {
        Parameter curve;
        curve.key = "c" + std::to_string(i);
        curve.label = "Curve";
        curve.type = ParamType::Curve;
        pack.parameters.push_back(curve);
    }
    rejects(pack, "parameter block");
}

void test_the_uniform_layout_matches_legacy()
{
    // Two scalars pack into four bytes each, the block padded to 16.
    Pack pack = good();
    Parameter amount;
    amount.key = "amount";
    amount.label = "Amount";
    amount.min = 0.0;
    amount.max = 1.0;
    amount.default_value = 0.5;
    pack.parameters.push_back(amount);
    check(validate(pack).empty(), "a two-parameter Pack is clean");
    check(lay_out(pack.parameters) == 16, "two scalars span 16 bytes");
    check(pack.parameters[0].offset == 0, "the first scalar at 0");
    check(pack.parameters[1].offset == 4, "the second scalar at 4");

    // A wheel (vec3) then a curve (array<vec4,8>): offsets 0 and 16, span 144.
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
    std::vector<Parameter> compound = { lift, luma };
    check(lay_out(compound) == 144, "vec3 then a 128-byte curve spans 144");
    check(compound[0].offset == 0, "the wheel at 0");
    check(compound[1].offset == 16, "the curve aligned to 16");
}

// `good()` with a one-property CPU fallback mapping its `radius` knob onto the
// element's `brightness` property.
Pack with_cpu(const std::string &element = "frei0r-filter-brightness",
              const std::string &expr = "radius / 50.0")
{
    Pack pack = good();
    CpuFallback cpu;
    cpu.element = element;
    cpu.properties = { { "brightness", expr } };
    pack.cpu = cpu;
    return pack;
}

void test_a_cpu_fallback_validates_clean()
{
    check(validate(with_cpu()).empty(), "a well-formed cpu fallback is clean");
}

void test_a_cpu_fallback_needs_an_element()
{
    rejects(with_cpu(""), "element is empty");
}

void test_a_cpu_property_key_must_not_be_blank()
{
    Pack pack = with_cpu();
    pack.cpu->properties[0].key = "  ";
    rejects(pack, "property key is empty");
}

void test_a_cpu_property_key_must_be_unique()
{
    Pack pack = with_cpu();
    pack.cpu->properties.push_back(pack.cpu->properties[0]);
    rejects(pack, "set twice");
}

void test_a_cpu_expr_must_reference_a_parameter()
{
    rejects(with_cpu("frei0r-filter-brightness", "bogus"), "bogus");
}

} // namespace

int main()
{
    test_a_well_formed_pack_validates_clean();
    test_the_parameter_type_set_round_trips();
    test_the_id_must_be_namespaced();
    test_the_name_must_not_be_empty();
    test_a_parameter_key_must_be_an_identifier();
    test_the_index_key_is_reserved();
    test_the_intensity_key_is_reserved();
    test_parameters_must_be_unique();
    test_a_parameter_needs_a_label();
    test_min_must_not_exceed_max();
    test_a_default_lands_in_its_range();
    test_a_curve_cannot_be_animated();
    test_an_enum_lists_values_and_labels();
    test_intensity_names_a_parameter();
    test_a_transition_needs_a_transition_form();
    test_a_non_transition_has_no_transition_form();
    test_a_transition_xfade_must_be_known();
    test_a_pass_target_must_be_an_identifier();
    test_a_pass_target_must_not_be_taken();
    test_pass_targets_must_be_unique();
    test_a_pack_draws_at_most_eight_passes();
    test_the_parameter_block_stays_a_sane_size();
    test_the_uniform_layout_matches_legacy();
    test_a_cpu_fallback_validates_clean();
    test_a_cpu_fallback_needs_an_element();
    test_a_cpu_property_key_must_not_be_blank();
    test_a_cpu_property_key_must_be_unique();
    test_a_cpu_expr_must_reference_a_parameter();

    return genesis::test::summary();
}
