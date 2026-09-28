// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "workspace/update/Manifest.h"

namespace genesis::workspace::update {

// A 32-byte Ed25519 public key. The release public half ships with the app;
// the private half stays with the release tooling (docs/decisions/auto-update.md
// §8.4).
using PublicKey = std::array<std::uint8_t, 32>;

// The embedded release public key. Until a signing key is minted this is all
// zeros, which verifies nothing - every install is refused (signature, or do
// not self-update). Replace these bytes with the real public half before
// enabling GENESIS_UPDATER for a release.
inline constexpr PublicKey kReleasePublicKey{ };

enum class VerifyResult {
    Verified, // the signature matches the payload under the key
    Rejected, // the signature does not verify
    Unavailable, // crypto not compiled in (GENESIS_UPDATER=OFF)
};

// Whether the updater was built with crypto. False in the default build; the
// app gates any "check for updates" surface on this.
bool updater_enabled();

// Whether a real release key is embedded. Until one is minted kReleasePublicKey
// is all zeros, which verifies nothing - an enabled build still refuses every
// check as "release key not configured", distinct from the build-disabled case
// (docs/decisions/auto-update.md §10).
bool release_key_configured();

// Ed25519 signature verification. `Unavailable` when crypto is not compiled in;
// `Rejected` for a signature that does not verify (or is not 64 bytes).
VerifyResult verify_ed25519(std::span<const std::uint8_t> message,
                            std::span<const std::uint8_t> signature, const PublicKey &key);

// Verifies a manifest's signature over its canonical signed payload. A missing
// or malformed signature field reads `Rejected`, never a crash.
VerifyResult verify_manifest(const Manifest &manifest, const PublicKey &key);

} // namespace genesis::workspace::update
