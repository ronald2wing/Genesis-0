// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/PlaybackController.h"

#include <cstdio>
#include <cstdint>
#include <utility>

#include <QOpenGLContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QtGui/qopenglcontext_platform.h>

#include "app/support/AppLog.h"
#include "app/model/MonitorItem.h"
#include "render/Resolve.h"

using genesis::adapters::engine::DecodePreference;
using genesis::adapters::engine::EngineSession;
using genesis::adapters::engine::FrameTexture;
using genesis::adapters::engine::GlHandles;
namespace render = genesis::render;
using genesis::app::app_log;
using genesis::core::Rational;

PlaybackController::PlaybackController(EngineSession *session, render::BuiltTimeline timeline,
                                       QObject *parent)
    : QObject(parent),
      session_(session),
      timeline_(std::make_unique<render::BuiltTimeline>(std::move(timeline)))
{
}

PlaybackController::~PlaybackController() = default;

void PlaybackController::play()
{
    if (session_ && session_->play())
        setPlaying(true);
}

void PlaybackController::pause()
{
    if (session_ && session_->pause())
        setPlaying(false);
}

void PlaybackController::togglePlay()
{
    if (!session_)
        return;
    if (playing_) {
        if (session_->pause())
            setPlaying(false);
    } else {
        if (session_->play())
            setPlaying(true);
    }
}

void PlaybackController::seek(double seconds)
{
    if (!session_)
        return;
    if (const auto target = Rational::approximate(seconds)) {
        session_->seek(*target);
    }
}

double PlaybackController::position() const
{
    return session_ ? session_->position().as_double() : 0.0;
}

void PlaybackController::setDecodePreference(int value)
{
    if (value < 0 || value > 2 || value == decodePreference_) {
        return;
    }
    decodePreference_ = value;
    if (session_) {
        session_->set_decode_preference(static_cast<DecodePreference>(value));
        if (started_) {
            reload(*timeline_);
        }
    }
    emit decodePreferenceChanged();
}

void PlaybackController::start()
{
    attemptStart();
}

void PlaybackController::reload(render::BuiltTimeline timeline)
{
    *timeline_ = std::move(timeline);
    if (!started_ || !session_) {
        return;
    }
    const bool was_playing = playing_;
    const Rational position = session_->position();
    if (!session_->load(*timeline_)) {
        app_log("[app] reload failed\n");
        return;
    }
    session_->seek(position);
    if (was_playing) {
        if (session_->play()) {
            setPlaying(true);
        }
    } else {
        setPlaying(false);
    }
}

void PlaybackController::setPlaying(bool value)
{
    if (playing_ == value)
        return;
    playing_ = value;
    emit playingChanged();
}

void PlaybackController::attemptStart()
{
    QQuickWindow *win = monitor_ ? monitor_->window() : nullptr;
    QOpenGLContext *qctx = nullptr;
    if (win && win->rendererInterface()) {
        qctx = static_cast<QOpenGLContext *>(win->rendererInterface()->getResource(
                win, QSGRendererInterface::OpenGLContextResource));
    }
    if (!qctx) {
        if (++contextAttempts_ >= 120) {
            app_log("[app] gave up waiting for the RHI GL context\n");
            return;
        }
        QTimer::singleShot(16, this, [this] { attemptStart(); });
        return;
    }

    auto *egl = qctx->nativeInterface<QNativeInterface::QEGLContext>();
    if (!egl) {
        app_log("[app] no QEGLContext native interface\n");
        return;
    }

    const GlHandles handles{ reinterpret_cast<std::uintptr_t>(egl->display()),
                             reinterpret_cast<std::uintptr_t>(egl->nativeContext()) };
    if (!session_->set_gl_context(handles)) {
        app_log("[app] set_gl_context failed\n");
        return;
    }
    app_log("[app] set_gl_context ok\n");

    // Hand the handles to the composition root so it can build the Scopes
    // readback adapter (an engine-side detail) and install the frame observer.
    // Emitted synchronously (a same-thread direct connection), so the observer
    // is in place before the frame sink below starts presenting.
    emit glHandlesReady(handles);

    session_->set_frame_sink([this](FrameTexture frame) {
        ++framesSeen_;
        if (framesSeen_ <= 3) {
            app_log("[app] frame sink #%llu: id=%llu %ux%u\n",
                    static_cast<unsigned long long>(framesSeen_),
                    static_cast<unsigned long long>(frame.texture_id), frame.width, frame.height);
        }
        // Hand the frame to the injected observer; the composition root wires
        // it to the Scopes readback and the MonitorItem's stageFrame.
        if (frameObserver_)
            frameObserver_(std::move(frame));
    });

    if (!session_->load(*timeline_)) {
        app_log("[app] load failed\n");
        return;
    }
    if (!session_->play()) {
        app_log("[app] play failed\n");
        return;
    }
    setPlaying(true);
    started_ = true;
    app_log("[app] playing\n");
}
