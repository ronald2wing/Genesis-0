// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/model/MonitorItem.h"

#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QThread>
#include <cstdio>
#include <rhi/qrhi.h>
#include <utility>

MonitorItem::MonitorItem(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);
}

MonitorItem::~MonitorItem()
{
    // The scene graph normally releases these via releaseResources(); at
    // process exit it may already be gone, so best-effort here too.
    releaseResources();
}

void MonitorItem::stageFrame(genesis::adapters::engine::FrameTexture frame)
{
    // Runs wherever the engine's sink fires. No GL context is needed here:
    // this only reads the frame's fields and moves its keep-alive token under
    // the mutex. The actual GL work happens in updatePaintNode.
    if (frame.texture_id == 0 || frame.width == 0 || frame.height == 0) {
        return;
    }

    // Park the frame for the render thread, latest-wins: a burst of frames
    // between two paints overwrites the slot, so only the newest survives.
    PendingFrame pending;
    pending.textureId = frame.texture_id;
    pending.width = frame.width;
    pending.height = frame.height;
    pending.keepAlive = std::move(frame.keep_alive);
    {
        QMutexLocker lock(&frameMutex_);
        pending_ = std::move(pending);
    }

    // The item's update() (not the window's) is what re-fires updatePaintNode,
    // and it is a GUI-thread call. On the item's thread, dirty it directly.
    if (QThread::currentThread() == thread()) {
        update();
        return;
    }

    // Off the item's thread (the GStreamer streaming thread): marshal the
    // dirty to the item's thread. dirtyQueued_ coalesces: only one queued
    // handoff is in flight while the scene graph catches up. Frames arriving
    // in the meantime overwrite pending_ above and find the flag already set,
    // so they queue nothing; the single queued call dirties the item once and
    // updatePaintNode commits the newest parked frame. Without the flag every
    // presented frame would pile a queued event - each holding its own
    // keep-alive token - into the event loop unbounded.
    if (!dirtyQueued_.exchange(true)) {
        QMetaObject::invokeMethod(
                this,
                [this] {
                    dirtyQueued_.store(false);
                    update();
                },
                Qt::QueuedConnection);
    }
}

QSGNode *MonitorItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data)
{
    (void)data;

    std::optional<PendingFrame> frame;
    {
        QMutexLocker lock(&frameMutex_);
        frame = std::move(pending_);
        pending_.reset();
    }
    // First frame not arrived yet (or no new frame since the last paint):
    // keep the currently committed texture and draw nothing new. On the very
    // first paint this returns nullptr - committing an empty texture node
    // crashes the software renderer, which dereferences its null texture.
    if (!frame) {
        return oldNode;
    }

    QQuickWindow *win = window();
    QRhi *rhi = win ? win->rhi() : nullptr;
    if (!rhi) {
        // No RHI (the software renderer): the engine's GL texture cannot be
        // wrapped, so there is nothing to draw.
        return oldNode;
    }

    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setFiltering(QSGTexture::Linear);
    }

    // Wrap the engine's GL texture id (kept alive by frame->keepAlive) in a
    // QRhiTexture, then in a QSGTexture the node samples.
    QRhiTexture *rhiTex =
            rhi->newTexture(QRhiTexture::RGBA8,
                            QSize(static_cast<int>(frame->width), static_cast<int>(frame->height)));
    if (!rhiTex) {
        return node;
    }
    if (!rhiTex->createFrom({ quint64(frame->textureId), 0 })) {
        delete rhiTex;
        return node;
    }

    QSGTexture *sgTex =
            win->createTextureFromRhiTexture(rhiTex, QQuickWindow::TextureHasAlphaChannel);
    if (!sgTex) {
        delete rhiTex;
        return node;
    }

    node->setTexture(sgTex);
    node->setRect(boundingRect());
    node->markDirty(QSGNode::DirtyMaterial);

    // Swap out the previous frame's resources. createTextureFromRhiTexture()
    // returns a texture that owns the QRhiTexture, so deleting the QSGTexture
    // frees it; the QRhiTexture wraps (never owns) the GL id, which the engine
    // keeps valid until the keep-alive token is dropped here.
    if (sgTexture_) {
        delete sgTexture_;
    }
    sgTexture_ = sgTex;
    activeKeepAlive_ = std::move(frame->keepAlive);

    ++rendered_;
    emit renderedChanged();
    // Reflect the live frame's dimensions on every paint, not just the first
    // three, so the shell's readout tracks the current source. Guarded so a
    // steady size does not spam identical change signals.
    const QString info = QStringLiteral("%1x%2").arg(frame->width).arg(frame->height);
    if (info != frameInfo_) {
        frameInfo_ = info;
        emit frameInfoChanged();
    }
    return node;
}

void MonitorItem::releaseResources()
{
    if (sgTexture_) {
        delete sgTexture_;
        sgTexture_ = nullptr;
    }
    activeKeepAlive_.reset();
    QMutexLocker lock(&frameMutex_);
    pending_.reset();
}
