// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The Ed25519 signature suite, registered only when GENESIS_UPDATER is ON and
// OpenSSL was found. It generates a key pair in-process, signs manifests, and
// proves verify_manifest accepts the genuine signature and rejects tampering -
// the "signature, not hash" bar (docs/decisions/auto-update.md §4).

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/update/Manifest.h"
#include "workspace/update/Signature.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace upd = genesis::workspace::update;

struct KeyPair
{
    std::array<std::uint8_t, 32> seed;
    upd::PublicKey public_key;
};

// A deterministic Ed25519 key pair; `salt` varies the seed so two calls yield
// two distinct keys (the wrong-key test).
KeyPair make_key(std::uint8_t salt = 0)
{
    KeyPair pair{ };
    for (int i = 0; i < 32; ++i) {
        pair.seed[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i + 1 + salt);
    }
    EVP_PKEY *priv = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, pair.seed.data(),
                                                  pair.seed.size());
    check(priv != nullptr, "the in-test private key is created");
    std::size_t len = pair.public_key.size();
    EVP_PKEY_get_raw_public_key(priv, pair.public_key.data(), &len);
    EVP_PKEY_free(priv);
    return pair;
}

// The Ed25519 signature over `message`, from `pair`'s private half.
std::vector<std::uint8_t> sign(const KeyPair &pair, std::string_view message)
{
    EVP_PKEY *priv = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, pair.seed.data(),
                                                  pair.seed.size());
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, priv);
    std::size_t siglen = 0;
    EVP_DigestSign(ctx, nullptr, &siglen, reinterpret_cast<const unsigned char *>(message.data()),
                   message.size());
    std::vector<std::uint8_t> sig(siglen);
    EVP_DigestSign(ctx, sig.data(), &siglen,
                   reinterpret_cast<const unsigned char *>(message.data()), message.size());
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(priv);
    return sig;
}

std::string hex(std::span<const std::uint8_t> bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[2 * i] = kDigits[bytes[i] >> 4];
        out[2 * i + 1] = kDigits[bytes[i] & 0xf];
    }
    return out;
}

// A manifest signed over its canonical payload by `pair`.
upd::Manifest signed_manifest(const KeyPair &pair, const std::string &version,
                              const std::string &url, const std::string &sha256)
{
    upd::Manifest manifest;
    manifest.version = version;
    manifest.url = url;
    manifest.sha256 = sha256;
    manifest.signature = hex(sign(pair, upd::signed_payload(manifest)));
    return manifest;
}

std::string to_json(const upd::Manifest &manifest)
{
    return "{\"version\":\"" + manifest.version + "\",\"url\":\"" + manifest.url
            + "\",\"sha256\":\"" + manifest.sha256 + "\",\"signature\":\"" + manifest.signature
            + "\"}";
}

void test_build_reports_enabled()
{
    check(upd::updater_enabled(), "the crypto build reports enabled");
}

void test_good_signature_verifies()
{
    const KeyPair pair = make_key();
    const upd::Manifest manifest = signed_manifest(
            pair, "1.0.0", "https://example/genesis-1.0.0.AppImage", std::string(64, 'a'));
    check(upd::verify_manifest(manifest, pair.public_key) == upd::VerifyResult::Verified,
          "a genuine signature verifies");
}

void test_a_tampered_signature_is_rejected()
{
    const KeyPair pair = make_key();
    upd::Manifest manifest = signed_manifest(
            pair, "1.0.0", "https://example/genesis-1.0.0.AppImage", std::string(64, 'a'));
    // Flip the first signature hex digit.
    manifest.signature[0] = manifest.signature[0] == '0' ? '1' : '0';
    check(upd::verify_manifest(manifest, pair.public_key) == upd::VerifyResult::Rejected,
          "a tampered signature is rejected");
}

void test_a_tampered_payload_is_rejected()
{
    const KeyPair pair = make_key();
    const std::string sha256 = std::string(64, 'a');

    upd::Manifest changed_version =
            signed_manifest(pair, "1.0.0", "https://example/genesis-1.0.0.AppImage", sha256);
    changed_version.version = "9.9.9";
    check(upd::verify_manifest(changed_version, pair.public_key) == upd::VerifyResult::Rejected,
          "a tampered version is rejected");

    upd::Manifest changed_url =
            signed_manifest(pair, "1.0.0", "https://example/genesis-1.0.0.AppImage", sha256);
    changed_url.url = "https://evil.example/genesis-1.0.0.AppImage";
    check(upd::verify_manifest(changed_url, pair.public_key) == upd::VerifyResult::Rejected,
          "a tampered url is rejected");

    upd::Manifest changed_digest =
            signed_manifest(pair, "1.0.0", "https://example/genesis-1.0.0.AppImage", sha256);
    changed_digest.sha256 = std::string(64, 'b');
    check(upd::verify_manifest(changed_digest, pair.public_key) == upd::VerifyResult::Rejected,
          "a tampered digest is rejected");
}

void test_a_signature_under_the_wrong_key_is_rejected()
{
    const KeyPair pair = make_key();
    const KeyPair other = make_key(/*salt=*/1);
    const upd::Manifest manifest = signed_manifest(
            pair, "1.0.0", "https://example/genesis-1.0.0.AppImage", std::string(64, 'a'));
    check(upd::verify_manifest(manifest, other.public_key) == upd::VerifyResult::Rejected,
          "a signature under a different key is rejected");
}

void test_a_wrong_length_signature_is_rejected()
{
    const KeyPair pair = make_key();
    const std::vector<std::uint8_t> message{ 'h', 'i' };
    const std::vector<std::uint8_t> short_sig(63, 0);
    check(upd::verify_ed25519(message, short_sig, pair.public_key) == upd::VerifyResult::Rejected,
          "a 63-byte signature is rejected");
}

void test_parse_round_trip_preserves_the_signature()
{
    const KeyPair pair = make_key();
    const upd::Manifest original = signed_manifest(
            pair, "1.0.0", "https://example/genesis-1.0.0.AppImage", std::string(64, 'a'));

    const upd::ParseResult parsed = upd::parse_manifest(to_json(original));
    check(parsed.ok(), "the signed manifest round-trips through JSON");
    if (parsed.manifest) {
        check(upd::verify_manifest(*parsed.manifest, pair.public_key)
                      == upd::VerifyResult::Verified,
              "the round-tripped manifest still verifies");
    }
}

} // namespace

int main()
{
    test_build_reports_enabled();
    test_good_signature_verifies();
    test_a_tampered_signature_is_rejected();
    test_a_tampered_payload_is_rejected();
    test_a_signature_under_the_wrong_key_is_rejected();
    test_a_wrong_length_signature_is_rejected();
    test_parse_round_trip_preserves_the_signature();

    return genesis::test::summary();
}
