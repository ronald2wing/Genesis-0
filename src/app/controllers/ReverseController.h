// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's reverse controller: the thin, off-thread generation of
// the reversed copies a preview's reversed clips read through. It owns no
// media and no engine session - only the flatten step and the worker thread
// that fills the reverse cache (R4, shared with the proxy controller through
// `OffThreadGenerator`). The shell reloads the engine timeline when `finished`
// fires, so a generated reverse lands in the preview on the next build.

#pragma once

#include <filesystem>

#include "app/controllers/OffThreadGenerator.h"

namespace genesis::project {
struct Project;
}

class ReverseController : public OffThreadGenerator
{
    Q_OBJECT

public:
    explicit ReverseController(QObject *parent = nullptr);

    // Flattens the project on the caller's thread, then spawns a worker that
    // generates every reversed clip's copy off the UI thread. No-ops while
    // already generating. Emits `finished` on the controller's thread when the
    // run ends, whatever it produced.
    void generateMissingReverses(const genesis::project::Project &project,
                                 const std::filesystem::path &cache_root);
};
