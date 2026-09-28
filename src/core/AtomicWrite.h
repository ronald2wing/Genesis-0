// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Crash-safe whole-file replacement: the bytes land in an exclusively created
// staging file beside the destination, are flushed to disk, and are renamed
// into place. A reader therefore sees the old file or the new one, never a
// half-written mix, and a crash never leaves the destination pointing at bytes
// the kernel had not written.

#pragma once

#include <filesystem>
#include <string_view>

namespace genesis::core {

// Replaces `path` with `bytes`. Returns false on any failure, in which case
// the destination is untouched and the staging file is removed. The parent
// directory must already exist.
bool write_file_atomic(const std::filesystem::path &path, std::string_view bytes);

} // namespace genesis::core
