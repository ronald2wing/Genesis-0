// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <functional>
#include <string>

#include "core/ClipTimeline.h"

namespace genesis::render {
struct BuiltTimeline;
struct ExportSpec;
} // namespace genesis::render

namespace genesis::adapters::engine {

// One progress report from a running export. Engine-neutral (R5/R6): no
// GStreamer type escapes into the host.
struct ExportProgress
{
    // How much of the timeline has been written, 0..1, from the pipeline's
    // position against the timeline's duration.
    double fraction = 0.0;
    // The exact host time the pipeline had reached when the report was made.
    genesis::core::Rational position = genesis::core::Rational::ZERO;
};

// What an export run reports back when it finishes.
struct ExportOutcome
{
    bool ok = false;
    // Non-empty when !ok.
    std::string error;
    // What was written: the timeline's full span on success, zero on failure.
    genesis::core::Rational duration = genesis::core::Rational::ZERO;
};

// Renders `timeline` to `spec.output` to completion. Builds the GES timeline
// from the host graph, puts the GESPipeline in render mode, writes the file,
// reports progress through `progress`, and honours a cancellation request from
// `cancelled`. Blocking: run it off the UI thread (R4).
//
// `progress` is invoked on the export's own worker thread (the thread driving
// the GES pipeline), never the UI thread - a callback that touches the UI must
// marshal. `cancelled` is polled between position reports; when it returns
// true the render stops cleanly, the partial file is removed, and the outcome
// is !ok with a reason. A codec family whose encoder element is not installed
// is a runtime refusal here, not a host validation problem (R5).
ExportOutcome export_timeline(const genesis::render::ExportSpec &spec,
                              const genesis::render::BuiltTimeline &timeline,
                              const std::function<void(ExportProgress)> &progress,
                              const std::function<bool()> &cancelled);

} // namespace genesis::adapters::engine
