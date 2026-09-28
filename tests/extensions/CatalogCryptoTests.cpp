// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The catalog-signature suite, registered only when GENESIS_UPDATER is ON and
// OpenSSL was found. It generates a key pair in-process, signs catalog entries
// over their canonical payload, and proves verify_entry_signature accepts the
// genuine signature and rejects tampering - the "signature, not hash" bar the
// updater sets for itself (docs/decisions/auto-update.md §4), applied here to a
// catalog entry's id + digest.

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "extensions/Catalog.h"
#include "workspace/update/Signature.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace ext = genesis::extensions;
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

// A catalog entry signed over its canonical payload by `pair`.
ext::CatalogEntry signed_entry(const KeyPair &pair, const std::string &id,
                               const std::string &sha256)
{
    ext::CatalogEntry entry;
    entry.id = id;
    entry.name = "Fixture";
    entry.version = "1";
    entry.sha256 = sha256;
    entry.signature = hex(sign(pair, ext::signed_entry_payload(entry)));
    return entry;
}

void test_key_not_yet_configured()
{
    // The catalog signing key is pending: until one is minted the embedded key
    // is all zeros, so a signed entry is refused fail-closed at install even in
    // a crypto-enabled build.
    check(!ext::catalog_key_configured(), "no catalog signing key is configured");
}

void test_good_signature_verifies()
{
    const KeyPair pair = make_key();
    const ext::CatalogEntry entry = signed_entry(pair, "a.dev", std::string(64, 'a'));
    check(ext::verify_entry_signature(entry, pair.public_key) == upd::VerifyResult::Verified,
          "a genuine signature verifies");
}

void test_a_tampered_signature_is_rejected()
{
    const KeyPair pair = make_key();
    ext::CatalogEntry entry = signed_entry(pair, "a.dev", std::string(64, 'a'));
    entry.signature->front() = entry.signature->front() == '0' ? '1' : '0';
    check(ext::verify_entry_signature(entry, pair.public_key) == upd::VerifyResult::Rejected,
          "a tampered signature is rejected");
}

void test_a_tampered_digest_is_rejected()
{
    const KeyPair pair = make_key();
    ext::CatalogEntry entry = signed_entry(pair, "a.dev", std::string(64, 'a'));
    entry.sha256 = std::string(64, 'b');
    check(ext::verify_entry_signature(entry, pair.public_key) == upd::VerifyResult::Rejected,
          "a tampered digest is rejected");
}

void test_a_tampered_id_is_rejected()
{
    const KeyPair pair = make_key();
    ext::CatalogEntry entry = signed_entry(pair, "a.dev", std::string(64, 'a'));
    entry.id = "b.dev";
    check(ext::verify_entry_signature(entry, pair.public_key) == upd::VerifyResult::Rejected,
          "a tampered id is rejected");
}

void test_a_signature_under_the_wrong_key_is_rejected()
{
    const KeyPair pair = make_key();
    const KeyPair other = make_key(/*salt=*/1);
    const ext::CatalogEntry entry = signed_entry(pair, "a.dev", std::string(64, 'a'));
    check(ext::verify_entry_signature(entry, other.public_key) == upd::VerifyResult::Rejected,
          "a signature under a different key is rejected");
}

void test_a_missing_sha256_is_rejected()
{
    const KeyPair pair = make_key();
    const ext::CatalogEntry entry = signed_entry(pair, "a.dev", std::string(64, 'a'));
    ext::CatalogEntry no_digest = entry;
    no_digest.sha256.reset();
    check(ext::verify_entry_signature(no_digest, pair.public_key) == upd::VerifyResult::Rejected,
          "a signed entry with no sha256 to bind to is rejected");
}

void test_a_missing_signature_is_rejected()
{
    const KeyPair pair = make_key();
    const ext::CatalogEntry entry = signed_entry(pair, "a.dev", std::string(64, 'a'));
    ext::CatalogEntry unsigned_entry = entry;
    unsigned_entry.signature.reset();
    check(ext::verify_entry_signature(unsigned_entry, pair.public_key)
                  == upd::VerifyResult::Rejected,
          "an unsigned entry is never read as verified");
}

void test_a_malformed_signature_is_rejected()
{
    const KeyPair pair = make_key();
    ext::CatalogEntry entry = signed_entry(pair, "a.dev", std::string(64, 'a'));
    entry.signature = std::string(128, 'g'); // not hex
    check(ext::verify_entry_signature(entry, pair.public_key) == upd::VerifyResult::Rejected,
          "a malformed signature is rejected");
}

} // namespace

int main()
{
    test_key_not_yet_configured();
    test_good_signature_verifies();
    test_a_tampered_signature_is_rejected();
    test_a_tampered_digest_is_rejected();
    test_a_tampered_id_is_rejected();
    test_a_signature_under_the_wrong_key_is_rejected();
    test_a_missing_sha256_is_rejected();
    test_a_missing_signature_is_rejected();
    test_a_malformed_signature_is_rejected();

    return genesis::test::summary();
}
