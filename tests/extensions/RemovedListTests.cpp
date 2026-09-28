// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "extensions/RemovedList.h"

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

const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-removed-list-tests";
const std::filesystem::path kStore = kBase / "store";

// Reads the persisted list file back as raw text, for shape assertions.
std::string read_file(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::string out;
    std::string line;
    while (std::getline(in, line)) {
        out += line;
    }
    return out;
}

void test_empty_store_removes_nothing()
{
    check(!ext::removed(kStore, "ext.fixture"), "an empty store has no tombstone for the id");
    check(ext::ids(kStore).empty(), "and the id set is empty");
}

void test_remove_persists_and_round_trips()
{
    check(ext::remove(kStore, "ext.fixture"), "remove saves successfully");
    check(ext::removed(kStore, "ext.fixture"), "the id is tombstoned");
    check(ext::ids(kStore).count("ext.fixture") == 1, "the id is in the set");

    const std::string on_disk = read_file(kStore / "removed.json");
    check(on_disk.find("ext.fixture") != std::string::npos,
          "the id is written to the removed.json file");

    check(ext::removed(kStore, "ext.fixture"), "a fresh read of the store round-trips the id");
    check(!ext::removed(kStore, "ext.other"), "and nothing else is removed");
}

void test_restore_clears_the_id()
{
    check(ext::restore(kStore, "ext.fixture"), "restore saves successfully");
    check(!ext::removed(kStore, "ext.fixture"), "the id is no longer removed");
    check(ext::ids(kStore).count("ext.fixture") == 0, "and it left the set");

    check(!ext::removed(kStore, "ext.fixture"), "the restored id stays gone after a fresh read");
}

void test_corrupt_file_reads_as_empty()
{
    std::filesystem::create_directories(kStore);
    {
        std::ofstream out(kStore / "removed.json");
        out << "not json at all";
    }
    check(!ext::removed(kStore, "ext.fixture"), "a corrupt removed.json is treated as empty");
    check(ext::ids(kStore).empty(), "with an empty id set");
}

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);

    test_empty_store_removes_nothing();
    test_remove_persists_and_round_trips();
    test_restore_clears_the_id();
    test_corrupt_file_reads_as_empty();

    std::filesystem::remove_all(kBase);

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
