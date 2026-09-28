// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/Sha256.h"

#include <array>
#include <cstring>

namespace genesis::ai {

namespace {

constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::array<std::uint32_t, 8> kInitial = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

constexpr std::uint32_t rotr(std::uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

} // namespace

Sha256::Sha256() : h_(kInitial) { }

void Sha256::update(std::span<const std::uint8_t> bytes)
{
    total_len_ += bytes.size();
    while (!bytes.empty()) {
        const std::size_t room = 64 - block_len_;
        const std::size_t take = room < bytes.size() ? room : bytes.size();
        std::memcpy(block_.data() + block_len_, bytes.data(), take);
        block_len_ += take;
        bytes = bytes.subspan(take);
        if (block_len_ == 64) {
            process(block_);
            block_len_ = 0;
        }
    }
}

void Sha256::process(std::span<const std::uint8_t, 64> block)
{
    std::array<std::uint32_t, 64> w{ };
    for (int i = 0; i < 16; ++i) {
        w[static_cast<std::size_t>(i)] = (static_cast<std::uint32_t>(block[4 * i]) << 24)
                | (static_cast<std::uint32_t>(block[4 * i + 1]) << 16)
                | (static_cast<std::uint32_t>(block[4 * i + 2]) << 8)
                | static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[static_cast<std::size_t>(i - 15)], 7)
                ^ rotr(w[static_cast<std::size_t>(i - 15)], 18)
                ^ (w[static_cast<std::size_t>(i - 15)] >> 3);
        const std::uint32_t s1 = rotr(w[static_cast<std::size_t>(i - 2)], 17)
                ^ rotr(w[static_cast<std::size_t>(i - 2)], 19)
                ^ (w[static_cast<std::size_t>(i - 2)] >> 10);
        w[static_cast<std::size_t>(i)] =
                w[static_cast<std::size_t>(i - 16)] + s0 + w[static_cast<std::size_t>(i - 7)] + s1;
    }

    std::uint32_t a = h_[0];
    std::uint32_t b = h_[1];
    std::uint32_t c = h_[2];
    std::uint32_t d = h_[3];
    std::uint32_t e = h_[4];
    std::uint32_t f = h_[5];
    std::uint32_t g = h_[6];
    std::uint32_t h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + big_s1 + choose + kRound[i] + w[i];
        const std::uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = big_s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

std::array<std::uint8_t, 32> Sha256::digest()
{
    // 0x80, zero padding, then the 64-bit big-endian bit length.
    std::array<std::uint8_t, 64> tail{ };
    std::size_t tail_len = block_len_;
    std::memcpy(tail.data(), block_.data(), block_len_);
    tail[tail_len++] = 0x80;
    if (tail_len > 56) {
        process(tail);
        tail = { };
        tail_len = 0;
    }
    const std::uint64_t bit_len = total_len_ * 8;
    for (int i = 0; i < 8; ++i) {
        tail[56 + i] = static_cast<std::uint8_t>(bit_len >> (56 - 8 * i));
    }
    process(tail);

    std::array<std::uint8_t, 32> out{ };
    for (std::size_t i = 0; i < 8; ++i) {
        out[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
        out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
        out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
    }
    return out;
}

std::string sha256_hex(std::span<const std::uint8_t> bytes)
{
    Sha256 hasher;
    hasher.update(bytes);
    const std::array<std::uint8_t, 32> digest = hasher.digest();
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[2 * i] = kDigits[digest[i] >> 4];
        out[2 * i + 1] = kDigits[digest[i] & 0xf];
    }
    return out;
}

} // namespace genesis::ai
