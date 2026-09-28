// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "extensions/Sources.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

namespace ext = genesis::extensions;

// A scratch store under the system temp dir, cleared at the start of the run
// and removed at the end.
const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-sources-tests";
const std::filesystem::path kStore = kBase / "store";

void test_type_round_trip()
{
    check(std::string(ext::source_type_name(ext::SourceType::Dir)) == "dir", "Dir spells as 'dir'");
    check(std::string(ext::source_type_name(ext::SourceType::Git)) == "git", "Git spells as 'git'");
    check(std::string(ext::source_type_name(ext::SourceType::Builtin)) == "builtin",
          "Builtin spells as 'builtin'");

    check(ext::parse_source_type("dir") == ext::SourceType::Dir, "'dir' parses to Dir");
    check(ext::parse_source_type("git") == ext::SourceType::Git, "'git' parses to Git");
    check(ext::parse_source_type("builtin") == ext::SourceType::Builtin,
          "'builtin' parses to Builtin");
    check(!ext::parse_source_type("http"), "an unknown type does not parse");
    check(!ext::parse_source_type(""), "an empty type does not parse");
}

void test_absent_reads_empty()
{
    const auto sources = ext::read_sources(kStore);
    check(sources.empty(), "an absent sources.json reads as empty");
}

void test_record_and_read_round_trip()
{
    check(ext::record_source(kStore, "a.dev",
                             ext::InstallSource{ .type = ext::SourceType::Dir, .url = "/src/a" }),
          "a Dir source records");
    check(ext::record_source(kStore, "b.dev",
                             ext::InstallSource{ .type = ext::SourceType::Git,
                                                 .url = "https://example.com/b.git" }),
          "a Git source records");
    check(ext::record_source(kStore, "c.builtin",
                             ext::InstallSource{ .type = ext::SourceType::Builtin }),
          "a Builtin source records");

    const auto sources = ext::read_sources(kStore);
    check(sources.count("a.dev") == 1, "the Dir id is present");
    check(sources.count("b.dev") == 1, "the Git id is present");
    check(sources.count("c.builtin") == 1, "the Builtin id is present");

    if (sources.count("a.dev") == 1) {
        check(sources.at("a.dev").type == ext::SourceType::Dir, "the Dir id has type Dir");
        check(sources.at("a.dev").url == "/src/a", "and its url");
    }
    if (sources.count("b.dev") == 1) {
        check(sources.at("b.dev").type == ext::SourceType::Git, "the Git id has type Git");
        check(sources.at("b.dev").url == "https://example.com/b.git", "and its url");
    }
    if (sources.count("c.builtin") == 1) {
        check(sources.at("c.builtin").type == ext::SourceType::Builtin,
              "the Builtin id has type Builtin");
        check(sources.at("c.builtin").url.empty(), "and a Builtin record omits its url");
    }
}

void test_latest_record_wins()
{
    check(ext::record_source(kStore, "a.dev",
                             ext::InstallSource{ .type = ext::SourceType::Dir, .url = "/first" }),
          "the first record writes");
    check(ext::record_source(kStore, "a.dev",
                             ext::InstallSource{ .type = ext::SourceType::Dir, .url = "/second" }),
          "a later record overwrites");
    const auto sources = ext::read_sources(kStore);
    check(sources.count("a.dev") == 1, "still one id entry");
    if (sources.count("a.dev") == 1) {
        check(sources.at("a.dev").url == "/second", "the latest url wins");
    }
    // The other ids' records survive the read-modify-write.
    check(sources.count("b.dev") == 1 && sources.count("c.builtin") == 1,
          "the other ids' records survive");
}

void test_corrupt_reads_empty()
{
    {
        std::ofstream out(kStore / "sources.json");
        out << "{ not json";
    }
    check(ext::read_sources(kStore).empty(), "a corrupt sources.json reads as empty");
}

void test_unknown_type_reads_dir()
{
    {
        std::ofstream out(kStore / "sources.json");
        out << "{\"x.dev\": {\"type\": \"http\", \"url\": \"/x\"}}";
    }
    const auto sources = ext::read_sources(kStore);
    check(sources.count("x.dev") == 1, "the unknown-type id is still read");
    if (sources.count("x.dev") == 1) {
        check(sources.at("x.dev").type == ext::SourceType::Dir, "an unknown type reads as Dir");
    }
}

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);

    test_type_round_trip();
    test_absent_reads_empty();
    test_record_and_read_round_trip();
    test_latest_record_wins();
    test_corrupt_reads_empty();
    test_unknown_type_reads_dir();

    std::filesystem::remove_all(kBase);

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
