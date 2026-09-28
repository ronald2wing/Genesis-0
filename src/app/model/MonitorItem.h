// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <QQuickItem>
#include <QMutex>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "adapters/engine/EngineSession.h"

class QSGSimpleTextureNode;

// Draws the most recent engine frame inside a Qt Quick scene.
//
// The engine's frame sink fires on a GStreamer streaming thread, while the
// scene-graph state this item owns (the committed QSGTexture and node) may only
// be touched on the render thread inside updatePaintNode, where the RHI
// context is current. The threading contract is: stageFrame() parks the frame
// under a mutex, then dirties the item - directly via update() when already on
// the item's thread, or by queuing a coalesced update() onto it from the
// streaming thread (latest-wins, so a fast sink never piles up queued events).
// updatePaintNode() picks the frame up and does all the GL work. The engine's
// frame's texture is copied into an app-owned GL texture so the engine's
// buffer pool can recycle immediately — the scene graph samples the copy,
// not the pool's shared texture. The frame is fit into the item preserving its
// aspect ratio (letterboxed), never stretched to the pane's shape.
class MonitorItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    // Frames committed to the scene graph since the item was created.
    Q_PROPERTY(int rendered READ rendered NOTIFY renderedChanged)
    // Human-readable description of the most recent frame (WxH), or "none".
    Q_PROPERTY(QString frameInfo READ frameInfo NOTIFY frameInfoChanged)

public:
    explicit MonitorItem(QQuickItem *parent = nullptr);
    ~MonitorItem() override;

    int rendered() const { return rendered_; }
    QString frameInfo() const { return frameInfo_; }

    // The engine sink's target. Thread-safe: may be called from the engine's
    // streaming thread or the item's thread (see the class comment for the
    // threading contract).
    void stageFrame(genesis::adapters::engine::FrameTexture frame);

    // Drops the parked frame's keep-alive token without touching GL. Called at
    // shutdown before the engine session is destroyed: the token holds a
    // GstBuffer whose GL memory keeps the engine's GL context alive, and that
    // context must be finalized while the app's EGL context is still valid.
    void clearFrame();

signals:
    void renderedChanged();
    void frameInfoChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void releaseResources() override;

private:
    // A frame parked for the render thread. The keep-alive token keeps the GL
    // texture alive until the frame is copied into the app-owned texture.
    struct PendingFrame
    {
        std::uint64_t textureId = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::shared_ptr<void> keepAlive;
    };

    // Guards pending_ (streaming thread writes, render thread reads).
    QMutex frameMutex_;
    std::optional<PendingFrame> pending_;

    // True while a queued update() is scheduled for the item's thread. Gates
    // the streaming-thread marshalling in stageFrame so at most one queued
    // handoff is in flight, coalescing bursts into a single latest-wins paint.
    std::atomic<bool> dirtyQueued_{ false };

    // Render-thread-only state. The app-owned GL texture decouples the scene
    // graph from the engine's buffer pool: each frame's content is copied in
    // before the pool recycles its textures. Recreated lazily when frame
    // dimensions change.
    unsigned int ownedTextureId_ = 0;
    std::uint32_t ownedWidth_ = 0;
    std::uint32_t ownedHeight_ = 0;

    int rendered_ = 0;
    QString frameInfo_ = QStringLiteral("none");
};
