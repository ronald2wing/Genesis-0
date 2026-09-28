// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "core/ShaderPass.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace genesis::core {

namespace {

// FNV-1a, so a table's id needs no dependency and is the same on every machine.
std::uint64_t fnv64(const std::uint8_t *bytes, std::size_t count)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < count; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= 0x00000100000001b3ULL;
    }
    return hash;
}

std::uint64_t fnv64(const std::vector<std::uint8_t> &bytes)
{
    return fnv64(bytes.data(), bytes.size());
}

} // namespace

std::optional<Lut> Lut::from_rgb(std::uint32_t size, const std::vector<float> &rgb)
{
    if (size < 2) {
        return std::nullopt;
    }
    // size^3, checked so a huge size cannot wrap.
    std::uint64_t texels = 1;
    for (int i = 0; i < 3; ++i) {
        texels *= size;
        if (texels > (1ULL << 40)) {
            return std::nullopt;
        }
    }
    if (rgb.size() != texels * 3) {
        return std::nullopt;
    }
    for (float value : rgb) {
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
    }

    auto rgba = std::make_shared<std::vector<float>>();
    rgba->reserve(texels * 4);
    for (std::size_t i = 0; i < rgb.size(); i += 3) {
        rgba->push_back(rgb[i]);
        rgba->push_back(rgb[i + 1]);
        rgba->push_back(rgb[i + 2]);
        rgba->push_back(1.0F);
    }

    std::vector<std::uint8_t> bytes(rgba->size() * sizeof(float));
    std::memcpy(bytes.data(), rgba->data(), bytes.size());
    const std::uint64_t id = fnv64(bytes) ^ static_cast<std::uint64_t>(size);

    Lut lut;
    lut.id = id;
    lut.size = size;
    lut.rgba = std::move(rgba);
    return lut;
}

Lut Lut::identity(std::uint32_t size)
{
    size = std::max(size, 2U);
    const float step = 1.0F / static_cast<float>(size - 1);
    std::vector<float> rgb;
    rgb.reserve(static_cast<std::size_t>(size) * size * size * 3);
    for (std::uint32_t b = 0; b < size; ++b) {
        for (std::uint32_t g = 0; g < size; ++g) {
            for (std::uint32_t r = 0; r < size; ++r) {
                rgb.push_back(static_cast<float>(r) * step);
                rgb.push_back(static_cast<float>(g) * step);
                rgb.push_back(static_cast<float>(b) * step);
            }
        }
    }
    return *from_rgb(size, rgb);
}

std::array<float, 3> Lut::sample(std::array<float, 3> rgb) const
{
    const std::size_t n = size;
    const float last = static_cast<float>(n - 1);
    const auto at = [n, last](float c) {
        const float x = std::min(std::max(c, 0.0F), 1.0F) * last;
        const std::size_t i = std::min(static_cast<std::size_t>(std::floor(x)), n - 2);
        return std::pair<std::size_t, float>{ i, x - static_cast<float>(i) };
    };
    const auto [ri, rf] = at(rgb[0]);
    const auto [gi, gf] = at(rgb[1]);
    const auto [bi, bf] = at(rgb[2]);

    const auto texel = [this, n](std::size_t r, std::size_t g, std::size_t b) {
        const std::size_t o = ((((b * n) + g) * n) + r) * 4;
        return std::array<float, 3>{ (*rgba)[o], (*rgba)[o + 1], (*rgba)[o + 2] };
    };
    const auto lerp = [](std::array<float, 3> a, std::array<float, 3> b, float t) {
        return std::array<float, 3>{ a[0] + ((b[0] - a[0]) * t), a[1] + ((b[1] - a[1]) * t),
                                     a[2] + ((b[2] - a[2]) * t) };
    };

    const auto c00 = lerp(texel(ri, gi, bi), texel(ri + 1, gi, bi), rf);
    const auto c10 = lerp(texel(ri, gi + 1, bi), texel(ri + 1, gi + 1, bi), rf);
    const auto c01 = lerp(texel(ri, gi, bi + 1), texel(ri + 1, gi, bi + 1), rf);
    const auto c11 = lerp(texel(ri, gi + 1, bi + 1), texel(ri + 1, gi + 1, bi + 1), rf);
    return lerp(lerp(c00, c10, gf), lerp(c01, c11, gf), bf);
}

RevealMap RevealMap::from_rects(std::uint32_t width, std::uint32_t height,
                                const std::vector<std::array<std::int32_t, 4>> &rects)
{
    auto gray = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(width) * height, 0);
    const float last =
            static_cast<float>(rects.empty() ? 1 : std::max<std::size_t>(rects.size() - 1, 1));

    for (std::size_t index = 0; index < rects.size(); ++index) {
        const auto &[x, y, w, h] = rects[index];
        const auto value =
                static_cast<std::uint8_t>(std::round((static_cast<float>(index) / last) * 255.0F));
        const auto clamp_axis = [](std::int32_t v, std::uint32_t limit) {
            return static_cast<std::uint32_t>(
                    std::min(std::max(v, 0), static_cast<std::int32_t>(limit)));
        };
        const std::uint32_t x0 = clamp_axis(x, width);
        const std::uint32_t y0 = clamp_axis(y, height);
        const std::uint32_t x1 = clamp_axis(x + static_cast<std::int32_t>(w), width);
        const std::uint32_t y1 = clamp_axis(y + static_cast<std::int32_t>(h), height);
        for (std::uint32_t row = y0; row < y1; ++row) {
            const std::size_t start = (static_cast<std::size_t>(row) * width) + x0;
            std::fill(gray->begin() + static_cast<std::ptrdiff_t>(start),
                      gray->begin() + static_cast<std::ptrdiff_t>(start + (x1 - x0)), value);
        }
    }

    RevealMap map;
    map.id = fnv64(*gray) ^ static_cast<std::uint64_t>(width)
            ^ (static_cast<std::uint64_t>(height) << 32);
    map.width = width;
    map.height = height;
    map.gray = std::move(gray);
    return map;
}

RevealMap RevealMap::identity()
{
    return from_rects(2, 2, { });
}

std::pair<std::uint32_t, std::uint32_t> Stage::size(std::uint32_t width, std::uint32_t height) const
{
    const std::uint32_t across = std::max(shrink[0], 1U);
    const std::uint32_t down = std::max(shrink[1], 1U);
    const auto div_ceil = [](std::uint32_t value, std::uint32_t divisor) {
        return std::max((value + divisor - 1) / divisor, 1U);
    };
    return { div_ceil(width, across), div_ceil(height, down) };
}

} // namespace genesis::core
