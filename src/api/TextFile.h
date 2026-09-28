// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The two file operations the API performs on a caller's behalf. They live
// apart from dispatch because they are the part with filesystem semantics -
// bounding, encoding, durability - and reading them should not mean scrolling
// through a nine-hundred-line switch.

#pragma once

#include <string>

namespace genesis::api {

// Reads `path` into `out` as UTF-8 text. The size is bounded by the document
// guard, so a binary or hostile file is refused before it is buffered whole,
// and the bytes must be well-formed UTF-8 (a reply carries them as a JSON
// string). Returns false on open, size, or validation failure.
bool read_text_file(const std::string &path, std::string &out);

// Replaces `path` with `text` atomically: a same-directory staging file is
// written, flushed to disk, and renamed onto the target, so a reader never sees
// a half-written file and a failure leaves the original intact.
bool write_text_atomic(const std::string &path, const std::string &text);

} // namespace genesis::api
