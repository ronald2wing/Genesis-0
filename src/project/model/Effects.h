// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/model.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// Effects model: applied filters and effects with their keyed parameters.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Animation.h"

#include "project/model/Keyframe.h"

namespace genesis::project {

// One key on one parameter of an applied effect; see AppliedFilter::keys.
// On disk: camelCase.
struct ParamKey
{
    // Where in the clip, as a fraction of its timeline length, 0..=1.
    double at = 0.0;
    // The value there, in the parameter's own units.
    double value = 0.0;
    // How this key is approached from the previous one. Absent means linear.
    KeyEase ease;

    bool operator==(const ParamKey &) const = default;
};

// One applied audio filter or video effect: a catalogue id plus whatever
// parameters were set. The catalogues themselves (and the FFmpeg strings
// they build) live in the UI today; the model only stores the numbers.
// On disk: camelCase.
struct AppliedFilter
{
    // Which catalogue entry this is, e.g. "sepia". Not minted; an id the
    // catalogue no longer knows is simply skipped at render time.
    std::string id;
    // The user's knob settings, keyed by parameter name. Missing keys mean
    // the catalogue's defaults; ordered so serialisation is stable.
    std::map<std::string, double> params;
    // False bypasses without losing settings. Absent means enabled.
    bool enabled = true;
    // The knobs that ride rather than hold: a sorted run of keys per
    // parameter name, each `at` a fraction of the clip like a ClipKey's.
    // A parameter with keys is played from them and its `params` entry is
    // only what it falls back to with the keys taken off. Absent, and left
    // out of the document, when empty.
    std::map<std::string, std::vector<ParamKey>> keys;

    AppliedFilter() = default;

    // A link with nothing set: the package's defaults, enabled.
    static AppliedFilter create(std::string id)
    {
        AppliedFilter filter;
        filter.id = std::move(id);
        return filter;
    }

    // This parameter's keys, in order; empty for one that holds still.
    //
    // The span points into this filter's own storage, so it is valid only
    // until the filter changes - the same borrow Rust hands callers.
    std::span<const ParamKey> keys_on(std::string_view key) const
    {
        const auto it = keys.find(std::string(key));
        if (it == keys.end()) {
            return { };
        }
        return it->second;
    }

    // Whether this parameter rides at all.
    bool is_keyed(std::string_view key) const { return !keys_on(key).empty(); }

    // The index into this parameter's keys of the one at `at`, within
    // KEY_EPSILON, choosing the nearest when two are in reach.
    std::optional<std::size_t> key_at(std::string_view key, double at) const
    {
        const std::span<const ParamKey> run = keys_on(key);
        std::optional<std::size_t> best;
        double best_dist = 0.0;
        for (std::size_t index = 0; index < run.size(); ++index) {
            const double dist = std::abs(run[index].at - at);
            if (dist > KEY_EPSILON) {
                continue;
            }
            if (!best || dist < best_dist) {
                best = index;
                best_dist = dist;
            }
        }
        return best;
    }

    // This parameter's keys as the engine plays them.
    genesis::core::KeyTrack track_on(std::string_view key) const
    {
        std::vector<genesis::core::Key> held_keys;
        for (const ParamKey &held : keys_on(key)) {
            held_keys.push_back(genesis::core::Key{
                    held.at,
                    held.value,
                    static_cast<genesis::core::Ease>(held.ease),
            });
        }
        return genesis::core::KeyTrack(std::move(held_keys));
    }

    // What a parameter is worth at `at`: its ride where it is keyed, and
    // otherwise what `params` holds, or `fallback` - the package's default,
    // which the model does not know - when that holds nothing either.
    double value_at(std::string_view key, double at, double fallback) const
    {
        double constant = fallback;
        const auto it = params.find(std::string(key));
        if (it != params.end()) {
            constant = it->second;
        }
        if (!is_keyed(key)) {
            return constant;
        }
        return track_on(key).value_at(at, constant);
    }

    // Every set parameter at `at`: `params` with each keyed one replaced
    // by its ride's value there. What a renderer resolves a frame from.
    std::map<std::string, double> params_at(double at) const
    {
        std::map<std::string, double> set = params;
        for (const auto &[key, held] : keys) {
            if (held.empty()) {
                continue;
            }
            const auto it = params.find(key);
            const double rest = it != params.end() ? it->second : 0.0;
            set[key] = track_on(key).value_at(at, rest);
        }
        return set;
    }

    // The nearest key strictly before `at`, and the nearest strictly after;
    // see `Clip::keys_around`.
    std::pair<std::optional<double>, std::optional<double>> keys_around(std::string_view key,
                                                                        double at) const
    {
        std::optional<double> before;
        std::optional<double> after;
        for (const ParamKey &held : keys_on(key)) {
            if (held.at < at - KEY_EPSILON) {
                before = before ? std::max(*before, held.at) : held.at;
            } else if (held.at > at + KEY_EPSILON) {
                after = after ? std::min(*after, held.at) : held.at;
            }
        }
        return { before, after };
    }

    // Sets a key, replacing whichever key on that parameter was already
    // within KEY_EPSILON of `at`. Keeps the run sorted by `at`.
    void set_key(std::string_view key, double at, double value, KeyEase ease)
    {
        if (!std::isfinite(at) || !std::isfinite(value)) {
            return;
        }
        at = std::clamp(at, 0.0, 1.0);
        const std::optional<std::size_t> slot = key_at(key, at);
        auto &run = keys[std::string(key)];
        if (slot) {
            run[*slot] = ParamKey{ at, value, ease };
        } else {
            run.push_back(ParamKey{ at, value, ease });
        }
        std::sort(run.begin(), run.end(),
                  [](const ParamKey &a, const ParamKey &b) { return a.at < b.at; });
    }

    // Removes this parameter's key at `at`, if there is one. True when a
    // key actually went; the run goes with its last key.
    bool clear_key(std::string_view key, double at)
    {
        const std::optional<std::size_t> index = key_at(key, at);
        if (!index) {
            return false;
        }
        const auto it = keys.find(std::string(key));
        if (it != keys.end()) {
            it->second.erase(it->second.begin() + *index);
            if (it->second.empty()) {
                keys.erase(it);
            }
        }
        return true;
    }

    // Takes every key off one parameter. True when there were any.
    bool clear_keys(std::string_view key)
    {
        const auto it = keys.find(std::string(key));
        if (it == keys.end()) {
            return false;
        }
        const bool had_keys = !it->second.empty();
        keys.erase(it);
        return had_keys;
    }

    // Drops keys that are not finite or not in 0..=1, empty runs with
    // them, and orders what is left. For the document reader.
    void sort_keys()
    {
        for (auto &entry : keys) {
            std::vector<ParamKey> &run = entry.second;
            run.erase(std::remove_if(run.begin(), run.end(),
                                     [](const ParamKey &k) {
                                         return !std::isfinite(k.at) || !std::isfinite(k.value)
                                                 || k.at < 0.0 || k.at > 1.0;
                                     }),
                      run.end());
            std::sort(run.begin(), run.end(),
                      [](const ParamKey &a, const ParamKey &b) { return a.at < b.at; });
        }
        std::erase_if(keys, [](const auto &entry) { return entry.second.empty(); });
    }

    bool operator==(const AppliedFilter &) const = default;
};

} // namespace genesis::project
