// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The engine-neutral controller the QML scene drives. It owns the session on
// the host's side of the EngineSession seam - no GES type appears in its
// interface - and forwards the engine's frame sink to an injected observer.
// The two engine-side details the controller used to own are pushed out to the
// composition root (src/app/AppShell), where the GES session and the Scopes
// readback adapter are built:
//
//   * The frame sink is handed to an injected `FrameSink` observer. The
//     controller keeps only its own bookkeeping (the framesSeen counter and
//     the first-frames diagnostic), then calls the observer; the shell wires
//     that observer to the Scopes readback and the MonitorItem's stageFrame.
//
//   * Once the app's GL context exists the controller emits the handles it
//     acquired (`glHandlesReady`) instead of building the Scopes readback
//     adapter itself. The shell connects that signal, constructs the adapter on
//     the engine's side, and installs the observer from those handles.
//
// The emission is synchronous (a same-thread direct connection), so the
// observer is in place before the frame sink starts presenting.

#pragma once

#include <QObject>

#include <cstdint>
#include <memory>
#include <utility>

#include "adapters/engine/EngineSession.h"

class MonitorItem;

namespace genesis::render {
struct BuiltTimeline;
}

class PlaybackController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    // Which decoders the session should prefer (DecodePreference::Auto=0,
    // Software=1, Hardware=2). Applied before the next load().
    Q_PROPERTY(int decodePreference READ decodePreference NOTIFY decodePreferenceChanged)

public:
    PlaybackController(genesis::adapters::engine::EngineSession *session,
                       genesis::render::BuiltTimeline timeline, QObject *parent = nullptr);
    ~PlaybackController() override;

    void setMonitor(MonitorItem *monitor) { monitor_ = monitor; }

    // Injects the frame observer: the sink's target beyond the controller's own
    // bookkeeping (the frame counter and the first-frames diagnostic). The
    // composition root wires it to the Scopes readback and the MonitorItem's
    // stageFrame once the GL context exists; empty means frames drain without a
    // consumer.
    void setFrameObserver(genesis::adapters::engine::FrameSink observer)
    {
        frameObserver_ = std::move(observer);
    }

    bool playing() const { return playing_; }
    int decodePreference() const { return decodePreference_; }

    // Frames the engine handed to the sink, for the presentation diagnostic.
    std::uint64_t framesSeen() const { return framesSeen_; }

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE double position() const;

    // Sets the decoder preference and rebuilds the pipeline so it takes
    // effect: set_decode_preference must run before load(), and reload() is
    // exactly that next load. A preference the engine cannot honour is still
    // accepted - the session falls back to its own ranking.
    Q_INVOKABLE void setDecodePreference(int value);

    // Hands the RHI GL context to the engine, wires the frame sink, loads and
    // plays. Called from the shell once the QML scene (and its MonitorItem) is up;
    // polls until the scene graph has created its GL context.
    void start();

    // Reload policy: every structural edit rebuilds the flattened clip list
    // and the engine timeline, then calls load() again - the session tears
    // the old pipeline down and rebuilds (cheap at this clip count), so the
    // monitor and the host project can never desync. To keep the picture
    // where it was, the playhead position and playing state are captured and
    // restored across the reload; a position past a now-shorter timeline
    // clamps in seek(). Before start() has finished (no GL context yet) the
    // reload only swaps the stored timeline, which start() loads once ready.
    void reload(genesis::render::BuiltTimeline timeline);

signals:
    void playingChanged();
    void decodePreferenceChanged();
    // Emitted once the app's GL context is adopted, carrying the raw handles
    // the engine now shares. The composition root builds the Scopes readback
    // adapter (an engine-side detail) and wires the frame observer from these.
    void glHandlesReady(genesis::adapters::engine::GlHandles handles);

private:
    void setPlaying(bool value);
    void attemptStart();

    genesis::adapters::engine::EngineSession *session_ = nullptr;
    MonitorItem *monitor_ = nullptr;
    genesis::adapters::engine::FrameSink frameObserver_;
    // Held behind a pointer so the header needs only the forward declaration,
    // not the full render::Resolve.h definition.
    std::unique_ptr<genesis::render::BuiltTimeline> timeline_;
    int contextAttempts_ = 0;
    bool playing_ = false;
    bool started_ = false;
    int decodePreference_ = 0;
    std::uint64_t framesSeen_ = 0;
};
