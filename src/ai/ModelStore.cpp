// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/ModelStore.h"

#include <array>
#include <atomic>
#include <fstream>
#include <string_view>
#include <system_error>

#include <unistd.h>

#include "ai/Sha256.h"

namespace genesis::ai {

namespace {

constexpr char kHex[] = "0123456789abcdef";

bool is_hex_byte(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool is_valid_sha256(const std::string &hex)
{
    if (hex.size() != 64) {
        return false;
    }
    for (const char c : hex) {
        if (!is_hex_byte(c)) {
            return false;
        }
    }
    return true;
}

// A directory-name segment that cannot escape its parent: no '/', no '\\', and
// no ".." component. Defense in depth on top of the download-refusal checks, so
// an id/version read from an untrusted manifest can never name a path outside
// the model root.
bool is_safe_path_segment(std::string_view value)
{
    return !value.empty() && value.find('/') == std::string_view::npos
            && value.find('\\') == std::string_view::npos
            && value.find("..") == std::string_view::npos;
}

// The basename of a URL, or empty when the URL names no file.
std::string url_basename(const std::string &url)
{
    const std::size_t slash = url.find_last_of('/');
    if (slash == std::string::npos) {
        return url;
    }
    return url.substr(slash + 1);
}

// The filename the model's bytes land under, so the runtime reads the extension
// the weights actually have (`.bin`, `.onnx`, ...).
std::string model_filename(const ModelEntry &entry)
{
    const std::string name = url_basename(entry.url);
    if (!name.empty()) {
        return name;
    }
    return url_basename(entry.mirror);
}

// The sidecar path: the model file's path with ".meta" appended, so the model
// file stays a plain blob a runtime can open without knowing the sidecar
// format.
std::filesystem::path sidecar_path(const std::filesystem::path &model_file)
{
    return std::filesystem::path(model_file.string() + ".meta");
}

// The partial file a download streams into, named like the extensions store's
// staging directories so a reader never mistakes it for a complete model. The
// pid + per-process counter suffix keeps the name unique across concurrent
// installs of the same entry (two threads staging the same id/version would
// otherwise collide on one path), so the pre-open `remove_all` could be
// dropped: no install ever reuses another's partial.
std::filesystem::path staging_path(const std::filesystem::path &dir, const ModelEntry &entry)
{
    static std::atomic<std::uint64_t> counter{ 0 };
    const std::uint64_t nonce = counter.fetch_add(1, std::memory_order_relaxed);
    return dir
            / (".tmp-" + entry.id + "-" + entry.version + "-" + std::to_string(::getpid()) + "-"
               + std::to_string(nonce));
}

// Writes the licence/digest sidecar: two key=value lines, so the licence
// travels with the weights it governs.
bool write_sidecar(const std::filesystem::path &model_file, const std::string &license,
                   const std::string &sha256)
{
    std::ofstream out(sidecar_path(model_file), std::ios::trunc);
    if (!out) {
        return false;
    }
    out << "license=" << license << "\n"
        << "sha256=" << sha256 << "\n";
    return static_cast<bool>(out);
}

// The lowercase hex of `bytes`, no hashing: the digest is already computed.
std::string hex_encode(std::span<const std::uint8_t> bytes)
{
    std::string out(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[2 * i] = kHex[bytes[i] >> 4];
        out[(2 * i) + 1] = kHex[bytes[i] & 0xf];
    }
    return out;
}

} // namespace

std::filesystem::path model_path(const std::filesystem::path &root, const ModelEntry &entry)
{
    return root / entry.id / entry.version / model_filename(entry);
}

bool installed(const std::filesystem::path &root, const ModelEntry &entry)
{
    if (!is_valid_sha256(entry.sha256)) {
        return false;
    }
    const std::filesystem::path file = model_path(root, entry);

    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) {
        return false;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    Sha256 hasher;
    std::array<char, 65536> buffer{ };
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            hasher.update(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t *>(buffer.data()),
                    static_cast<std::size_t>(got)));
        }
    }
    const std::array<std::uint8_t, 32> digest = hasher.digest();
    return hex_encode(digest) == entry.sha256;
}

InstallResult install(const std::filesystem::path &root, const ModelEntry &entry,
                      const Fetcher &fetcher, const Progress &progress)
{
    InstallResult result;

    // 1. Refuse before any file is touched.
    if (entry.id.empty() || entry.version.empty()) {
        result.problems.push_back("a model needs an id and a version");
        return result;
    }
    if (!is_safe_path_segment(entry.id)) {
        result.problems.push_back("the model id is not a safe path segment");
        return result;
    }
    if (!is_safe_path_segment(entry.version)) {
        result.problems.push_back("the model version is not a safe path segment");
        return result;
    }
    if (!is_valid_sha256(entry.sha256)) {
        result.problems.push_back("no sha256: a download with nothing to check against is refused");
        return result;
    }
    if (entry.url.empty() && entry.mirror.empty()) {
        result.problems.push_back("no download URL (upstream or mirror)");
        return result;
    }
    if (model_filename(entry).empty()) {
        result.problems.push_back("the download URL names no file");
        return result;
    }

    const std::filesystem::path final_path = model_path(root, entry);

    // 2. Reuse a verified copy.
    if (installed(root, entry)) {
        result.installed = final_path;
        return result;
    }

    // 3. Stage beside the destination (same filesystem, so the rename is
    //    atomic).
    std::error_code ec;
    std::filesystem::create_directories(final_path.parent_path(), ec);
    if (ec) {
        result.problems.push_back("cannot create the model directory: " + ec.message());
        return result;
    }

    const std::filesystem::path staging = staging_path(final_path.parent_path(), entry);

    std::ofstream out(staging, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.problems.push_back("cannot open the partial file for writing");
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return result;
    }

    Sha256 hasher;
    std::uintmax_t written = 0;
    bool write_failed = false;
    bool oversize = false;

    const bool fetched = fetcher(entry, [&](std::span<const std::uint8_t> chunk) -> bool {
        // Refuse to write past the pinned size, so a lying upstream (or a
        // buggy fetcher) cannot balloon the partial file past what the
        // manifest promised. A size of 0 means "unpinned": no cap.
        if (entry.size != 0 && (written > entry.size || chunk.size() > entry.size - written)) {
            oversize = true;
            return false;
        }
        out.write(reinterpret_cast<const char *>(chunk.data()),
                  static_cast<std::streamsize>(chunk.size()));
        if (!out) {
            write_failed = true;
            return false;
        }
        hasher.update(chunk);
        written += chunk.size();
        if (progress) {
            progress(written, entry.size);
        }
        return true;
    });
    out.flush();
    out.close();

    if (oversize) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        result.problems.push_back("the download exceeds the pinned size ("
                                  + std::to_string(entry.size) + " bytes)");
        return result;
    }

    if (!fetched || write_failed) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        result.problems.push_back("the download did not complete");
        return result;
    }

    // 4. Digest check before the bytes are allowed to move into place.
    const std::array<std::uint8_t, 32> digest = hasher.digest();
    const std::string actual = hex_encode(digest);
    if (actual != entry.sha256) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        result.problems.push_back("digest mismatch: expected " + entry.sha256 + ", got " + actual);
        return result;
    }

    // 5. Move into place, then write the sidecar. A sidecar write that fails is
    //    not fatal: the model is already installed and verified.
    std::filesystem::remove(final_path, ec);
    ec.clear();
    std::filesystem::rename(staging, final_path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        result.problems.push_back("cannot move the model into place: " + ec.message());
        return result;
    }

    write_sidecar(final_path, entry.license, entry.sha256);

    result.installed = final_path;
    return result;
}

} // namespace genesis::ai
