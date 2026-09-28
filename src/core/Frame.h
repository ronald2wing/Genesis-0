// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-core/src/frame.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace genesis::core {

// Bytes per pixel of an eight-bit frame, the format nearly everything uses.
inline constexpr std::size_t BYTES_PER_PIXEL = 4;

// How many bits a channel a frame's pixels hold.
enum class Depth : std::uint8_t {
    // Eight: u8 channels, four bytes a pixel.
    Eight,
    // Sixteen: little-endian u16 channels, eight bytes a pixel.
    Sixteen,
};

// What a frame's RGB values mean: the transfer they are encoded with and the
// primaries they are on. An eight-bit frame is always Signal::Sdr; a deep one
// carries its source's.
enum class Signal : std::uint8_t {
    // Gamma-encoded (BT.1886, 2.4) on Rec. 709 primaries.
    Sdr,
    // Gamma-encoded (BT.1886, 2.4) on Rec. 2020 primaries: a wide-gamut SDR
    // source.
    SdrWide,
    // ARIB STD-B67 hybrid log-gamma on Rec. 2020: an iPhone's HDR.
    Hlg,
    // SMPTE ST 2084 perceptual quantiser on Rec. 2020: HDR10.
    Pq,
};

// An RGBA8 image.
//
// Width and height are fixed at construction, and the buffer is always exactly
// width * height * 4 bytes, so indexing arithmetic cannot go wrong after the
// fact.
class Frame
{
public:
    // A number no other frame has had, and this one loses the moment its
    // pixels are touched: a renderer keys what it has uploaded by it, so a
    // picture it already holds - a still, a title, a paused clip's frame - is
    // not uploaded again. Never zero.
    std::uint64_t id() const { return id_; }

    // How many bytes a frame of this size occupies.
    static constexpr std::size_t byte_len(std::uint32_t width, std::uint32_t height)
    {
        return static_cast<std::size_t>(width) * height * BYTES_PER_PIXEL;
    }

    // An opaque black frame.
    static Frame black(std::uint32_t width, std::uint32_t height);

    // A fully transparent frame.
    static Frame transparent(std::uint32_t width, std::uint32_t height);

    // Wraps an existing buffer. Returns nullopt if the buffer is not exactly
    // width * height * 4 bytes. Decoders should use this rather than trusting
    // their own arithmetic.
    static std::optional<Frame> from_rgba(std::uint32_t width, std::uint32_t height,
                                          std::vector<std::uint8_t> pixels);

    // Wraps a deep buffer: little-endian u16 RGBA, exactly width * height * 8
    // bytes, in signal. nullopt if the size is wrong.
    static std::optional<Frame> from_rgba64(std::uint32_t width, std::uint32_t height,
                                            std::vector<std::uint8_t> pixels, Signal signal);

    // A copy is a picture of its own: it may be changed next, and a renderer
    // holding the original must not take it for the copy.
    Frame(const Frame &other);
    Frame &operator=(const Frame &other);
    Frame(Frame &&) noexcept = default;
    Frame &operator=(Frame &&) noexcept = default;

    // The same picture, whatever its identity.
    bool operator==(const Frame &other) const;

    // Eight bits a channel, or sixteen.
    Depth depth() const { return depth_; }

    // What the values mean.
    Signal signal() const { return signal_; }

    // Bytes a pixel of this frame takes.
    std::size_t bytes_per_pixel() const
    {
        return depth_ == Depth::Eight ? BYTES_PER_PIXEL : BYTES_PER_PIXEL * 2;
    }

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }

    // True if both frames have the same dimensions.
    bool same_size_as(const Frame &other) const
    {
        return width_ == other.width_ && height_ == other.height_;
    }

    // The raw RGBA bytes, row-major from the top-left.
    std::span<const std::uint8_t> pixels() const { return pixels_; }

    // The raw RGBA bytes, mutably. A new identity with them: whatever is
    // written makes this a different picture from the one uploaded.
    std::span<std::uint8_t> pixels_mut();

    // Consumes the frame and yields its buffer, so it can be handed to an
    // encoder without a copy.
    std::vector<std::uint8_t> into_pixels() && { return std::move(pixels_); }

    // Byte offset of the pixel at (x, y), or nullopt if out of bounds.
    std::optional<std::size_t> offset_of(std::uint32_t x, std::uint32_t y) const;

    // Reads one pixel as [r, g, b, a]: a deep frame's to eight bits, its
    // channels' high bytes, for a test or a log to look at.
    std::optional<std::array<std::uint8_t, 4>> pixel(std::uint32_t x, std::uint32_t y) const;

    // Writes one pixel. Out-of-bounds writes are ignored.
    void set_pixel(std::uint32_t x, std::uint32_t y, std::array<std::uint8_t, 4> rgba);

    // Copies source into this frame with its top-left corner at (x, y),
    // clipping whatever falls outside. No blending: the source's pixels
    // replace what was there.
    void blit(const Frame &source, std::uint32_t x, std::uint32_t y);

    // Fills the whole frame with one colour.
    void fill(std::array<std::uint8_t, 4> rgba);

private:
    Frame(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> pixels, Depth depth,
          Signal signal);

    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<std::uint8_t> pixels_;
    std::uint64_t id_;
    Depth depth_;
    Signal signal_;
};

} // namespace genesis::core
