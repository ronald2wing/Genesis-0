// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The host-side control schema for a Pack: what an effect Pack declares.
//
// A Pack's `effect.toml` names it, lists its parameters, and carries one
// backend. This module holds the engine-free declaration only - identity,
// parameters, passes and the transition form. No GStreamer/GES type and no
// GLSL/WGSL source appears here; the shader body is the resolved render
// payload (`core::ShaderPass`) a later stage builds from this schema plus a
// clip's `AppliedFilter` values (docs/decisions/effect-execution.md §3).

namespace genesis::effects {

// Which catalogue a Pack belongs to. The vocabulary is the one video editors'
// users already know: an effect is something that *happens* to the picture, a
// filter is a colour look with an intensity, and audio is its own world.
enum class Kind {
    // A video effect: image in, image out.
    Effect,
    // A colour look: image in, image out, one intensity.
    Filter,
    // An audio effect: sound in, sound out.
    Audio,
    // A transition: two images and a progress, one image out.
    Transition,
    // A generator: no input, an image out.
    Generator,
};

// The name this kind carries in the manifest.
constexpr std::string_view kind_name(Kind kind)
{
    switch (kind) {
    case Kind::Effect:
        return "effect";
    case Kind::Filter:
        return "filter";
    case Kind::Audio:
        return "audio";
    case Kind::Transition:
        return "transition";
    case Kind::Generator:
        return "generator";
    }
    return "effect";
}

// The kind of that name, or nullopt for a name a Pack never spells.
constexpr std::optional<Kind> kind_from_name(std::string_view name)
{
    if (name == "effect") {
        return Kind::Effect;
    }
    if (name == "filter") {
        return Kind::Filter;
    }
    if (name == "audio") {
        return Kind::Audio;
    }
    if (name == "transition") {
        return Kind::Transition;
    }
    if (name == "generator") {
        return Kind::Generator;
    }
    return std::nullopt;
}

// The kinds of parameter. The document stores every kind as a number.
enum class ParamType {
    // A real number on a slider.
    Float,
    // A whole number on a stepper.
    Int,
    // A toggle, stored as 0 or 1.
    Bool,
    // One of `values`, chosen from a list.
    Enum,
    // A colour, stored as packed RGBA.
    Color,
    // A position on the picture, stored as `<key>.x` and `<key>.y` in the
    // 0..1 square.
    Point,
    // A colour wheel: a puck on a disc of hues, stored as `<key>.x` and
    // `<key>.y`, and a master `<key>.m`.
    Wheel,
    // A curve through up to MAX_CURVE_POINTS points of the unit square.
    Curve,
};

// The name this type carries in the manifest.
constexpr std::string_view name(ParamType type)
{
    switch (type) {
    case ParamType::Float:
        return "float";
    case ParamType::Int:
        return "int";
    case ParamType::Bool:
        return "bool";
    case ParamType::Enum:
        return "enum";
    case ParamType::Color:
        return "color";
    case ParamType::Point:
        return "point";
    case ParamType::Wheel:
        return "wheel";
    case ParamType::Curve:
        return "curve";
    }
    return "float";
}

// The type of that name, or nullopt for a name a Pack never spells.
constexpr std::optional<ParamType> from_name(std::string_view name)
{
    if (name == "float") {
        return ParamType::Float;
    }
    if (name == "int") {
        return ParamType::Int;
    }
    if (name == "bool") {
        return ParamType::Bool;
    }
    if (name == "enum") {
        return ParamType::Enum;
    }
    if (name == "color") {
        return ParamType::Color;
    }
    if (name == "point") {
        return ParamType::Point;
    }
    if (name == "wheel") {
        return ParamType::Wheel;
    }
    if (name == "curve") {
        return ParamType::Curve;
    }
    return std::nullopt;
}

// The most points a curve may have.
inline constexpr std::size_t MAX_CURVE_POINTS = 8;

// The most passes a Pack may draw before `effect`: each is a picture bound
// beside the layer, and a pass may bind sixteen.
inline constexpr std::size_t MAX_PASSES = 8;

// The uniform block's minimum size: a struct with nothing in it still needs
// a binding. The same floor the resolved payload uses
// (`core::ShaderPass::MIN_PARAMS`), so a Pack's span and its ShaderPass agree.
inline constexpr std::uint32_t MIN_PARAMS = 16;

// One declared parameter. The document stores every kind as a number, and
// `type` says how that number is interpreted by the control that shows it.
struct Parameter
{
    // The name the backend reads and the document stores.
    std::string key;
    // What the control is labelled.
    std::string label;
    // The subhead the control sits under in a grouped panel; empty is none.
    std::string group;
    // What kind of control, and how the number is interpreted.
    ParamType type = ParamType::Float;
    // Lowest value.
    double min = 0.0;
    // Highest value.
    double max = 1.0;
    // The value an untouched control means.
    double default_value = 0.0;
    // Slider increment; 0 means continuous.
    double step = 0.0;
    // Displayed after the number: "%", "dB", "K", "s".
    std::string unit;
    // Whether the control can carry keyframes.
    bool animate = false;
    // For `enum`: the values the document may hold.
    std::vector<double> values;
    // For `enum`: what each value is called, in `values` order.
    std::vector<std::string> labels;
    // The byte offset of this parameter's field in the Params uniform block,
    // as `lay_out` computes it in place of an offset table read back
    // from naga.
    std::uint32_t offset = 0;
};

// A derived uniform: a value the shader reads by name, computed from the
// declared parameters when the pass resolves, rather than stored on the clip.
// The `expr` is an expression over the pack's parameter names only (see
// Expr.h), e.g. `clamp(strength / 100.0, 0.0, 1.0)` hands the shader a ready
// 0..1 fraction so the body does not do the math itself.
struct Uniform
{
    // The name the shader reads, laid out as a float field in the uniform
    // block after the declared parameters.
    std::string key;
    // The expression, over the pack's parameter names only.
    std::string expr;
};

// One pass drawn before `effect`, into a picture of its own that every pass
// after it reads. `effect` is always the last pass.
struct Pass
{
    // The picture the pass draws, by name.
    std::string target;
    // How many times smaller than the layer the picture is, across and down:
    // two expressions over the knobs, each rounded down to a power of two.
    // Absent is the layer's own size.
    std::optional<std::array<std::string, 2>> shrink;
};

// One element property a CPU fallback feeds: the value the element's `key`
// property should hold, computed from the pack's parameters by `expr` - the
// same expression language a derived `Uniform` uses, so a manifest maps its
// knobs to a stock filter the same way it derives a uniform.
struct CpuProperty
{
    // The element property name, e.g. `brightness` on `frei0r-filter-brightness`.
    std::string key;
    // The expression, over the pack's parameter names only.
    std::string expr;
};

// The CPU fallback a Pack declares for the passes its GPU path cannot express:
// a stock GStreamer filter element (e.g. `frei0r-filter-brightness`, or an
// `avfilter-*` element where libav ships them) plus, per element property, the
// expression that supplies its value. The transition form's `xfade` is the
// same idea for transitions: the manifest names a built-in filter, the host
// maps its knobs, and the engine runs it where the shader cannot. Whether the element exists on a host is not the loader's business -
// a host that lacks it keeps the pass's R9 skip.
struct CpuFallback
{
    // The GStreamer element factory name.
    std::string element;
    // Property name -> expression over the pack's parameters, in declaration
    // order.
    std::vector<CpuProperty> properties;
};

// The audio backend a sound Pack declares: a stock GStreamer audio element
// (e.g. `rgvolume`, `audiodynamic`, `pitch`) plus, per element property, the
// expression that supplies its value - the sound-world sibling of `CpuFallback`,
// whose element/property shape it shares (`CpuProperty`). The element's
// existence is a host concern the adapter checks, not the loader's; a host that
// lacks it keeps the audio filter skipped rather than failing the build.
struct AudioBackend
{
    // The GStreamer element factory name.
    std::string element;
    // Property name -> expression over the pack's parameters, in declaration
    // order.
    std::vector<CpuProperty> properties;
};

// The two-input combine form of a transition Pack: the outgoing picture, the
// incoming one, and a progress into one.
struct TransitionForm
{
    // The shader file, beside the manifest.
    std::string entry;
    // A built-in FFmpeg `xfade` transition name for the GPU-less export
    // fallback; absent falls back to a plain dissolve.
    std::optional<std::string> xfade;
};

// A Pack's manifest, as host types. What the catalogue shows, what knobs the
// inspector draws, and the passes/transition the engine resolves - never the
// shader source itself.
struct Pack
{
    // Namespaced, `author.name`, lower-case. Written into project files, so it
    // is forever.
    std::string id;
    // What the catalogue card says.
    std::string name;
    // Which catalogue the Pack belongs to.
    Kind kind = Kind::Effect;
    // The shelf the card sits on, e.g. "Blur". Free text.
    std::string category;
    // The Pack's own version, bumped when its output changes.
    std::uint32_t version = 1;
    // The parameter the simple view shows as the one slider.
    std::optional<std::string> intensity;
    // Sort key within the category; ties break on name.
    std::int32_t order = 0;
    // A sentence for the tooltip.
    std::string description;
    // The body file path, relative to the manifest for a scoped [[effect]]
    // block, or empty for the legacy directory form (read from pack_dir/effect.glsl).
    std::string body;
    // The knobs, in the order the inspector shows them.
    std::vector<Parameter> parameters;
    // The derived uniforms, in the order the uniform block lays them out after
    // the parameters; each is one float field computed from the parameters.
    std::vector<Uniform> uniforms;
    // The passes drawn before `effect`, in order; empty is `effect` alone.
    std::vector<Pass> passes;
    // The two-input backend, when the Pack is a transition.
    std::optional<TransitionForm> transition;
    // The CPU fallback, when the Pack declares one: the element and property
    // mapping the engine runs when this Pack's passes cannot run on the GPU
    // path.
    std::optional<CpuFallback> cpu;
    // The audio backend, when the Pack is a sound effect: the element and
    // property mapping the engine runs on a clip's audio path. Only an audio
    // Pack may declare one.
    std::optional<AudioBackend> audio;

    // Memo for the resolver's pipeline cache key: the key is derived from
    // fields that never change after load, so it is computed once and reused
    // across every frame. Mutable because a const Pack is what the resolver
    // holds; the value is deterministic and written at the first resolve,
    // which the resolver's concurrency contract makes single-threaded.
    mutable std::shared_ptr<const std::string> cache_key_memo;
};

// Lays the declared parameters out - each field at
// the offset naga would have reported - writing each parameter's `offset`, and
// returns the padded span in bytes: at least MIN_PARAMS, rounded up to a
// 16-byte multiple.
std::uint32_t lay_out(std::vector<Parameter> &parameters);

// The padded span of the parameter block plus one float field per derived
// uniform, so `validate`'s bound and the resolver's buffer agree.
std::uint32_t span_with_uniforms(const Pack &pack);

// Validates a Pack for internal consistency and returns every problem found,
// empty for a sound manifest. See Pack.cpp for the rules.
std::vector<std::string> validate(const Pack &pack);

} // namespace genesis::effects
