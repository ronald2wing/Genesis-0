// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace genesis::workspace::update {

// One signed release manifest: what a release is, where its package lives, and
// how both are verified. The signature covers signed_payload() - version, url
// and the package's SHA-256 - and is verified against the embedded release key
// before any byte is downloaded (docs/decisions/auto-update.md §4: signature,
// not hash, for the application itself).
struct Manifest
{
    // The release version, a path segment under <root>/versions/ (e.g. "1.2.3").
    std::string version;
    // The package download URL, pinned to an immutable release.
    std::string url;
    // The SHA-256 of the downloaded package, 64 lowercase hex digits.
    std::string sha256;
    // The Ed25519 signature over signed_payload(), 128 lowercase hex digits.
    std::string signature;
    // Optional release notes the notify-only surface shows before the user
    // decides to update. Informational and unsigned: the signature covers only
    // version, url and sha256, so a compromised feed can only misinform through
    // the notes, never install (docs/decisions/auto-update.md §3(b)).
    std::string notes;
};

// The exact bytes a signature covers: version, url and sha256, newline-joined
// in that order. The signature field itself is not part of the payload. This is
// the one canonical form, so the release signer and the verifier agree on what
// was signed even if the on-disk JSON were to reorder keys.
std::string signed_payload(const Manifest &manifest);

struct ParseResult
{
    std::optional<Manifest> manifest;
    std::vector<std::string> problems;
    bool ok() const { return manifest.has_value(); }
};

// Parses a release manifest from JSON. A manifest that does not parse, is
// missing a field, or carries a malformed digest/signature is refused with a
// non-empty `problems` and no Manifest - nothing is downloaded on its account.
ParseResult parse_manifest(std::string_view json);

} // namespace genesis::workspace::update
