// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "extensions/RevokedList.h"
#include "../support/Checks.h"
#include "../support/Fs.h"

namespace {

using genesis::test::check;
using genesis::test::read_file;

namespace ext = genesis::extensions;

const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-revoked-list-tests";
const std::filesystem::path kStore = kBase / "store";

void test_empty_store_revokes_nothing()
{
    check(ext::read_revocations(kStore).empty(), "an empty store has no revocations");
    check(!ext::revoked_at(kStore, "ext.fixture", "1"), "and no id is revoked");
}

void test_replace_persists_and_round_trips()
{
    const ext::Revocation versionless{ "ext.fixture", std::nullopt, "malware" };
    const ext::Revocation versioned{ "ext.other", std::string("2"), std::nullopt };

    check(ext::replace_revocations(kStore, { versionless, versioned }),
          "replace saves successfully");
    check(ext::revoked_at(kStore, "ext.fixture", "1").has_value(),
          "a version-less entry matches any version");
    check(ext::revoked_at(kStore, "ext.fixture", "99").has_value(), "again, at another version");
    check(ext::revoked_at(kStore, "ext.other", "2").has_value(),
          "a versioned entry matches its version");
    check(!ext::revoked_at(kStore, "ext.other", "3"), "and no other version");
    check(!ext::revoked_at(kStore, "ext.missing", "1"), "an unrelated id is not revoked");

    const std::optional<ext::Revocation> found = ext::revoked_at(kStore, "ext.fixture", "1");
    check(found && found->reason && *found->reason == "malware",
          "the reason is carried through the round trip");

    const std::string on_disk = read_file(kStore / "revoked.json");
    check(on_disk.find("ext.fixture") != std::string::npos,
          "the id is written to the revoked.json file");
    check(on_disk.find("malware") != std::string::npos, "with its reason");
}

void test_replace_is_authoritative()
{
    const ext::Revocation only{ "ext.kept", std::nullopt, std::nullopt };
    check(ext::replace_revocations(kStore, { only }), "a later replace saves successfully");
    check(!ext::revoked_at(kStore, "ext.fixture", "1"),
          "an id the later catalog no longer revokes is cleared");
    check(ext::revoked_at(kStore, "ext.kept", "1").has_value(),
          "the surviving entry still matches");
}

void test_corrupt_file_reads_as_empty()
{
    std::filesystem::create_directories(kStore);
    {
        std::ofstream out(kStore / "revoked.json");
        out << "not json at all";
    }
    check(ext::read_revocations(kStore).empty(), "a corrupt revoked.json is treated as empty");
    check(!ext::revoked_at(kStore, "ext.fixture", "1"), "with nothing revoked");
}

void test_malformed_entries_are_skipped()
{
    std::filesystem::create_directories(kStore);
    {
        std::ofstream out(kStore / "revoked.json");
        out << R"({"revocations": [{"version": "1"}, "nope", {"id": "ext.ok"}]})";
    }
    const std::vector<ext::Revocation> list = ext::read_revocations(kStore);
    check(list.size() == 1, "only the well-formed entry survives");
    check(list.front().id == "ext.ok", "with the right id");
}

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);

    test_empty_store_revokes_nothing();
    test_replace_persists_and_round_trips();
    test_replace_is_authoritative();
    test_corrupt_file_reads_as_empty();
    test_malformed_entries_are_skipped();

    std::filesystem::remove_all(kBase);

    return genesis::test::summary();
}
