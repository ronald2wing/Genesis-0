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

namespace genesis::workspace::update {

namespace {

constexpr const char *kVersionsDir = "versions";
constexpr const char *kCurrent = "current";
constexpr const char *kPending = "pending";
constexpr const char *kConfirmed = "confirmed";

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
        out[2 * i + 1] = kDigits[bytes[i] & 0xf];
    }
    return out;
}

// Reads a marker file's single line (the active/confirmed/pending version).
// Absent or unreadable -> nullopt.
std::optional<std::string> read_marker(const std::filesystem::path &path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream in(path);
    std::string value;
    std::getline(in, value);
    while (!value.empty()
           && (value.back() == '\n' || value.back() == '\r' || value.back() == ' '
               || value.back() == '\t')) {
        value.pop_back();
    }
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

// Writes a marker by writing a sibling then renaming over the target, so a
// reader never observes a half-written marker (atomic on the same filesystem).
bool write_marker(const std::filesystem::path &path, std::string_view value)
{
    const std::filesystem::path tmp = std::filesystem::path(path.string() + ".tmp");
    std::error_code ec;
    std::ofstream out(tmp, std::ios::trunc);
    out << value << '\n';
    out.close();
    if (out.fail()) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

bool remove_marker(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
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

InstallResult install(const std::filesystem::path &root, const Manifest &manifest,
                      const Verifier &verifier, const Fetcher &fetcher)
{
    InstallResult result;
    // TODO(security): this pipeline verifies a signature and a digest but does
    // not yet reject an anti-downgrade or a replay: an attacker who can sign
    // (or re-serve an old, validly signed manifest) can roll the install back
    // to an earlier version, or replay the same update. The intended fix is a
    // monotonic version plus a timestamp/nonce carried in the signed payload
    // (signed_payload() / Manifest), rejected here when it does not move
    // forward. See docs/decisions/auto-update.md §10. Not implemented yet.
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
