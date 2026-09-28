// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-core/src/frame.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include "core/Frame.h"

#include <atomic>
#include <cstring>
#include <utility>

namespace genesis::core {

namespace {

// The next frame identity; see Frame::id().
std::atomic<std::uint64_t> g_next_id{ 1 };

std::uint64_t mint()
{
    return g_next_id.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

Frame::Frame(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> pixels,
             Depth depth, Signal signal)
    : width_(width),
      height_(height),
      pixels_(std::move(pixels)),
      id_(mint()),
      depth_(depth),
      signal_(signal)
{
}

Frame::Frame(const Frame &other)
    : width_(other.width_),
      height_(other.height_),
      pixels_(other.pixels_),
      id_(mint()),
      depth_(other.depth_),
      signal_(other.signal_)
{
}

Frame &Frame::operator=(const Frame &other)
{
    if (this == &other)
        return *this;
    width_ = other.width_;
    height_ = other.height_;
    pixels_ = other.pixels_;
    id_ = mint();
    depth_ = other.depth_;
    signal_ = other.signal_;
    return *this;
}

bool Frame::operator==(const Frame &other) const
{
    return width_ == other.width_ && height_ == other.height_ && depth_ == other.depth_
            && signal_ == other.signal_ && pixels_ == other.pixels_;
}

Frame Frame::black(std::uint32_t width, std::uint32_t height)
{
    std::vector<std::uint8_t> pixels(byte_len(width, height), 0);
    for (std::size_t i = 3; i < pixels.size(); i += BYTES_PER_PIXEL) {
        pixels[i] = 255;
    }
    return Frame(width, height, std::move(pixels), Depth::Eight, Signal::Sdr);
}

Frame Frame::transparent(std::uint32_t width, std::uint32_t height)
{
    return Frame(width, height, std::vector<std::uint8_t>(byte_len(width, height), 0), Depth::Eight,
                 Signal::Sdr);
}

std::optional<Frame> Frame::from_rgba(std::uint32_t width, std::uint32_t height,
                                      std::vector<std::uint8_t> pixels)
{
    if (pixels.size() != byte_len(width, height))
        return std::nullopt;
    return Frame(width, height, std::move(pixels), Depth::Eight, Signal::Sdr);
}

std::optional<Frame> Frame::from_rgba64(std::uint32_t width, std::uint32_t height,
                                        std::vector<std::uint8_t> pixels, Signal signal)
{
    if (pixels.size() != byte_len(width, height) * 2)
        return std::nullopt;
    return Frame(width, height, std::move(pixels), Depth::Sixteen, signal);
}

std::span<std::uint8_t> Frame::pixels_mut()
{
    id_ = mint();
    return pixels_;
}

std::optional<std::size_t> Frame::offset_of(std::uint32_t x, std::uint32_t y) const
{
    if (x >= width_ || y >= height_)
        return std::nullopt;
    return (static_cast<std::size_t>(y) * width_ + x) * BYTES_PER_PIXEL;
}

std::optional<std::array<std::uint8_t, 4>> Frame::pixel(std::uint32_t x, std::uint32_t y) const
{
    const auto offset = offset_of(x, y);
    if (!offset)
        return std::nullopt;
    if (depth_ == Depth::Eight) {
        return std::array<std::uint8_t, 4>{ pixels_[*offset], pixels_[*offset + 1],
                                            pixels_[*offset + 2], pixels_[*offset + 3] };
    }
    const std::size_t at = *offset * 2;
    return std::array<std::uint8_t, 4>{ pixels_[at + 1], pixels_[at + 3], pixels_[at + 5],
                                        pixels_[at + 7] };
}

void Frame::set_pixel(std::uint32_t x, std::uint32_t y, std::array<std::uint8_t, 4> rgba)
{
    id_ = mint();
    const auto offset = offset_of(x, y);
    if (!offset)
        return;
    std::memcpy(pixels_.data() + *offset, rgba.data(), BYTES_PER_PIXEL);
}

void Frame::blit(const Frame &source, std::uint32_t x, std::uint32_t y)
{
    id_ = mint();
    const std::size_t columns = std::min<std::size_t>(source.width_, width_ > x ? width_ - x : 0);
    const std::size_t rows = std::min<std::size_t>(source.height_, height_ > y ? height_ - y : 0);
    if (columns == 0 || rows == 0)
        return;

    const std::size_t source_row = static_cast<std::size_t>(source.width_) * 4;
    const std::size_t target_row = static_cast<std::size_t>(width_) * 4;
    for (std::size_t row = 0; row < rows; ++row) {
        const std::size_t from = row * source_row;
        const std::size_t to =
                (static_cast<std::size_t>(y) + row) * target_row + static_cast<std::size_t>(x) * 4;
        std::memcpy(pixels_.data() + to, source.pixels_.data() + from, columns * 4);
    }
}

void Frame::fill(std::array<std::uint8_t, 4> rgba)
{
    id_ = mint();
    for (std::size_t i = 0; i < pixels_.size(); i += BYTES_PER_PIXEL) {
        std::memcpy(pixels_.data() + i, rgba.data(), BYTES_PER_PIXEL);
    }
}

} // namespace genesis::core
