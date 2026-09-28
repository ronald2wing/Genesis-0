// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-effects/src/manifest.rs inline tests,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "effects/PackLoader.h"

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

using namespace genesis::effects;

// The problem list flattened for a needle search.
std::string joined(const std::vector<LoadError> &problems)
{
    std::string out;
    for (const LoadError &problem : problems) {
        out += problem.where;
        out += ": ";
        out += problem.what;
        out += '\n';
    }
    return out;
}

void test_a_minimal_format_2_manifest_loads_clean()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.blur"
        name = "Blur"
        kind = "effect"

        [wgsl]
        entry = "effect.wgsl"
    )");
    check(result.problems.empty(), "a minimal format-2 manifest has no problems");
    check(result.pack.has_value(), "and it loads");
    if (!result.pack) {
        return;
    }
    check(result.pack->id == "concat.blur", "id");
    check(result.pack->name == "Blur", "name");
    check(result.pack->kind == Kind::Effect, "kind");
    check(result.pack->version == 1, "version defaults to 1");
    check(result.pack->parameters.empty(), "no parameters");
    check(result.pack->passes.empty(), "no passes");
}

void test_a_full_featured_pack_round_trips()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.demo"
        name = "Demo"
        kind = "effect"
        category = "Test"
        version = 3
        intensity = "amount"
        aliases = ["demo", "old.demo"]
        order = 7
        description = "Every parameter type at once."

        [[param]]
        key = "amount"
        label = "Amount"
        type = "float"
        min = 0
        max = 10
        default = 2.5
        step = 0.5
        unit = "%"
        animate = true

        [[param]]
        key = "count"
        label = "Count"
        type = "int"
        min = 1
        max = 9
        default = 3
        step = 2

        [[param]]
        key = "flip"
        label = "Flip"
        type = "bool"
        min = 0
        max = 1
        default = 0
        step = 1

        [[param]]
        key = "mode"
        label = "Mode"
        type = "enum"
        min = 1
        max = 3
        default = 2
        values = [1, 2, 3]
        labels = ["One", "Two", "Three"]

        [[param]]
        key = "tint"
        label = "Tint"
        type = "color"
        min = 0
        max = 4294967295
        default = 11616511

        [[param]]
        key = "center"
        label = "Center"
        type = "point"

        [[param]]
        key = "wheel"
        label = "Wheel"
        type = "wheel"
        min = -1
        max = 1
        default = 0
        step = 0.01
        animate = true

        [[param]]
        key = "curve"
        label = "Curve"
        type = "curve"

        [wgsl]
        entry = "effect.wgsl"

        [[wgsl.pass]]
        target = "across"
        shrink = ["radius / 3", "1"]

        [[wgsl.pass]]
        target = "down"
    )");
    check(result.problems.empty(), "a full-featured format-2 manifest has no problems");
    if (!result.pack) {
        return;
    }
    const Pack &pack = *result.pack;
    check(pack.id == "concat.demo", "id");
    check(pack.name == "Demo", "name");
    check(pack.kind == Kind::Effect, "kind");
    check(pack.category == "Test", "category");
    check(pack.version == 3, "version");
    check(pack.intensity.has_value() && *pack.intensity == "amount", "intensity");
    check(pack.aliases.size() == 2 && pack.aliases[0] == "demo" && pack.aliases[1] == "old.demo",
          "aliases");
    check(pack.order == 7, "order");
    check(pack.description == "Every parameter type at once.", "description");

    check(pack.parameters.size() == 8, "eight parameters");
    if (pack.parameters.size() != 8) {
        return;
    }

    const Parameter &amount = pack.parameters[0];
    check(amount.key == "amount" && amount.label == "Amount", "param 0 key/label");
    check(amount.type == ParamType::Float, "param 0 is a float");
    check(amount.min == 0.0 && amount.max == 10.0, "param 0 range");
    check(amount.default_value == 2.5, "param 0 default");
    check(amount.step == 0.5, "param 0 step");
    check(amount.unit == "%", "param 0 unit");
    check(amount.animate, "param 0 animates");

    const Parameter &count = pack.parameters[1];
    check(count.type == ParamType::Int, "param 1 is an int");
    check(count.min == 1.0 && count.max == 9.0 && count.default_value == 3.0 && count.step == 2.0,
          "param 1 range/default/step");

    const Parameter &flip = pack.parameters[2];
    check(flip.type == ParamType::Bool, "param 2 is a bool");
    check(flip.default_value == 0.0, "param 2 default");

    const Parameter &mode = pack.parameters[3];
    check(mode.type == ParamType::Enum, "param 3 is an enum");
    check(mode.values.size() == 3 && mode.labels.size() == 3, "param 3 values/labels");
    check(mode.labels[1] == "Two", "param 3 second label");

    const Parameter &tint = pack.parameters[4];
    check(tint.type == ParamType::Color, "param 4 is a color");
    check(tint.default_value == 11616511.0, "param 4 packed default");

    check(pack.parameters[5].type == ParamType::Point, "param 5 is a point");
    check(pack.parameters[6].type == ParamType::Wheel, "param 6 is a wheel");
    check(pack.parameters[7].type == ParamType::Curve, "param 7 is a curve");

    check(pack.passes.size() == 2, "two passes");
    if (pack.passes.size() != 2) {
        return;
    }
    check(pack.passes[0].target == "across", "pass 0 target");
    check(pack.passes[0].shrink.has_value() && (*pack.passes[0].shrink)[0] == "radius / 3"
                  && (*pack.passes[0].shrink)[1] == "1",
          "pass 0 shrink pair");
    check(pack.passes[1].target == "down", "pass 1 target");
    check(!pack.passes[1].shrink.has_value(), "pass 1 has no shrink");
}

void test_a_transition_form_round_trips()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.dissolve"
        name = "Dissolve"
        kind = "transition"
        aliases = ["cross-fade"]

        [transition]
        entry = "effect.wgsl"
        xfade = "fade"
        fallback = "cross-fade"
    )");
    check(result.problems.empty(), "a transition manifest has no problems");
    if (!result.pack) {
        return;
    }
    check(result.pack->kind == Kind::Transition, "kind is transition");
    check(result.pack->transition.has_value(), "it carries a transition form");
    if (!result.pack->transition) {
        return;
    }
    check(result.pack->transition->entry == "effect.wgsl", "transition entry");
    check(result.pack->transition->xfade.has_value() && *result.pack->transition->xfade == "fade",
          "transition xfade");
    check(result.pack->transition->fallback.has_value()
                  && *result.pack->transition->fallback == "cross-fade",
          "transition fallback");
}

void test_an_unknown_format_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 3

        [effect]
        id = "concat.blur"
        name = "Blur"
        kind = "effect"

        [wgsl]
        entry = "effect.wgsl"
    )");
    check(!result.pack.has_value(), "an unknown format is not loadable");
    check(!result.problems.empty(), "and reports a problem");
    check(joined(result.problems).find("format 3") != std::string::npos,
          "the problem names the format");
}

void test_a_sound_chain_format_is_rejected()
{
    // Format 1 is a sound (FFmpeg) chain, which the host control schema does
    // not model: it is rejected rather than loaded as an empty picture Pack.
    const LoadResult result = load_pack(R"(
        [effect]
        id = "concat.bass"
        name = "Bass"
        kind = "audio"

        [ffmpeg]
        chain = "bass=g=6"
    )");
    check(!result.pack.has_value(), "a format-1 sound chain is not loadable");
    check(!result.problems.empty(), "and reports a problem");
    check(joined(result.problems).find("format") != std::string::npos,
          "the problem mentions the format");
}

void test_malformed_toml_is_rejected_with_a_where()
{
    // A broken table header: the parse fails before any Pack is built.
    const LoadResult result = load_pack("format = 2\n[effect\nid = \"concat.blur\"\n");
    check(!result.pack.has_value(), "malformed TOML is not loadable");
    check(!result.problems.empty(), "and reports a problem");
    check(!result.problems[0].where.empty(), "with a non-empty where");
    check(result.problems[0].where.find("line") != std::string::npos,
          "the where carries a line number");
}

void test_a_default_outside_its_range_is_rejected_by_validation()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.blur"
        name = "Blur"
        kind = "effect"

        [[param]]
        key = "radius"
        label = "Radius"
        min = 1
        max = 50
        default = 99
        unit = "px"

        [wgsl]
        entry = "effect.wgsl"
    )");
    check(!result.pack.has_value(), "a default outside its range is not loadable");
    check(!result.problems.empty(), "and reports a problem");
    check(joined(result.problems).find("outside") != std::string::npos,
          "the problem comes from the schema's validation");
}

void test_a_missing_file_is_rejected_cleanly()
{
    const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "genesis-no-such-pack" / "effect.toml";
    const LoadResult result = load_pack_file(path);
    check(!result.pack.has_value(), "a missing file is not loadable");
    check(!result.problems.empty(), "and reports a problem");
    check(!result.problems[0].where.empty(), "with a usable where");
    check(joined(result.problems).find("could not be opened") != std::string::npos,
          "explaining the file could not be opened");
}

void test_a_derived_uniform_round_trips()
{
    const LoadResult result = load_pack(R"toml(
        format = 2

        [effect]
        id = "concat.vignette"
        name = "Vignette"
        kind = "effect"

        [[param]]
        key = "strength"
        label = "Strength"
        min = 10
        max = 100
        default = 50

        [[uniform]]
        key = "strength_norm"
        expr = "clamp(strength / 100.0, 0.0, 1.0)"

        [wgsl]
        entry = "effect.wgsl"
    )toml");
    check(result.problems.empty(), "a derived uniform manifest has no problems");
    check(result.pack.has_value(), "and it loads");
    if (!result.pack) {
        return;
    }
    check(result.pack->uniforms.size() == 1, "one derived uniform");
    check(result.pack->uniforms[0].key == "strength_norm", "uniform key");
    check(result.pack->uniforms[0].expr == "clamp(strength / 100.0, 0.0, 1.0)",
          "uniform expression");
}

void test_a_uniform_referencing_an_unknown_parameter_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.vignette"
        name = "Vignette"
        kind = "effect"

        [[param]]
        key = "strength"
        label = "Strength"

        [[uniform]]
        key = "bogus_norm"
        expr = "bogus * 2.0"

        [wgsl]
        entry = "effect.wgsl"
    )");
    check(!result.pack.has_value(), "an unknown parameter in an expression is not loadable");
    check(joined(result.problems).find("bogus") != std::string::npos,
          "the problem names the unknown parameter");
}

void test_a_uniform_referencing_another_uniform_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.vignette"
        name = "Vignette"
        kind = "effect"

        [[param]]
        key = "strength"
        label = "Strength"

        [[uniform]]
        key = "half"
        expr = "strength / 2.0"

        [[uniform]]
        key = "twice_half"
        expr = "half * 2.0"

        [wgsl]
        entry = "effect.wgsl"
    )");
    check(!result.pack.has_value(), "a uniform reading another uniform is not loadable");
    check(joined(result.problems).find("half") != std::string::npos,
          "the problem names the uniform that was read");
}

void test_a_uniform_with_an_unknown_function_is_rejected()
{
    const LoadResult result = load_pack(R"toml(
        format = 2

        [effect]
        id = "concat.vignette"
        name = "Vignette"
        kind = "effect"

        [[param]]
        key = "strength"
        label = "Strength"

        [[uniform]]
        key = "norm"
        expr = "foo(strength)"

        [wgsl]
        entry = "effect.wgsl"
    )toml");
    check(!result.pack.has_value(), "an unknown function is not loadable");
    check(joined(result.problems).find("foo") != std::string::npos,
          "the problem names the unknown function");
}

void test_a_uniform_with_bad_syntax_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.vignette"
        name = "Vignette"
        kind = "effect"

        [[param]]
        key = "strength"
        label = "Strength"

        [[uniform]]
        key = "norm"
        expr = "strength +"

        [wgsl]
        entry = "effect.wgsl"
    )");
    check(!result.pack.has_value(), "a malformed expression is not loadable");
    check(!result.problems.empty(), "and reports a problem");
}

void test_a_uniform_colliding_with_a_parameter_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.vignette"
        name = "Vignette"
        kind = "effect"

        [[param]]
        key = "strength"
        label = "Strength"

        [[uniform]]
        key = "strength"
        expr = "strength / 2.0"

        [wgsl]
        entry = "effect.wgsl"
    )");
    check(!result.pack.has_value(), "a uniform under a parameter's name is not loadable");
    check(joined(result.problems).find("collides") != std::string::npos,
          "the problem reports the collision");
}

void test_a_cpu_fallback_round_trips()
{
    const LoadResult result = load_pack(R"toml(
        format = 2

        [effect]
        id = "concat.video-balance"
        name = "Video Balance"
        kind = "effect"

        [[param]]
        key = "brightness"
        label = "Brightness"
        min = -1
        max = 1
        default = 0

        [[param]]
        key = "contrast"
        label = "Contrast"
        min = 0
        max = 2
        default = 1

        [cpu]
        element = "videobalance"

        [[cpu.param]]
        key = "brightness"
        expr = "brightness"

        [[cpu.param]]
        key = "contrast"
        expr = "clamp(contrast, 0.0, 2.0)"
    )toml");
    check(result.problems.empty(), "a cpu-fallback manifest has no problems");
    check(result.pack.has_value(), "and it loads");
    if (!result.pack) {
        return;
    }
    const CpuFallback &cpu = *result.pack->cpu;
    check(cpu.element == "videobalance", "the element name");
    check(cpu.properties.size() == 2, "two property mappings");
    if (cpu.properties.size() != 2) {
        return;
    }
    check(cpu.properties[0].key == "brightness" && cpu.properties[0].expr == "brightness",
          "property 0 key/expr");
    check(cpu.properties[1].key == "contrast"
                  && cpu.properties[1].expr == "clamp(contrast, 0.0, 2.0)",
          "property 1 key/expr");
}

void test_a_cpu_fallback_with_no_element_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.balance"
        name = "Balance"
        kind = "effect"

        [[param]]
        key = "brightness"
        label = "Brightness"

        [cpu]

        [[cpu.param]]
        key = "brightness"
        expr = "brightness"
    )");
    check(!result.pack.has_value(), "a cpu fallback without an element is not loadable");
    check(joined(result.problems).find("missing") != std::string::npos,
          "the problem names the missing element");
}

void test_a_cpu_fallback_referencing_an_unknown_parameter_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.balance"
        name = "Balance"
        kind = "effect"

        [[param]]
        key = "brightness"
        label = "Brightness"

        [cpu]
        element = "videobalance"

        [[cpu.param]]
        key = "brightness"
        expr = "bogus"
    )");
    check(!result.pack.has_value(), "a cpu expr naming an unknown parameter is not loadable");
    check(joined(result.problems).find("bogus") != std::string::npos,
          "the problem names the unknown parameter");
}

void test_a_cpu_fallback_with_a_duplicate_property_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.balance"
        name = "Balance"
        kind = "effect"

        [[param]]
        key = "brightness"
        label = "Brightness"

        [cpu]
        element = "videobalance"

        [[cpu.param]]
        key = "brightness"
        expr = "brightness"

        [[cpu.param]]
        key = "brightness"
        expr = "brightness + 1"
    )");
    check(!result.pack.has_value(), "a cpu property set twice is not loadable");
    check(joined(result.problems).find("set twice") != std::string::npos,
          "the problem reports the duplicate property");
}

void test_an_audio_backend_round_trips()
{
    const LoadResult result = load_pack(R"toml(
        format = 2

        [effect]
        id = "genesis.loudness"
        name = "Loudness"
        kind = "audio"
        category = "Audio"

        [[param]]
        key = "boost"
        label = "Boost"
        min = -60
        max = 60
        default = 0

        [[param]]
        key = "album"
        label = "Album Mode"
        type = "bool"
        min = 0
        max = 1
        default = 1

        [audio]
        element = "rgvolume"

        [[audio.param]]
        key = "pre-amp"
        expr = "boost"

        [[audio.param]]
        key = "album-mode"
        expr = "album"
    )toml");
    check(result.problems.empty(), "an audio manifest has no problems");
    check(result.pack.has_value(), "and it loads");
    if (!result.pack) {
        return;
    }
    check(result.pack->kind == Kind::Audio, "kind is audio");
    check(result.pack->audio.has_value(), "it carries an audio backend");
    if (!result.pack->audio) {
        return;
    }
    check(result.pack->audio->element == "rgvolume", "the element name");
    check(result.pack->audio->properties.size() == 2, "two property mappings");
    if (result.pack->audio->properties.size() != 2) {
        return;
    }
    check(result.pack->audio->properties[0].key == "pre-amp"
                  && result.pack->audio->properties[0].expr == "boost",
          "property 0 key/expr");
    check(result.pack->audio->properties[1].key == "album-mode"
                  && result.pack->audio->properties[1].expr == "album",
          "property 1 key/expr");
}

void test_an_audio_pack_without_a_backend_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "genesis.loudness"
        name = "Loudness"
        kind = "audio"
    )");
    check(!result.pack.has_value(), "an audio pack without a backend is not loadable");
    check(joined(result.problems).find("needs an [audio] table") != std::string::npos,
          "the problem asks for the backend");
}

void test_an_audio_backend_on_a_non_audio_pack_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "concat.blur"
        name = "Blur"
        kind = "effect"

        [wgsl]
        entry = "effect.wgsl"

        [audio]
        element = "rgvolume"

        [[audio.param]]
        key = "pre-amp"
        expr = "1"
    )");
    check(!result.pack.has_value(), "an [audio] table on an effect is not loadable");
    check(joined(result.problems).find("only for an audio package") != std::string::npos,
          "the problem reports the misplaced backend");
}

void test_an_audio_backend_with_no_element_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "genesis.loudness"
        name = "Loudness"
        kind = "audio"

        [audio]

        [[audio.param]]
        key = "pre-amp"
        expr = "1"
    )");
    check(!result.pack.has_value(), "an audio backend without an element is not loadable");
    check(joined(result.problems).find("missing") != std::string::npos,
          "the problem names the missing element");
}

void test_an_audio_backend_referencing_an_unknown_parameter_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "genesis.loudness"
        name = "Loudness"
        kind = "audio"

        [[param]]
        key = "boost"
        label = "Boost"

        [audio]
        element = "rgvolume"

        [[audio.param]]
        key = "pre-amp"
        expr = "bogus"
    )");
    check(!result.pack.has_value(), "an audio expr naming an unknown parameter is not loadable");
    check(joined(result.problems).find("bogus") != std::string::npos,
          "the problem names the unknown parameter");
}

void test_an_audio_backend_with_a_duplicate_property_is_rejected()
{
    const LoadResult result = load_pack(R"(
        format = 2

        [effect]
        id = "genesis.loudness"
        name = "Loudness"
        kind = "audio"

        [[param]]
        key = "boost"
        label = "Boost"

        [audio]
        element = "rgvolume"

        [[audio.param]]
        key = "pre-amp"
        expr = "boost"

        [[audio.param]]
        key = "pre-amp"
        expr = "boost + 1"
    )");
    check(!result.pack.has_value(), "an audio property set twice is not loadable");
    check(joined(result.problems).find("set twice") != std::string::npos,
          "the problem reports the duplicate property");
}

} // namespace

int main()
{
    test_a_minimal_format_2_manifest_loads_clean();
    test_a_full_featured_pack_round_trips();
    test_a_transition_form_round_trips();
    test_an_unknown_format_is_rejected();
    test_a_sound_chain_format_is_rejected();
    test_malformed_toml_is_rejected_with_a_where();
    test_a_default_outside_its_range_is_rejected_by_validation();
    test_a_missing_file_is_rejected_cleanly();
    test_a_derived_uniform_round_trips();
    test_a_uniform_referencing_an_unknown_parameter_is_rejected();
    test_a_uniform_referencing_another_uniform_is_rejected();
    test_a_uniform_with_an_unknown_function_is_rejected();
    test_a_uniform_with_bad_syntax_is_rejected();
    test_a_uniform_colliding_with_a_parameter_is_rejected();
    test_a_cpu_fallback_round_trips();
    test_a_cpu_fallback_with_no_element_is_rejected();
    test_a_cpu_fallback_referencing_an_unknown_parameter_is_rejected();
    test_a_cpu_fallback_with_a_duplicate_property_is_rejected();
    test_an_audio_backend_round_trips();
    test_an_audio_pack_without_a_backend_is_rejected();
    test_an_audio_backend_on_a_non_audio_pack_is_rejected();
    test_an_audio_backend_with_no_element_is_rejected();
    test_an_audio_backend_referencing_an_unknown_parameter_is_rejected();
    test_an_audio_backend_with_a_duplicate_property_is_rejected();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
