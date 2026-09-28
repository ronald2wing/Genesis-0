// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Shared document primitives: the scalar accessors, enum spellings, and ease codec.

#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <string>

#include "project/model/Clip.h"
#include "project/model/Cutout.h"
#include "project/model/Keyframe.h"
#include "project/model/Media.h"
#include "project/model/Text.h"

namespace genesis::project::detail {

using nlohmann::json;

// The on-disk spellings of the model's enums. These mirror the `// On disk:`
// comments in the project/model/*.h headers, which record each serde rename
// rule verbatim.

const char *media_kind_name(MediaKind kind);
const char *media_origin_name(MediaOrigin origin);
const char *clip_kind_name(ClipKind kind);
const char *color_range_name(ColorRange range);
const char *color_space_name(ColorSpace space);
const char *cutout_mode_name(CutoutMode mode);
const char *brush_tool_name(BrushTool tool);
const char *text_align_name(TextAlign align);

// KeyEase serialises as a bare array of its four control points, the
// document's spelling of what the engine plays as an Ease.
json write_ease(const KeyEase &ease);

// -- reader helpers ---------------------------------------------------------
//
// The standing rule: a hand-edited or older file degrades to something
// openable. A scalar that is absent keeps its default; one that is present
// with the wrong type fails the enclosing entry. A list keeps the entries
// that parse and drops the rest; a non-list is an empty one. An enum keeps a
// name it knows and falls back to the plainest one for a name it does not.

bool as_string(const json &value, std::string &out);
bool as_bool(const json &value, bool &out);
bool as_double(const json &value, double &out);
bool as_i64(const json &value, std::int64_t &out);
bool as_u32(const json &value, std::uint32_t &out);

// Absent keeps the default; present-but-wrong-type fails the entry.
bool take_string(const json &obj, const char *key, std::string &out);
bool take_bool(const json &obj, const char *key, bool &out);
bool take_double(const json &obj, const char *key, double &out);
bool take_u32(const json &obj, const char *key, std::uint32_t &out);

// Optional scalars: absent or null is none; anything else must parse.
bool take_opt_double(const json &obj, const char *key, std::optional<double> &out);
bool take_opt_u32(const json &obj, const char *key, std::optional<std::uint32_t> &out);
bool take_opt_string(const json &obj, const char *key, std::optional<std::string> &out);
bool take_opt_bool(const json &obj, const char *key, std::optional<bool> &out);

// Times: a JSON number is f64 seconds, a string the host's exact
// "num/den". Wrong type (or a non-finite number) fails the entry.
bool take_time(const json &obj, const char *key, Rational &out);
bool take_opt_time(const json &obj, const char *key, std::optional<Rational> &out);

// -- enums ------------------------------------------------------------------

MediaKind read_media_kind(const json &value);
ClipKind read_clip_kind(const json &value);
ColorSpace read_color_space(const json &value);
TextAlign read_text_align(const json &value);

// Optional enums: nothing for a name this build does not know.
std::optional<ColorRange> read_color_range(const json &value);
std::optional<MediaOrigin> read_media_origin(const json &value);
std::optional<CutoutMode> read_cutout_mode(const json &value);
std::optional<Subject> read_subject(const json &value);
std::optional<BrushTool> read_brush_tool(const json &value);

// KeyEase is total: a preset name or four numbers, anything else a straight
// line. Never the reason an entry is dropped.
KeyEase read_ease(const json &value);

} // namespace genesis::project::detail
