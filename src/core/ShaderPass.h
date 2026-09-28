// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace genesis::core {

// A 3D look-up table: size texels a side, RGBA floats, red fastest, then
// green, then blue - the order a .cube file lists its rows in and the layout a
// texture_3d is uploaded from. A colour is looked up by its own components: the
// table maps every input colour to an output one. Floats, not bytes: a table
// read in log spreads seventeen stops over its range, and eight bits of that
// would be steps a fifteenth of a stop apart - bands in any sky.
struct Lut
{
    // A hash of the contents, so a renderer can cache the upload.
    std::uint64_t id = 0;
    // Texels a side, at least 2.
    std::uint32_t size = 0;
    // size^3 * 4 floats, alpha 1.
    std::shared_ptr<const std::vector<float>> rgba;

    // A table from size^3 RGB triples, red fastest, as the table gives them -
    // usually 0..=1, though a table made for log or for HDR may reach past
    // either end. A table of the wrong length, or with a value that is not a
    // number, is nullopt.
    static std::optional<Lut> from_rgb(std::uint32_t size, const std::vector<float> &rgb);

    // The table that changes nothing: what a pass without one binds.
    static Lut identity(std::uint32_t size);

    // The colour the table maps rgb to, trilinearly interpolated - the same
    // arithmetic the GPU's sampler does, for a CPU that wants it.
    std::array<float, 3> sample(std::array<float, 3> rgb) const;

    bool operator==(const Lut &other) const { return id == other.id && size == other.size; }
};

// A title's words, baked into one grayscale map the size of its canvas: each
// pixel holds the normalized order (0 first, 1 last) of the word painted there,
// 0 wherever no word was - background is moot since it is transparent
// regardless, so it is left "already revealed" rather than given a value that
// would mean something if it were ever visible.
//
// This is how a per-word reveal effect works without the renderer or the shader
// knowing what a "word" is: the layout that already happens to paint a title is
// baked once into a texture, exactly as a colour look-up table bakes a grade,
// and a shader reads it back with one comparison against progress.
struct RevealMap
{
    // A hash of the contents, so a renderer can cache the upload.
    std::uint64_t id = 0;
    // The canvas width this map was baked at.
    std::uint32_t width = 0;
    // The canvas height this map was baked at.
    std::uint32_t height = 0;
    // width * height bytes, one per pixel, red channel only.
    std::shared_ptr<const std::vector<std::uint8_t>> gray;

    // A map width by height, rects each (x, y, width, height) in canvas pixels
    // and in the order the words should reveal - reading order is what a caller
    // normally means. A pixel outside every rect stays revealed from the start;
    // one inside more than one keeps the later word's order, though words in
    // practice never overlap.
    static RevealMap from_rects(std::uint32_t width, std::uint32_t height,
                                const std::vector<std::array<std::int32_t, 4>> &rects);

    // The map that reveals everything from the first frame: what a pass without
    // one binds, and a two-by-two texture, so the identity is almost free to
    // keep resident.
    static RevealMap identity();

    bool operator==(const RevealMap &other) const
    {
        return id == other.id && width == other.width && height == other.height;
    }
};

// One pass of a package drawn in several, before its last: which entry point of
// the module draws it, and how much smaller than the layer the picture it draws
// is.
struct Stage
{
    // The fragment entry point that draws it.
    std::string entry;
    // How many times smaller than the layer the picture is, across and down: a
    // power of two, 1 the layer's own size.
    std::array<std::uint32_t, 2> shrink{ 1, 1 };

    // The size of the picture the stage draws over a layer width by height: the
    // layer's divided by the shrink, rounded up, never nothing.
    std::pair<std::uint32_t, std::uint32_t> size(std::uint32_t width, std::uint32_t height) const;

    bool operator==(const Stage &other) const
    {
        return entry == other.entry && shrink == other.shrink;
    }
};

// One field of a pass's uniform block: what the shader must declare, and
// where the bytes in `params` carry it. Kept beside the bytes because bytes
// alone cannot be typed - a float and a default-valued vec2 of the same width
// are indistinguishable - and a declaration that does not match the upload is
// worse than none.
struct ParamField
{
    // The parameter's key, as the declaration names it.
    std::string name;
    // The manifest type's GLSL spelling - float, vec2, vec3, vec4 or
    // vec4[8] - which the shader's declaration must match to the upload.
    std::string type;
    // Where the field's first byte sits in `params`.
    std::uint32_t offset = 0;
    // The field's size in bytes, padding to its next neighbour not counted.
    std::uint32_t size = 0;

    bool operator==(const ParamField &) const = default;
};

// One fragment pass over a layer.
struct ShaderPass
{
    // The uniform buffer's minimum size: a struct with nothing in it still
    // needs a binding.
    static constexpr std::size_t MIN_PARAMS = 16;

    // The package's id, author.name: what a renderer without a shader stage
    // keys its own kernel for the package by.
    std::string package;
    // What to cache the compiled pipeline under: the package's id and version
    // and a fingerprint of its source, so a package that changes its shader
    // gets a new pipeline - version bump or not - and one that only changes its
    // knobs keeps the old.
    std::string key;
    // The complete WGSL module: the host's prelude with the package's body,
    // declaring fn effect(uv: vec2<f32>) -> vec4<f32>.
    std::shared_ptr<const std::string> source;
    // The Params uniform, laid out to the struct's offsets. Sixteen bytes at
    // least, so an empty struct still has a buffer.
    std::vector<std::uint8_t> params;
    // The same values by the manifest's keys, every declared parameter present,
    // resolved for the frame: what params was written from.
    std::map<std::string, double> values;
    // The layout of `params`: one field per declared parameter, naming its GLSL
    // type and where its bytes sit, so the engine can declare a uniform block
    // that matches the upload.
    std::vector<ParamField> fields;
    // How much of the result to keep over the untouched layer, 0..=1. A look at
    // half strength is half the look; an effect is always one.
    float intensity = 1.0F;
    // The package's look-up table, bound as a 3D texture the shader's lut()
    // samples; nullopt binds the identity so the call is harmless.
    std::optional<Lut> lut;
    // A title's per-word reveal order, bound as a 2D texture the shader's
    // reveal_order() samples; nullopt binds a map that reveals everything, so
    // the call is harmless for a pass over anything that is not a title.
    std::optional<RevealMap> reveal_map;
    // The passes drawn before the last, in order, each into a picture of its
    // own that every pass after it may read: empty for a package drawn in one
    // pass, which is most of them. The last pass is fs_main.
    std::vector<Stage> stages;

    bool operator==(const ShaderPass &other) const
    {
        return package == other.package && key == other.key && params == other.params
                && values == other.values && fields == other.fields && intensity == other.intensity
                && stages == other.stages;
    }
};

// One two-input transition combine, as the compositor runs it.
//
// Where a ShaderPass changes one layer, a transition reads two - the outgoing
// picture and the incoming one - and a progress from 0 to 1, and writes the
// single picture between them. It is resolved from a transition package and the
// cut's timing by the effect catalogue and carried to the renderer as data,
// exactly like a pass: the renderer compiles and caches the pipeline by key and
// pours params into the shader's uniform buffer as the catalogue laid them out.
struct TransitionPass
{
    // What to cache the compiled pipeline under: the package's id and version,
    // the same rule a ShaderPass follows.
    std::string key;
    // The complete WGSL module: the host's transition prelude with the
    // package's body, declaring
    // fn transition(uv: vec2<f32>, progress: f32) -> vec4<f32>.
    std::shared_ptr<const std::string> source;
    // The Params uniform, laid out to the struct's offsets. Sixteen bytes at
    // least, so an empty struct still has a buffer.
    std::vector<std::uint8_t> params;
    // How far through the cut this frame is, 0..=1: 0 is all the outgoing
    // picture, 1 all the incoming one.
    float progress = 0.0F;
    // The package's look-up table, bound like a pass's so the shared grading
    // helpers work; nullopt binds the identity.
    std::optional<Lut> lut;
    // The shape a compositor that runs no shaders draws instead: the FFmpeg
    // xfade name the package's manifest declares, which the CPU reference knows
    // how to draw in plain arithmetic. nullopt, or a name it does not know, and
    // the compositor declines the combine, leaving the dissolve the incoming
    // layer already carries.
    std::optional<std::string> xfade;

    bool operator==(const TransitionPass &other) const
    {
        return key == other.key && params == other.params && progress == other.progress
                && xfade == other.xfade;
    }
};

} // namespace genesis::core
