// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include "adapters/engine/EngineSession.h"

#include <algorithm>
#include <memory>

// GES types are forward-declared so this header stays lean; GesSession.cpp is
// the only translation unit that includes the full GES headers.
typedef struct _GESPipeline GESPipeline;

namespace genesis::adapters::engine::ges {

// A headless playback session over one compiled GESTimeline. load() compiles
// the host graph, prerolls to PAUSED, and leaves the first frame ready; the
// clock methods then drive the playhead against the host's own rational time.
//
// Presentation is opt-in: when the host adopts a GL context via
// set_gl_context, the video stream drains through the GL upload chain into an
// appsink and each frame is handed to the FrameSink set by set_frame_sink.
// With no context adopted the session stays a pure clock/graph driver and
// drains both streams into fakesinks, so it runs without a display.
class GesSession final : public genesis::adapters::engine::EngineSession
{
public:
    GesSession();
    ~GesSession() override;

    bool load(const genesis::render::BuiltTimeline &timeline) override;
    bool play() override;
    bool pause() override;
    bool seek(genesis::core::Rational position) override;
    genesis::core::Rational position() const override;
    bool prepare(genesis::core::Rational position) override;
    bool set_gl_context(genesis::adapters::engine::GlHandles handles) override;

    void set_frame_sink(genesis::adapters::engine::FrameSink sink) override;
    bool set_audio_output(bool enabled) override;
    bool set_decode_preference(genesis::adapters::engine::DecodePreference preference) override;

    // The GStreamer-side presentation state (wrapped GL display/context, the
    // frame sink, and the callback user_data). Opaque here so no GStreamer
    // type leaks into this header; GesSession.cpp owns the full definition.
    // Public so the GStreamer callbacks in that file (free functions outside
    // the class) can reach the state they are passed as user_data.
    struct Presentation;

private:
    // Marks the presentation detached and clears the appsink callback, so a
    // frame delivered after teardown begins cannot reach the host. Called
    // before every pipeline teardown.
    void detach_presentation();

    GESPipeline *pipeline_ = nullptr;
    std::unique_ptr<Presentation> presentation_;
    // The host's audio-output request, applied when the pipeline is next
    // (re)built in load(). Off until set_audio_output(true) succeeds, so a
    // headless run never grabs the sound device on its own.
    bool audio_output_requested_ = false;
    // The loaded timeline held no clips. An empty pipeline has nothing to
    // preroll or play, so play()/pause() skip the blocking state wait that
    // would otherwise sit out its full timeout on the UI thread.
    bool empty_ = false;
};

} // namespace genesis::adapters::engine::ges
