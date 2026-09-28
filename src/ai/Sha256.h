// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace genesis::ai {

// Streaming SHA-256 (FIPS 180-4). The host core takes no third-party crypto,
// and FNV-1a (render/) is a cache-key hash, not a content digest, so the model
// store needs a real hash to verify downloads against. This is that hash:
// compact, dependency-free, and pinned against the NIST vectors in the
// model_store suite.
class Sha256
{
public:
    Sha256();

    // Feeds bytes; callable any number of times before digest().
    void update(std::span<const std::uint8_t> bytes);

    // The 32-byte digest. Finalizes the state; do not call update() afterward.
    std::array<std::uint8_t, 32> digest();

private:
    void process(std::span<const std::uint8_t, 64> block);

    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> block_;
    std::size_t block_len_ = 0;
    std::uint64_t total_len_ = 0;
};

// The SHA-256 of `bytes` as 64 lowercase hex digits - the canonical digest form
// ModelEntry.sha256 stores.
std::string sha256_hex(std::span<const std::uint8_t> bytes);

} // namespace genesis::ai
