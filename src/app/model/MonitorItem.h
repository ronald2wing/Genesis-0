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
class QSGTexture;

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
// keep-alive token rides alongside the committed texture so the buffer it names
// is not recycled while the scene graph still samples it, and is released when
// the texture is swapped out or the item is destroyed.
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

signals:
    void renderedChanged();
    void frameInfoChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void releaseResources() override;

private:
    // A frame parked for the render thread. The keep-alive token keeps the GL
    // texture (owned by its GStreamer memory) alive until it is sampled.
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

    // Render-thread-only state. The QSGTexture is owned here and swapped
    // (deleted + keep-alive dropped) on every new frame; the node itself
    // belongs to the scene graph and is handed back each paint.
    // createTextureFromRhiTexture() returns a texture that owns its
    // QRhiTexture, so deleting the QSGTexture frees it - never double-free
    // the QRhiTexture.
    QSGTexture *sgTexture_ = nullptr;
    std::shared_ptr<void> activeKeepAlive_;

    int rendered_ = 0;
    QString frameInfo_ = QStringLiteral("none");
};
