// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "workspace/update/Signature.h"

#include <cstdint>
#include <string>
#include <vector>

#if defined(GENESIS_UPDATER_ENABLED)
#  include <openssl/evp.h>
#endif

namespace genesis::workspace::update {

bool updater_enabled()
{
#if defined(GENESIS_UPDATER_ENABLED)
    return true;
#else
    return false;
#endif
}

bool release_key_configured()
{
    for (const std::uint8_t byte : kReleasePublicKey) {
        if (byte != 0) {
            return true;
        }
    }
    return false;
}

namespace {

// The 128-hex signature field decoded to 64 raw bytes. Returns empty on any
// non-hex input or a wrong length - parse_manifest has already refused such a
// manifest, but this stays robust for a hand-built Manifest in tests.
std::vector<std::uint8_t> decode_signature(const Manifest &manifest)
{
    if (manifest.signature.size() != 128) {
        return { };
    }
    const auto is_hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        return c - 'a' + 10;
    };
    std::vector<std::uint8_t> out(64, 0);
    for (std::size_t i = 0; i < 64; ++i) {
        const char hi = manifest.signature[2 * i];
        const char lo = manifest.signature[2 * i + 1];
        if (!is_hex(hi) || !is_hex(lo)) {
            return { };
        }
        out[i] = static_cast<std::uint8_t>((nibble(hi) << 4) | nibble(lo));
    }
    return out;
}

} // namespace

VerifyResult verify_ed25519(std::span<const std::uint8_t> message,
                            std::span<const std::uint8_t> signature, const PublicKey &key)
{
#if defined(GENESIS_UPDATER_ENABLED)
    if (signature.size() != 64) {
        return VerifyResult::Rejected;
    }
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size());
    if (pkey == nullptr) {
        return VerifyResult::Rejected;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const bool ok = ctx != nullptr
            && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1
            && EVP_DigestVerify(ctx, signature.data(), signature.size(), message.data(),
                                message.size())
                    == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok ? VerifyResult::Verified : VerifyResult::Rejected;
#else
    (void)message;
    (void)signature;
    (void)key;
    return VerifyResult::Unavailable;
#endif
}

VerifyResult verify_manifest(const Manifest &manifest, const PublicKey &key)
{
    const std::vector<std::uint8_t> signature = decode_signature(manifest);
    if (signature.empty()) {
        return VerifyResult::Rejected;
    }
    const std::string payload = signed_payload(manifest);
    return verify_ed25519(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(payload.data()),
                                          payload.size()),
            signature, key);
}

} // namespace genesis::workspace::update
