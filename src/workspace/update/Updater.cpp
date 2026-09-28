// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "workspace/update/Updater.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include "ai/Sha256.h"
#include "workspace/update/Markers.h"
#include "workspace/update/UpdateVersion.h"

namespace genesis::workspace::update {

namespace {

constexpr const char *kVersionsDir = "versions";

// A directory-name segment that cannot escape its parent: no '/', no '\\', and
// no ".." component. Defense in depth so a version read from a signed manifest
// can never name a path outside <root>/versions/.
bool is_safe_path_segment(std::string_view value)
{
    return !value.empty() && value.find('/') == std::string_view::npos
            && value.find('\\') == std::string_view::npos
            && value.find("..") == std::string_view::npos;
}

// The package's on-disk name: the basename of its URL, so two versions whose
// URLs differ only by path never collide.
std::string package_filename(const Manifest &manifest)
{
    return std::filesystem::path(manifest.url).filename().string();
}

std::string hex_encode(std::span<const std::uint8_t> bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[2 * i] = kDigits[bytes[i] >> 4];
        out[(2 * i) + 1] = kDigits[bytes[i] & 0xf];
    }
    return out;
}

// Downloads the package through `fetcher` into a sibling partial, verifying the
// SHA-256 as bytes arrive; only a digest match renames it into
// versions/<v>/<file>. Any failure leaves no partial and no version entry.
InstallResult stage(const std::filesystem::path &root, const Manifest &manifest,
                    const Fetcher &fetcher)
{
    InstallResult result;
    if (manifest.version.empty() || manifest.url.empty() || manifest.sha256.size() != 64) {
        result.problems.push_back("the manifest is missing a version, url or sha256");
        return result;
    }
    if (!is_safe_path_segment(manifest.version)) {
        result.problems.push_back("the manifest version is not a safe path segment");
        return result;
    }
    const std::string filename = package_filename(manifest);
    if (filename.empty()) {
        result.problems.push_back("the manifest url has no filename");
        return result;
    }

    std::error_code ec;
    const std::filesystem::path versions = root / kVersionsDir;
    std::filesystem::create_directories(versions, ec);
    if (ec) {
        result.problems.push_back("cannot create the versions directory: " + ec.message());
        return result;
    }

    const std::filesystem::path partial = versions / (".tmp-" + manifest.version + "-" + filename);
    std::filesystem::remove_all(partial, ec);
    ec.clear();

    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.problems.push_back("cannot open the download partial file");
        return result;
    }

    genesis::ai::Sha256 hasher;
    const bool delivered = fetcher(manifest, [&](std::span<const std::uint8_t> chunk) {
        out.write(reinterpret_cast<const char *>(chunk.data()),
                  static_cast<std::streamsize>(chunk.size()));
        hasher.update(chunk);
        return static_cast<bool>(out);
    });
    out.close();
    if (!delivered || out.fail()) {
        std::filesystem::remove(partial, ec);
        result.problems.push_back("the download was interrupted");
        return result;
    }

    const std::array<std::uint8_t, 32> digest = hasher.digest();
    if (hex_encode(digest) != manifest.sha256) {
        std::filesystem::remove(partial, ec);
        result.problems.push_back("the downloaded bytes do not match the manifest sha256");
        return result;
    }

    const std::filesystem::path version_dir = versions / manifest.version;
    std::filesystem::create_directories(version_dir, ec);
    if (ec) {
        std::filesystem::remove(partial, ec);
        result.problems.push_back("cannot create the version directory: " + ec.message());
        return result;
    }
    // Reinstalling the same version overwrites deterministically: drop the old
    // file before the rename, which cannot replace a non-empty directory entry.
    std::filesystem::remove(version_dir / filename, ec);
    ec.clear();
    std::filesystem::rename(partial, version_dir / filename, ec);
    if (ec) {
        std::filesystem::remove(partial, ec);
        result.problems.push_back("cannot move the download into place: " + ec.message());
        return result;
    }
    result.installed = version_dir / filename;
    return result;
}

} // namespace

std::optional<std::string> last_seen_version(const std::filesystem::path &root)
{
    return read_marker(root / kLastSeen);
}

std::string monotonic_refusal(const std::filesystem::path &root, std::string_view version)
{
    const std::optional<std::string> seen = last_seen_version(root);
    if (!seen) {
        return { };
    }
    const int comparison = compare_versions(version, *seen);
    if (comparison < 0) {
        return "refusing to downgrade: version " + std::string(version)
                + " is older than the last-seen " + *seen;
    }
    if (comparison == 0) {
        return "refusing to replay: version " + std::string(version) + " was already applied";
    }
    return { };
}

InstallResult install(const std::filesystem::path &root, const Manifest &manifest,
                      const Verifier &verifier, const Fetcher &fetcher,
                      std::string_view running_version)
{
    InstallResult result;

    // Anti-downgrade/replay: a manifest whose version is older than or equal to
    // the persisted last-seen record is refused before a signature is checked
    // or a byte is fetched. On a fresh install with no record, the caller's
    // running version seeds the baseline so the running version is never
    // silently regressed. The signed payload carries no timestamp/nonce, so the
    // version is the monotonic freshness signal and equality means replay.
    if (!running_version.empty() && !last_seen_version(root).has_value()) {
        write_marker(root / kLastSeen, running_version);
    }
    if (const std::string refusal = monotonic_refusal(root, manifest.version); !refusal.empty()) {
        result.problems.push_back(refusal);
        return result;
    }

    if (!verifier) {
        result.problems.push_back("no signature verifier supplied - refusing to install "
                                  "(signature, or do not self-update)");
        return result;
    }
    const VerifyResult verdict = verifier(manifest);
    if (verdict == VerifyResult::Unavailable) {
        result.problems.push_back("disabled: crypto not compiled in (rebuild "
                                  "with GENESIS_UPDATER=ON)");
        return result;
    }
    if (verdict != VerifyResult::Verified) {
        result.problems.push_back("the manifest signature does not verify");
        return result;
    }

    // Download + digest-verify + stage. No file is placed on a bad digest.
    InstallResult staged = stage(root, manifest, fetcher);
    if (!staged.ok()) {
        return staged;
    }

    // Atomic swap: record the new version as pending, then repoint current.
    // The previous version directory is never touched - the running binary
    // stays put; only the `current` pointer moves.
    if (!write_marker(root / kPending, manifest.version)
        || !write_marker(root / kCurrent, manifest.version)) {
        InstallResult failed;
        failed.problems.push_back("cannot swap the current version into place");
        return failed;
    }
    return staged;
}

void confirm(const std::filesystem::path &root)
{
    if (const std::optional<std::string> current = current_version(root)) {
        write_marker(root / kConfirmed, *current);
        // Advance the anti-downgrade/replay high-water mark: the version has
        // now actually run, so anything at or below it is a regression.
        write_marker(root / kLastSeen, *current);
    }
    remove_marker(root / kPending);
}

bool rollback(const std::filesystem::path &root)
{
    const std::optional<std::string> confirmed = confirmed_version(root);
    if (!confirmed) {
        return false;
    }
    write_marker(root / kCurrent, *confirmed);
    remove_marker(root / kPending);
    return true;
}

std::optional<std::string> current_version(const std::filesystem::path &root)
{
    return read_marker(root / kCurrent);
}

std::optional<std::string> confirmed_version(const std::filesystem::path &root)
{
    return read_marker(root / kConfirmed);
}

std::optional<std::string> pending_version(const std::filesystem::path &root)
{
    return read_marker(root / kPending);
}

} // namespace genesis::workspace::update
