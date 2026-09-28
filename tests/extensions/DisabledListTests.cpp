// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "extensions/DisabledList.h"

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
        std::filesystem::temp_directory_path() / "genesis-disabled-list-tests";
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

void test_empty_store_disables_nothing()
{
    ext::DisabledList list(kStore);
    check(!list.disabled("ext.fixture"), "an empty store disables nothing");
    check(list.ids().empty(), "and the id set is empty");
}

void test_add_persists_and_round_trips()
{
    ext::DisabledList list(kStore);
    check(list.add("ext.fixture"), "add saves successfully");
    check(list.disabled("ext.fixture"), "the id is disabled in memory");
    check(list.ids().count("ext.fixture") == 1, "the id is in the set");

    const std::string on_disk = read_file(kStore / "disabled.json");
    check(on_disk.find("ext.fixture") != std::string::npos, "the id is written to the list file");

    // A fresh DisabledList over the same store sees the persisted list.
    ext::DisabledList reloaded(kStore);
    check(reloaded.disabled("ext.fixture"), "a reloaded list round-trips the id");
    check(!reloaded.disabled("ext.other"), "and nothing else is disabled");
}

void test_remove_clears_the_id()
{
    ext::DisabledList list(kStore);
    check(list.remove("ext.fixture"), "remove saves successfully");
    check(!list.disabled("ext.fixture"), "the id is no longer disabled");
    check(list.ids().count("ext.fixture") == 0, "and it left the set");

    ext::DisabledList reloaded(kStore);
    check(!reloaded.disabled("ext.fixture"), "the removed id stays gone after a reload");
}

void test_corrupt_file_reads_as_empty()
{
    std::filesystem::create_directories(kStore);
    {
        std::ofstream out(kStore / "disabled.json");
        out << "not json at all";
    }
    ext::DisabledList list(kStore);
    check(!list.disabled("ext.fixture"), "a corrupt list file is treated as empty");
    check(list.ids().empty(), "with an empty id set");
}

void test_reason_round_trips()
{
    ext::DisabledList list(kStore);
    check(list.add("ext.fixture", "requires dep.base"), "add with a reason saves successfully");
    check(list.reason("ext.fixture") == "requires dep.base", "the reason is recorded in memory");
    check(list.reason("ext.other").empty(), "an id off the list has no reason");

    ext::DisabledList reloaded(kStore);
    check(reloaded.reason("ext.fixture") == "requires dep.base",
          "a reloaded list round-trips the reason");
}

void test_reason_clears_on_direct_disable()
{
    ext::DisabledList list(kStore);
    check(list.add("ext.fixture", "requires dep.base"), "the reasoned add succeeds");
    check(list.add("ext.fixture"), "a direct disable re-adds without a reason");
    check(list.reason("ext.fixture").empty(), "the reason is erased by a direct disable");
    check(list.disabled("ext.fixture"), "the id stays disabled");

    ext::DisabledList reloaded(kStore);
    check(reloaded.reason("ext.fixture").empty(), "the erased reason stays gone after a reload");
}

void test_ids_only_file_reads_with_empty_reasons()
{
    // A list written by an older build (or by hand) carries only `ids`; every
    // reason reads empty.
    std::filesystem::create_directories(kStore);
    {
        std::ofstream out(kStore / "disabled.json");
        out << "{\"ids\": [\"ext.fixture\"]}\n";
    }
    ext::DisabledList list(kStore);
    check(list.disabled("ext.fixture"), "the ids-only list still disables");
    check(list.reason("ext.fixture").empty(), "and its reason reads empty");
}

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);

    test_empty_store_disables_nothing();
    test_add_persists_and_round_trips();
    test_remove_clears_the_id();
    test_corrupt_file_reads_as_empty();
    test_reason_round_trips();
    test_reason_clears_on_direct_disable();
    test_ids_only_file_reads_with_empty_reasons();

    std::filesystem::remove_all(kBase);

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
