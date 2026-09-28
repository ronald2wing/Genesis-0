// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/ReverseController.h"

#include <optional>
#include <utility>
#include <vector>

#include "adapters/engine/ges/media/Reverse.h"
#include "project/Project.h"
#include "render/Flatten.h"

namespace ges = genesis::adapters::engine::ges;
namespace render = genesis::render;

ReverseController::ReverseController(QObject *parent) : OffThreadGenerator(parent) { }

void ReverseController::generateMissingReverses(const genesis::project::Project &project,
                                                const std::filesystem::path &cache_root)
{
    if (generating()) {
        return;
    }
    if (cache_root.empty()) {
        return;
    }

    // Flatten on the caller's (controller's) thread, so the worker only reads
    // the snapshot and never the project, which a concurrent edit could mutate.
    // Only the reversed footage clips travel: a timeline with none has no
    // worker to start, so an ordinary edit spawns no thread here.
    std::vector<render::ExportClip> reversed;
    for (render::ExportClip &clip : render::flatten_timeline(project, std::nullopt)) {
        if (clip.reverse && clip.kind == render::ClipKind::Video && !clip.path.empty()) {
            reversed.push_back(std::move(clip));
        }
    }
    if (reversed.empty()) {
        return;
    }

    start([this, reversed = std::move(reversed), cache_root]() {
        return ges::ensure_reverses(reversed, cache_root, [this]() { return cancelRequested(); })
                > 0;
    });
}
