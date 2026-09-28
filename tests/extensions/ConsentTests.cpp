// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "extensions/Consent.h"
#include "../support/Checks.h"
#include "../support/Fs.h"

namespace {

using genesis::test::check;
using genesis::test::read_file;

namespace ext = genesis::extensions;

const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-consent-tests";
const std::filesystem::path kStore = kBase / "store";

void test_empty_store_has_no_consent()
{
    ext::Consent consent(kStore);
    check(!consent.consented("ext.dev"), "an empty store consents nothing");
    check(consent.ids().empty(), "and the id set is empty");
}

void test_grant_persists_and_round_trips()
{
    ext::Consent consent(kStore);
    check(consent.grant("ext.dev"), "grant saves successfully");
    check(consent.consented("ext.dev"), "the id is consented in memory");
    check(consent.ids().count("ext.dev") == 1, "the id is in the set");

    const std::string on_disk = read_file(kStore / "developer-consent.json");
    check(on_disk.find("ext.dev") != std::string::npos, "the id is written to the consent file");

    // A fresh Consent over the same store sees the persisted list.
    ext::Consent reloaded(kStore);
    check(reloaded.consented("ext.dev"), "a reloaded Consent round-trips the id");
    check(!reloaded.consented("ext.other"), "and nothing else is consented");
}

void test_revoke_removes_the_id()
{
    ext::Consent consent(kStore);
    check(consent.revoke("ext.dev"), "revoke saves successfully");
    check(!consent.consented("ext.dev"), "the id is no longer consented");
    check(consent.ids().count("ext.dev") == 0, "and it left the set");

    ext::Consent reloaded(kStore);
    check(!reloaded.consented("ext.dev"), "the revoked id stays gone after a reload");
}

void test_corrupt_file_reads_as_empty()
{
    std::filesystem::create_directories(kStore);
    {
        std::ofstream out(kStore / "developer-consent.json");
        out << "not json at all";
    }
    ext::Consent consent(kStore);
    check(!consent.consented("ext.dev"), "a corrupt consent file is treated as empty");
    check(consent.ids().empty(), "with an empty id set");
}

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);

    test_empty_store_has_no_consent();
    test_grant_persists_and_round_trips();
    test_revoke_removes_the_id();
    test_corrupt_file_reads_as_empty();

    std::filesystem::remove_all(kBase);

    return genesis::test::summary();
}
