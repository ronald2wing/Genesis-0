// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <nlohmann/json.hpp>
#include <optional>

#include "project/command/Command.h"

namespace genesis::project::command {

// Encodes a command as the JSON the window and the wire share: a tagged `op`
// plus camelCase fields (`{"op": "addClip", "clipId": ..., ...}`). Every time
// field is written as the host's exact "num/den" string.
nlohmann::json encode(const Command &command);

// Decodes a command from that JSON. Fully strict: any malformed element (a
// wrong-typed scalar, an unknown enum spelling, a bad ease, a bad nested or
// list entry) rejects the whole command. This diverges from the document
// layer's tolerant list-dropping, which exists so a hand-edited file still
// opens; a command is a programmatic message with no "degrade gracefully"
// contract.
std::optional<Command> decode(const nlohmann::json &value);

} // namespace genesis::project::command
