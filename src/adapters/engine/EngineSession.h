// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "adapters/engine/ScopeTypes.h"
#include "core/ClipTimeline.h"

namespace genesis::render {
struct BuiltTimeline;
}

namespace genesis::adapters::engine {

// A frame the engine has already put on the GPU: a texture id in the
// process's GL context, the size to draw it at, and an opaque token that
// keeps the engine's buffer alive for exactly as long as the caller holds
// it. Deliberately not a GstSample: the UI wraps the id, it never learns
// what produced it (R5/R6).
struct FrameTexture
{
    std::uint64_t texture_id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The transfer the frame was rendered under, detected from its caps'
    // colorimetry; Sdr when the metadata is absent or down-converted away.
    ScopeSignal signal = ScopeSignal::Sdr;
    std::shared_ptr<void> keep_alive; // opaque; drop it and the frame may be recycled
};

// The native GL context the engine's upload path must use: the app owns the
// context, the engine shares it. Raw handles so no engine type crosses;
// zero means "not set" and the engine will try to make its own.
//
// `api` names the client API the wrapped context was created with. The engine
// wraps the app's context for GStreamer, and the wrapped context must declare
// the same API or the pipeline's share group cannot sample the app's textures
// (valid handle, black readback). The host detects it from its own context's
// API; Unknown leaves the engine's GLES2 default, which is right for a
// headless GLES context and wrong for a desktop-GL one.
enum class GlApi { Unknown, GLES2, OpenGL, OpenGL3 };

struct GlHandles
{
    std::uintptr_t display = 0;
    std::uintptr_t context = 0;
    GlApi api = GlApi::Unknown;
};

// Hands every presented frame to `sink` as it becomes ready. Called on the
// engine's streaming thread: a sink that touches the UI must marshal to its
// own thread.
using FrameSink = std::function<void(FrameTexture)>;

// Which decoders the session should prefer. The engine owns the actual
// choice (R5) — this only tilts it. Auto lets the engine rank; Software
// forces the pure-software path, which is the escape hatch when a hardware
// decoder is present but broken for a given file.
enum class DecodePreference { Auto, Software, Hardware };

// What the host may ask of any engine. Deliberately narrow: this is the seam
// where the engine (GES) lives on one side and the host
// graph on the other, and no engine type may cross it (R5/R6). Widened as
// later phases need it - this increment is just the clock and the graph.
class EngineSession
{
public:
    virtual ~EngineSession() = default;

    // Move the playhead to `position` on the host's own rational clock.
    // False when the engine could not honour the seek.
    virtual bool seek(genesis::core::Rational position) = 0;

    // Where the playhead is, on the host's own rational clock.
    virtual genesis::core::Rational position() const = 0;

    // Warms the pipeline around `position` so a later seek there lands without a
    // stall: decoders reopened, buffers refilled, the first frames pre-rolled.
    // Must not change the caller-visible playing state - a playing session is
    // still playing when this returns. Returns false when the position could not
    // be warmed (an unknown position, or the pipeline could not preroll).
    virtual bool prepare(genesis::core::Rational position) = 0;

    // Start rolling. False if the engine refused (nothing loaded, for one).
    virtual bool play() = 0;

    // Hold the current frame. False if the engine refused.
    virtual bool pause() = 0;

    // Compile `timeline` into whatever the engine renders. The engine keeps
    // no reference into host memory it does not own - it copies what it
    // needs. False when the graph cannot be built.
    virtual bool load(const genesis::render::BuiltTimeline &timeline) = 0;

    // Adopt the app's GL context for the engine's upload path. False when the
    // handles are unusable (zero, for one).
    virtual bool set_gl_context(GlHandles handles) = 0;

    // Register where presented frames go. Pass an empty sink to stop
    // presenting (frames then drain without a consumer).
    virtual void set_frame_sink(FrameSink sink) = 0;

    // Routes the session's audio to the system's default output. Off by default:
    // a headless run (tests, export) must not grab the sound device. Returns
    // false when no output could be opened - in that case audio keeps draining
    // silently and VIDEO MUST STILL PLAY. Call before load().
    virtual bool set_audio_output(bool enabled) = 0;

    // Applies the preference to how the session builds its pipeline. Call
    // before load(). Returns false when the preference cannot be honoured
    // (e.g. Hardware was asked for and no hardware decoder exists) — in that
    // case the session still loads and plays with whatever the engine ranks
    // first.
    virtual bool set_decode_preference(DecodePreference preference) = 0;
};

} // namespace genesis::adapters::engine
