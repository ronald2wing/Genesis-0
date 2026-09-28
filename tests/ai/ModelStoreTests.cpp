// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ai/ModelManifest.h"
#include "ai/ModelStore.h"
#include "ai/Sha256.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

bool contains(const std::vector<std::string> &problems, const std::string &needle)
{
    for (const std::string &problem : problems) {
        if (problem.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

namespace ai = genesis::ai;

// A per-run sandbox under the system temp dir, emptied first so a crashed run
// cannot leak a fresh-looking file into the assertions.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-model-store-tests";
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
        return p;
    }();
    return dir;
}

const std::string kPayload = "genesis-0 model store: a synthetic weight blob "
                             "for a hermetic install test.";

std::span<const std::uint8_t> bytes(const std::string &text)
{
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()),
                                         text.size());
}

const std::string kDigest = ai::sha256_hex(bytes(kPayload));

// A valid entry whose digest matches kPayload.
ai::ModelEntry entry(std::string sha256 = kDigest)
{
    ai::ModelEntry e;
    e.id = "test.model";
    e.version = "1";
    e.url = "https://example.invalid/models/test-model.bin";
    e.sha256 = std::move(sha256);
    e.license = "MIT";
    e.size = kPayload.size();
    return e;
}

// A fetcher that feeds `payload` in one chunk and reports how often it ran.
ai::Fetcher feed(std::string payload, int *calls = nullptr)
{
    return [payload, calls](const ai::ModelEntry &,
                            const std::function<bool(std::span<const std::uint8_t>)> &sink) {
        if (calls != nullptr) {
            ++*calls;
        }
        return sink(bytes(payload));
    };
}

// A fetcher that feeds a prefix then reports failure, modelling a dropped
// connection mid-download.
ai::Fetcher interrupt_after(std::string payload, std::size_t prefix)
{
    return [payload, prefix](const ai::ModelEntry &,
                             const std::function<bool(std::span<const std::uint8_t>)> &sink) {
        (void)sink(bytes(payload).first(prefix));
        return false;
    };
}

std::string read_all(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool has_staging(const std::filesystem::path &dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return false;
    }
    for (const std::filesystem::directory_entry &item :
         std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (item.path().filename().string().starts_with(".tmp-")) {
            return true;
        }
    }
    return false;
}

void test_sha256_nist_vectors()
{
    check(ai::sha256_hex(bytes(""))
                  == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 of the empty string");
    check(ai::sha256_hex(bytes("abc"))
                  == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 of \"abc\"");
    check(ai::sha256_hex(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
                  == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256 of the 56-byte two-block vector");
    const std::string million(1000000, 'a');
    check(ai::sha256_hex(bytes(million))
                  == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "SHA-256 of one million 'a's");

    // Streaming equals one-shot: feed the same bytes in odd-sized chunks.
    ai::Sha256 stream;
    const std::string text = "chunked streaming over a block boundary x";
    for (std::size_t i = 0; i < text.size(); i += 7) {
        const std::string chunk = text.substr(i, 7);
        stream.update(bytes(chunk));
    }
    const std::array<std::uint8_t, 32> digest = stream.digest();
    std::string hex(64, '0');
    static constexpr char kDigits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < digest.size(); ++i) {
        hex[2 * i] = kDigits[digest[i] >> 4];
        hex[2 * i + 1] = kDigits[digest[i] & 0xf];
    }
    check(hex == ai::sha256_hex(bytes(text)), "streamed updates equal the one-shot digest");
}

void test_catalogue_rows()
{
    const auto catalogue = ai::model_catalogue();
    check(catalogue.size() == 8, "the catalogue names eight models");

    const std::string ids[] = { "whisper",         "rvm",         "isnet",  "slimsam-encoder",
                                "slimsam-decoder", "real-esrgan", "kokoro", "pocket-tts" };
    for (const std::string &id : ids) {
        bool found = false;
        for (const ai::ModelEntry &row : catalogue) {
            if (row.id == id) {
                found = true;
                break;
            }
        }
        check(found, "the catalogue names " + id);
    }

    // Each named candidate records its weights' licence (ai-provider.md §5).
    for (const ai::ModelEntry &row : catalogue) {
        if (row.id == "whisper") {
            check(row.license == "MIT", "whisper weights are MIT");
        } else if (row.id == "rvm") {
            check(row.license == "GPL-3.0", "RVM weights are GPL-3.0");
        } else if (row.id == "isnet" || row.id == "slimsam-encoder" || row.id == "slimsam-decoder"
                   || row.id == "kokoro") {
            check(row.license == "Apache-2.0", row.id + " weights are Apache-2.0");
        } else if (row.id == "pocket-tts") {
            check(row.license == "CC-BY-4.0", "PocketTTS weights are CC-BY-4.0");
        } else if (row.id == "real-esrgan") {
            check(row.license == "BSD-3-Clause", "Real-ESRGAN weights are BSD-3-Clause");
        }
    }

    // No row may carry a malformed digest: a present digest is exactly 64
    // lowercase hex digits.
    for (const ai::ModelEntry &row : catalogue) {
        if (row.sha256.empty()) {
            continue;
        }
        bool well_formed = row.sha256.size() == 64;
        for (const char c : row.sha256) {
            const bool hex_digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            if (!hex_digit) {
                well_formed = false;
            }
        }
        check(well_formed, "a present digest is well-formed hex");
    }
}

void test_no_digest_refused()
{
    const std::filesystem::path root = sandbox() / "no-digest";
    const ai::InstallResult result = ai::install(root, entry(""), feed(kPayload));
    check(!result.ok(), "an entry with no sha256 is refused");
    check(contains(result.problems, "no sha256"), "the reason names the missing digest");
    check(!std::filesystem::exists(root / "test.model"),
          "the store gains no id directory for a refused entry");
}

void test_malformed_digest_refused()
{
    const std::filesystem::path root = sandbox() / "malformed";
    const ai::InstallResult result = ai::install(root, entry("0123456789abcdef"), feed(kPayload));
    check(!result.ok(), "an entry with a short sha256 is refused");
    check(contains(result.problems, "no sha256"), "the reason names the unusable digest");
}

void test_mismatch_refused_and_partial_deleted()
{
    const std::filesystem::path root = sandbox() / "mismatch";
    const ai::ModelEntry e = entry();
    const ai::InstallResult result =
            ai::install(root, e, feed("bytes that do not hash to the pinned digest"));
    check(!result.ok(), "a mismatched download is refused");
    check(contains(result.problems, "mismatch"), "the reason names the digest mismatch");
    check(!std::filesystem::exists(ai::model_path(root, e)), "no model file lands on a mismatch");
    check(!has_staging(ai::model_path(root, e).parent_path()),
          "the partial file is deleted on a mismatch");
}

void test_successful_install()
{
    const std::filesystem::path root = sandbox() / "ok";
    const ai::ModelEntry e = entry();
    const ai::InstallResult result = ai::install(root, e, feed(kPayload));

    check(result.ok(), "a matching download installs");
    check(result.installed.has_value(), "and returns its path");
    if (!result.installed) {
        return;
    }
    const std::filesystem::path file = *result.installed;
    check(file == ai::model_path(root, e), "the path matches model_path");
    check(std::filesystem::is_regular_file(file), "the model file exists");
    check(read_all(file) == kPayload, "the model file holds the exact bytes");

    const std::filesystem::path sidecar = std::filesystem::path(file.string() + ".meta");
    check(std::filesystem::is_regular_file(sidecar), "a sidecar is written");
    const std::string meta = read_all(sidecar);
    check(meta.find("license=MIT") != std::string::npos, "the sidecar records the licence");
    check(meta.find("sha256=" + kDigest) != std::string::npos, "the sidecar records the digest");

    check(ai::installed(root, e), "the copy reads back as installed");
}

void test_reinstall_reuses_verified_copy()
{
    const std::filesystem::path root = sandbox() / "reuse";
    const ai::ModelEntry e = entry();

    int calls = 0;
    check(ai::install(root, e, feed(kPayload, &calls)).ok(), "the first install succeeds");
    check(calls == 1, "the first install fetched once");

    const ai::InstallResult again = ai::install(root, e, feed(kPayload, &calls));
    check(again.ok(), "re-installing a verified copy succeeds");
    check(calls == 1, "the fetcher is not called again");
    check(again.installed == ai::model_path(root, e), "the reused copy is the same path");
}

void test_interrupted_install_leaves_nothing()
{
    const std::filesystem::path root = sandbox() / "interrupted";
    const ai::ModelEntry e = entry();
    const ai::InstallResult result = ai::install(root, e, interrupt_after(kPayload, 10));

    check(!result.ok(), "an interrupted download is refused");
    check(contains(result.problems, "did not complete"),
          "the reason names the incomplete download");
    check(!std::filesystem::exists(ai::model_path(root, e)),
          "no model file lands on an interruption");
    check(!has_staging(ai::model_path(root, e).parent_path()),
          "the partial file is deleted on an interruption");
    check(!ai::installed(root, e), "and nothing reads back as installed");
}

void test_installed_digest_check()
{
    const std::filesystem::path root = sandbox() / "verify";
    const ai::ModelEntry e = entry();
    check(ai::install(root, e, feed(kPayload)).ok(), "the install succeeds");
    check(ai::installed(root, e), "the verified copy is installed");

    const std::filesystem::path file = ai::model_path(root, e);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "replaced";
    }
    check(!ai::installed(root, e), "a replaced file no longer reads as installed");

    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << kPayload;
    }
    check(ai::installed(root, e), "restoring the exact bytes reads installed");

    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
    }
    check(!ai::installed(root, e), "a truncated file reads not installed");
}

void test_model_path_shape()
{
    const ai::ModelEntry e = entry();
    const std::filesystem::path a = ai::model_path("/models", e);
    const std::filesystem::path b = ai::model_path("/models", e);
    check(a == b, "model_path is deterministic");
    check(a == std::filesystem::path("/models/test.model/1/test-model.bin"),
          "model_path is <root>/<id>/<version>/<filename>");
}

void test_oversize_download_aborted()
{
    const std::filesystem::path root = sandbox() / "oversize";
    ai::ModelEntry e = entry(); // sha256 matches kPayload; only the size is lied about
    e.size = 5;
    const ai::InstallResult result = ai::install(root, e, feed(kPayload));

    check(!result.ok(), "a download exceeding the pinned size is aborted");
    check(contains(result.problems, "exceeds"), "the reason names the oversize");
    check(!std::filesystem::exists(ai::model_path(root, e)),
          "no model file lands on an oversize download");
    check(!has_staging(ai::model_path(root, e).parent_path()),
          "the partial file is deleted on an oversize download");
    check(!ai::installed(root, e), "nothing reads back as installed");
}

void test_unsafe_segments_rejected()
{
    // An id or version that would escape the model root is refused before any
    // directory or file is created.
    const struct
    {
        std::string id;
        std::string version;
        std::string why;
    } cases[] = {
        { "../evil", "1", "id with .." },
        { "a/b", "1", "id with a slash" },
        { "a\\b", "1", "id with a backslash" },
        { "test.model", "../1", "version with .." },
        { "test.model", "a/b", "version with a slash" },
        { "test.model", "a\\b", "version with a backslash" },
    };
    for (const auto &c : cases) {
        ai::ModelEntry e = entry();
        e.id = c.id;
        e.version = c.version;
        const ai::InstallResult result = ai::install(sandbox() / "unsafe", e, feed(kPayload));
        check(!result.ok(), c.why + " is refused");
        check(contains(result.problems, "path segment"), c.why + " names the unsafe segment");
    }
    check(!std::filesystem::exists(sandbox() / "unsafe" / ".."),
          "no directory escapes the model root");
}

void test_concurrent_installs_of_same_entry()
{
    const std::filesystem::path root = sandbox() / "concurrent";
    const ai::ModelEntry e = entry();

    // A ready/go handshake releases the threads as close together as possible,
    // then each installs the same entry into the same root. Each stages to its
    // own unique partial (pid + counter) and renames atomically, so a verified
    // copy always wins and no thread corrupts another's bytes.
    constexpr int kThreads = 8;
    std::atomic<int> ready{ 0 };
    std::atomic<bool> go{ false };
    std::vector<int> ok(kThreads, 0);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) { }
            const ai::InstallResult result = ai::install(root, e, feed(kPayload));
            ok[t] = result.ok() ? 1 : 0;
        });
    }
    while (ready.load(std::memory_order_acquire) < kThreads) { }
    go.store(true, std::memory_order_release);
    for (std::thread &thread : threads) {
        thread.join();
    }

    for (int t = 0; t < kThreads; ++t) {
        check(ok[t] == 1, "a concurrent install of the same entry succeeds");
    }
    check(ai::installed(root, e), "the concurrent installs leave a verified copy");
    check(!has_staging(ai::model_path(root, e).parent_path()),
          "no partial file survives the concurrent installs");
    check(read_all(ai::model_path(root, e)) == kPayload,
          "the surviving model file holds the exact bytes");
}

} // namespace

int main()
{
    test_sha256_nist_vectors();
    test_catalogue_rows();
    test_no_digest_refused();
    test_malformed_digest_refused();
    test_mismatch_refused_and_partial_deleted();
    test_successful_install();
    test_reinstall_reuses_verified_copy();
    test_interrupted_install_leaves_nothing();
    test_installed_digest_check();
    test_model_path_shape();
    test_oversize_download_aborted();
    test_unsafe_segments_rejected();
    test_concurrent_installs_of_same_entry();

    std::filesystem::remove_all(sandbox());

    return genesis::test::summary();
}
