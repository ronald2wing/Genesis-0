// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "workspace/update/Manifest.h"
#include "workspace/update/Signature.h"

namespace genesis::workspace::update {

// A transport for one package download, matching ai::Fetcher's shape so a real
// HTTP client (or a test fake) drops straight in. It is handed the manifest
// (for its URL) and a sink it must feed every downloaded byte through; `sink`
// returns false when the updater wants the transfer stopped, and a fetcher that
// sees that must stop and return false itself.
using Fetcher =
        std::function<bool(const Manifest &manifest,
                           const std::function<bool(std::span<const std::uint8_t> chunk)> &sink)>;

// Proves a manifest's signature. The app binds verify_manifest against its
// embedded release key; tests inject a test fake so the crypto-free pipeline
// (digest check, staging, swap, rollback) is exercised hermetically without
// OpenSSL. An
// empty verifier refuses the install - signature, or do not self-update.
using Verifier = std::function<VerifyResult(const Manifest &manifest)>;

struct InstallResult
{
    std::optional<std::filesystem::path> installed;
    std::vector<std::string> problems;
    bool ok() const { return installed.has_value(); }
};

// The self-update entry: verify -> download -> stage -> swap. The layout under
// `root`:
//
//   versions/<v>/<file>   one staged package per version (never mutated once
//                         placed; the running binary lives here)
//   current               active version string, swapped atomically
//   pending               version awaiting confirmation after the swap
//   confirmed             last known-good version, the rollback target
//
// The running binary is never overwritten: an install only adds a new
// versions/<v> directory and repoints `current`. `verifier` must prove the
// manifest before a single byte is fetched; a missing or failing verifier
// refuses the install and touches nothing.
InstallResult install(const std::filesystem::path &root, const Manifest &manifest,
                      const Verifier &verifier, const Fetcher &fetcher);

// The launcher calls confirm() after the swapped-in version has run
// successfully: `confirmed` becomes the active version and `pending` clears.
void confirm(const std::filesystem::path &root);

// The launcher calls rollback() when the swapped-in version fails to start:
// `current` is restored to `confirmed` and `pending` clears. Returns false when
// there is no confirmed baseline to roll back to (a fresh install).
bool rollback(const std::filesystem::path &root);

std::optional<std::string> current_version(const std::filesystem::path &root);
std::optional<std::string> confirmed_version(const std::filesystem::path &root);
std::optional<std::string> pending_version(const std::filesystem::path &root);

} // namespace genesis::workspace::update
