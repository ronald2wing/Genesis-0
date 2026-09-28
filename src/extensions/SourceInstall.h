// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "extensions/Install.h"

namespace genesis::extensions {

// The deterministic content digest of a directory tree, as 64 lowercase hex
// digits. It is the value a catalog entry's `sha256` pins for a `dir`/`git`
// source, and the convention is fixed so a publisher and this verifier agree on
// what was signed even when the tree is re-created on another machine:
//
//   - Only regular files contribute bytes; a directory is implied by the paths
//     of the files under it, and symlinks and other non-regular entries are
//     ignored (they are not content).
//   - Each file contributes its path relative to the tree root (UTF-8, `/` as
//     the separator) followed by a single NUL byte, then its raw contents.
//   - Files are fed in byte-wise lexicographic order of those relative paths,
//     so the digest is independent of directory enumeration order.
//
// The digest of an empty tree is the SHA-256 of the empty byte string. Returns
// nullopt when the path is not a directory or any file cannot be read - in
// which case no digest can be computed and a verifying caller must refuse.
std::optional<std::string> tree_sha256(const std::filesystem::path &dir);

// Installs the extension named by `source`, whose type decides how the install
// directory is obtained before the ordinary install path runs:
//   - Dir: the `url` must name an existing directory; install runs directly.
//   - Git: clone `url` (optionally at `ref`) into a private temporary
//     directory, resolve `subdir` under it, and install from there.
//   - Builtin: refused (a builtin re-seeds on startup; nothing to reproduce).
// The trust gate and the source recording are install_extension's unchanged:
// a Dir install records the absolute path, a Git install records the original
// url/ref/subdir (never the temporary clone), so a config export stays
// reproducible. A Git source requires the `git` binary on PATH; a missing
// binary, a refused clone, or an unsafe/missing subdir is a refusal, reported
// in `problems`, never a process error.
//
// `expected_sha256`, when set, is the content digest the source must match (a
// catalog entry's `sha256`): a `dir` install digests the directory itself, a
// `git` install digests the checked-out tree at the resolved `subdir`. A
// malformed expected digest, or a computed digest that does not match it,
// refuses the install before anything is copied; for `git` the temporary clone
// is removed on that refusal. When it is unset nothing is checked.
InstallResult install_from_source(const InstallSource &source,
                                  const std::filesystem::path &store_root, Origin origin,
                                  const std::optional<std::string> &expected_sha256 = std::nullopt);

} // namespace genesis::extensions
