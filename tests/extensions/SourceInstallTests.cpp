// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ai/Sha256.h"
#include "extensions/Install.h"
#include "extensions/SourceInstall.h"
#include "extensions/Sources.h"
#include "extensions/Subprocess.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace ext = genesis::extensions;

// A scratch area under the system temp dir, cleared at the start of the run and
// removed at the end.
const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-source-install-tests";
const std::filesystem::path kStore = kBase / "store";

// A minimal, valid native manifest for the given id.
std::string native_manifest(const std::string &id)
{
    return "[extension]\n"
           "id = \""
            + id
            + "\"\n"
              "name = \"Fixture\"\n"
              "version = 1\n"
              "api = 1\n"
              "entry = \"libfixture.so\"\n";
}

// Writes a native-extension directory (an extension.toml plus a placeholder
// library file) and returns it.
std::filesystem::path write_native(const std::string &name, const std::string &id)
{
    const std::filesystem::path dir = kBase / name;
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "extension.toml");
        out << native_manifest(id);
    }
    {
        std::ofstream out(dir / "libfixture.so");
        out << "placeholder";
    }
    return dir;
}

// Runs one git subprocess and returns its exit status, or -1 when git could not
// start or was killed. Used both to probe git availability and to build the
// local fixture repository.
int run_git(const std::vector<std::string> &argv)
{
    const ext::SubprocessResult run = ext::run_subprocess(argv);
    if (!run.started || !run.exited) {
        return -1;
    }
    return run.exit_status;
}

bool git_available()
{
    return run_git({ "git", "--version" }) == 0;
}

// Builds a local git repository holding one native extension, and returns its
// path. Returns an empty path when git is unavailable or the fixture cannot be
// committed.
std::filesystem::path make_git_fixture()
{
    const std::filesystem::path repo = kBase / "repo";
    std::filesystem::create_directories(repo);
    {
        std::ofstream out(repo / "extension.toml");
        out << native_manifest("git.fixture");
    }
    {
        std::ofstream out(repo / "libfixture.so");
        out << "placeholder";
    }
    if (run_git({ "git", "init", repo.string() }) != 0) {
        return { };
    }
    if (run_git({ "git", "-C", repo.string(), "add", "." }) != 0) {
        return { };
    }
    if (run_git({ "git", "-C", repo.string(), "-c", "user.email=test@example.com", "-c",
                  "user.name=Test", "commit", "-m", "fixture" })
        != 0) {
        return { };
    }
    return repo;
}

void test_dir_installs()
{
    const std::filesystem::path dir = write_native("dir", "dir.fixture");
    const ext::InstallResult result = ext::install_from_source(
            ext::InstallSource{ .type = ext::SourceType::Dir, .url = dir.string() }, kStore,
            ext::Origin::Developer);
    check(result.ok(), "a Dir source with an existing path installs");
    check(ext::installed_packs(kStore).count("dir.fixture") == 1,
          "the Dir extension lands in the store");
}

void test_dir_missing_refused()
{
    const ext::InstallResult result = ext::install_from_source(
            ext::InstallSource{ .type = ext::SourceType::Dir, .url = "/nonexistent/dir" }, kStore,
            ext::Origin::Developer);
    check(!result.ok(), "a Dir source with a missing path is refused");
}

void test_builtin_refused()
{
    const ext::InstallResult result = ext::install_from_source(
            ext::InstallSource{ .type = ext::SourceType::Builtin }, kStore, ext::Origin::Builtin);
    check(!result.ok(), "a Builtin source is refused");
}

void test_git_installs()
{
    if (!git_available()) {
        std::printf("note: git not found; skipping the git install test\n");
        return;
    }
    const std::filesystem::path repo = make_git_fixture();
    if (repo.empty()) {
        std::printf("note: could not build the git fixture; skipping\n");
        return;
    }
    const ext::InstallResult result = ext::install_from_source(
            ext::InstallSource{ ext::SourceType::Git, repo.string(), "", "" }, kStore,
            ext::Origin::Developer);
    check(result.ok(), "a Git source clones and installs");
    check(ext::installed_packs(kStore).count("git.fixture") == 1,
          "the cloned extension lands in the store");

    // The recorded source is the original repository path, not the temporary
    // clone, so a config export stays reproducible.
    const auto sources = ext::read_sources(kStore);
    check(sources.count("git.fixture") == 1, "the git install records a source");
    if (sources.count("git.fixture") == 1) {
        check(sources.at("git.fixture").type == ext::SourceType::Git, "the recorded source is Git");
        check(sources.at("git.fixture").url == repo.string(),
              "the recorded url is the original repository");
    }
}

void test_git_missing_url_refused()
{
    if (!git_available()) {
        std::printf("note: git not found; skipping the git refusal test\n");
        return;
    }
    const ext::InstallResult result = ext::install_from_source(
            ext::InstallSource{ ext::SourceType::Git, "/nonexistent/repository", "", "" }, kStore,
            ext::Origin::Developer);
    check(!result.ok(), "a Git source with an unreachable path is refused");
}

// Writes a directory tree of a known shape (a top-level file and a nested one)
// and returns it, for the tree-digest convention tests.
std::filesystem::path write_tree(const std::string &name)
{
    const std::filesystem::path dir = kBase / name;
    std::filesystem::create_directories(dir / "sub");
    {
        std::ofstream out(dir / "a.txt");
        out << "hello";
    }
    {
        std::ofstream out(dir / "sub" / "b.txt");
        out << "world";
    }
    return dir;
}

// The number of `.genesis-git-*` clone directories currently in the temp dir,
// so a digest refusal can prove it left no clone behind.
int count_clone_dirs()
{
    int count = 0;
    std::error_code ec;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(std::filesystem::temp_directory_path(), ec)) {
        if (entry.path().filename().string().starts_with(".genesis-git-")) {
            ++count;
        }
    }
    return count;
}

void test_tree_sha256_determinism()
{
    const std::filesystem::path a = write_tree("tree-a");
    const std::filesystem::path b = kBase / "tree-b";
    std::filesystem::copy(a, b, std::filesystem::copy_options::recursive);
    const std::optional<std::string> digest_a = ext::tree_sha256(a);
    const std::optional<std::string> digest_b = ext::tree_sha256(b);
    check(digest_a.has_value() && digest_b.has_value(), "both trees digest");
    if (digest_a && digest_b) {
        check(*digest_a == *digest_b, "the digest is independent of the tree's location");
    }
}

void test_tree_sha256_convention()
{
    // The documented convention: sorted relative paths (UTF-8, `/` separator)
    // each followed by a NUL byte, then the file bytes. Build the expected byte
    // stream and hash it directly, so the test pins the convention without a
    // magic constant.
    const std::filesystem::path dir = write_tree("tree-conv");
    const std::string stream = std::string("a.txt") + '\0' + "hello" + "sub/b.txt" + '\0' + "world";
    const std::string expected = genesis::ai::sha256_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t *>(stream.data()), stream.size()));
    const std::optional<std::string> actual = ext::tree_sha256(dir);
    check(actual.has_value(), "the tree digests");
    if (actual) {
        check(*actual == expected, "the digest matches the documented convention");
    }
}

void test_tree_sha256_differs_on_content()
{
    const std::filesystem::path dir = write_tree("tree-diff");
    const std::optional<std::string> before = ext::tree_sha256(dir);
    {
        std::ofstream out(dir / "a.txt");
        out << "changed";
    }
    const std::optional<std::string> after = ext::tree_sha256(dir);
    check(before.has_value() && after.has_value(), "the tree digests before and after");
    if (before && after) {
        check(*before != *after, "a content change changes the digest");
    }
}

void test_dir_digest_match_installs()
{
    const std::filesystem::path dir = write_native("dir-digest-ok", "dir.digest");
    const std::optional<std::string> digest = ext::tree_sha256(dir);
    const ext::InstallResult result = ext::install_from_source(
            ext::InstallSource{ .type = ext::SourceType::Dir, .url = dir.string() }, kStore,
            ext::Origin::Developer, digest);
    check(result.ok(), "a Dir source whose digest matches installs");
    check(ext::installed_packs(kStore).count("dir.digest") == 1, "and lands in the store");
}

void test_dir_digest_mismatch_refused()
{
    const std::filesystem::path dir = write_native("dir-digest-bad", "dir.digestbad");
    const ext::InstallResult result = ext::install_from_source(
            ext::InstallSource{ .type = ext::SourceType::Dir, .url = dir.string() }, kStore,
            ext::Origin::Developer, std::string(64, '0'));
    check(!result.ok(), "a Dir source whose digest does not match is refused");
    check(ext::installed_packs(kStore).count("dir.digestbad") == 0, "and nothing lands");
}

void test_malformed_expected_digest_refused()
{
    const std::filesystem::path dir = write_native("dir-digest-mal", "dir.digestmal");
    const ext::InstallResult result = ext::install_from_source(
            ext::InstallSource{ .type = ext::SourceType::Dir, .url = dir.string() }, kStore,
            ext::Origin::Developer, std::string("not-hex"));
    check(!result.ok(), "a malformed expected digest is refused before any source work");
}

void test_git_digest_mismatch_cleans_up()
{
    if (!git_available()) {
        std::printf("note: git not found; skipping the git digest test\n");
        return;
    }
    const std::filesystem::path repo = make_git_fixture();
    if (repo.empty()) {
        std::printf("note: could not build the git fixture; skipping\n");
        return;
    }
    const int before = count_clone_dirs();
    const ext::InstallResult result = ext::install_from_source(
            ext::InstallSource{ ext::SourceType::Git, repo.string(), "", "" }, kStore,
            ext::Origin::Developer, std::string(64, '0'));
    check(!result.ok(), "a Git source whose digest does not match is refused");
    check(ext::installed_packs(kStore).count("git.fixture") == 0, "and nothing lands");
    check(count_clone_dirs() == before, "and the temporary clone is removed");
}

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);

    test_dir_installs();
    test_dir_missing_refused();
    test_builtin_refused();
    test_git_installs();
    test_git_missing_url_refused();
    test_tree_sha256_determinism();
    test_tree_sha256_convention();
    test_tree_sha256_differs_on_content();
    test_dir_digest_match_installs();
    test_dir_digest_mismatch_refused();
    test_malformed_expected_digest_refused();
    test_git_digest_mismatch_cleans_up();

    std::filesystem::remove_all(kBase);

    return genesis::test::summary();
}
