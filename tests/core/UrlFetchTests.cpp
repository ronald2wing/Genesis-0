// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <filesystem>
#include <fstream>
#include <string>

#include "core/UrlFetch.h"
#include "../support/Checks.h"

using genesis::test::check;

int main()
{
    const std::filesystem::path fixture =
            std::filesystem::temp_directory_path() / "genesis-fetch-url-test.txt";
    {
        std::ofstream out(fixture, std::ios::binary | std::ios::trunc);
        out << "catalog-body";
    }

    std::string error;
    const std::optional<std::string> body =
            genesis::core::fetch_url("file://" + fixture.string(), error);
    check(body.has_value(), "a file URL fetches: " + error);
    check(body.has_value() && *body == "catalog-body", "the file URL body is returned");

    error.clear();
    const std::optional<std::string> missing =
            genesis::core::fetch_url("file://" + fixture.string() + ".missing", error);
    check(!missing.has_value(), "a missing file fails");
    check(!error.empty(), "a missing file fills an error");

    std::filesystem::remove(fixture);
    return genesis::test::summary();
}
