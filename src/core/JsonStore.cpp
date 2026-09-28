// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "core/JsonStore.h"

#include <fstream>
#include <string>
#include <system_error>

#include "core/AtomicWrite.h"
#include "core/JsonGuard.h"

namespace genesis::core {

std::optional<nlohmann::json> read_json_file(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    // The streaming parser stops reading at the document bound, so an
    // oversized file is refused without ever being buffered whole.
    std::string error;
    return parse_document_json(in, error);
}

bool write_json_atomic(const std::filesystem::path &path, const nlohmann::json &doc)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    return write_file_atomic(path, doc.dump(2) + '\n');
}

} // namespace genesis::core
