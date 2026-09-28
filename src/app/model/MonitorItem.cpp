// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/model/MonitorItem.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions_3_0>
#include <QOpenGLFunctions_4_3_Core>
#include <QRectF>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>
#include <QThread>
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

    // Copy the engine's GL texture into an app-owned texture so the engine's
    // buffer pool can recycle its textures immediately. Without this the pool
    // reuses a small ring of ~3 texture ids and glTexSubImage2D writes into
    // a texture the scene graph may still be sampling, causing alternating
    // frame flicker.
    {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (!ctx)
            return oldNode;

        QOpenGLFunctions *f = ctx->functions();

        const auto w = static_cast<GLsizei>(frame->width);
        const auto h = static_cast<GLsizei>(frame->height);

        // Lazy-create / recreate the owned texture when dimensions change
        if (ownedTextureId_ == 0 || ownedWidth_ != frame->width || ownedHeight_ != frame->height) {
            if (ownedTextureId_ != 0)
                f->glDeleteTextures(1, &ownedTextureId_);
            f->glGenTextures(1, &ownedTextureId_);
            f->glBindTexture(GL_TEXTURE_2D, ownedTextureId_);
            f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                            nullptr);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            ownedWidth_ = frame->width;
            ownedHeight_ = frame->height;
        }

        const auto srcId = static_cast<GLuint>(frame->textureId);

        QOpenGLFunctions_4_3_Core f43;
        if (f43.initializeOpenGLFunctions()) {
            // Preferred path: direct copy on GL 4.3+ Core — no FBO overhead
            f43.glCopyImageSubData(srcId, GL_TEXTURE_2D, 0, 0, 0, 0, ownedTextureId_, GL_TEXTURE_2D,
                                   0, 0, 0, 0, w, h, 1);
        } else {
            // Fallback: FBO attach + glBlitFramebuffer (GL 3.0+ / ES 3.0+)
            QOpenGLFunctions_3_0 f30;
            f30.initializeOpenGLFunctions();

            GLuint fbos[2] = { };
            f30.glGenFramebuffers(2, fbos);

            f30.glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[0]);
            f30.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       srcId, 0);
            f30.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[1]);
            f30.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       ownedTextureId_, 0);

            f30.glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

            f30.glBindFramebuffer(GL_FRAMEBUFFER, 0);
            f30.glDeleteFramebuffers(2, fbos);
        }
    }

    // The app-owned texture now holds the frame content; the keep-alive token
    // is no longer needed — the engine may recycle its buffer immediately.
    frame->keepAlive.reset();

    auto *node = static_cast<QSGImageNode *>(oldNode);
    if (!node) {
        // QSGImageNode is the RHI-compatible textured-quad node. The older
        // QSGSimpleTextureNode is documented as functional only with the
        // default/software scenegraph backends; under the RHI backend it
        // samples the wrapped texture with stale geometry, which shows as
        // garbled vertical bands.
        node = win->createImageNode();
        node->setFiltering(QSGTexture::Linear);
        // The node owns the texture it is given, so replacing it releases the
        // previous frame's texture at the right point in the render pass. A
        // manual delete here would free the texture the scene graph may still
        // be sampling from the previous frame, which shows as a flash.
        node->setOwnsTexture(true);
    }

    // Wrap the app-owned GL texture (already holds a copy of the frame) in a
    // QRhiTexture, then in a QSGTexture the node samples.
    QRhiTexture *rhiTex =
            rhi->newTexture(QRhiTexture::RGBA8,
                            QSize(static_cast<int>(frame->width), static_cast<int>(frame->height)));
    if (!rhiTex) {
        return node;
    }
    if (!rhiTex->createFrom({ quint64(ownedTextureId_), 0 })) {
        delete rhiTex;
        return node;
    }

    QSGTexture *sgTex =
            win->createTextureFromRhiTexture(rhiTex, QQuickWindow::TextureHasAlphaChannel);
    if (!sgTex) {
        delete rhiTex;
        return node;
    }

    // Fit the frame into the item preserving its aspect ratio, centred, so a
    // source whose shape differs from the pane (a vertical clip in a wide
    // monitor, or the reverse) is letterboxed rather than stretched. The bars
    // are the window showing through, since the node draws only the frame quad.
    const QRectF bounds = boundingRect();
    const qreal frameAspect = static_cast<qreal>(frame->width) / static_cast<qreal>(frame->height);
    QRectF target = bounds;
    if (bounds.width() > 0.0 && bounds.height() > 0.0 && frameAspect > 0.0) {
        if (bounds.width() / bounds.height() > frameAspect) {
            // Pane is wider than the frame: pillarbox, full height.
            target.setWidth(bounds.height() * frameAspect);
            target.moveLeft(bounds.left() + (bounds.width() - target.width()) * 0.5);
        } else {
            // Pane is taller than the frame: letterbox, full width.
            target.setHeight(bounds.width() / frameAspect);
            target.moveTop(bounds.top() + (bounds.height() - target.height()) * 0.5);
        }
    }

    node->setTexture(sgTex);
    node->setRect(target);
    node->markDirty(QSGNode::DirtyMaterial);

    // The node owns the texture (setOwnsTexture above), so it releases the
    // previous frame's texture itself when the new one is set. The keep-alive
    // token was already released after the blit into the app-owned texture.

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
    if (ownedTextureId_ != 0) {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (ctx)
            ctx->functions()->glDeleteTextures(1, &ownedTextureId_);
        ownedTextureId_ = 0;
    }
    ownedWidth_ = 0;
    ownedHeight_ = 0;

    QMutexLocker lock(&frameMutex_);
    pending_.reset();
}

void MonitorItem::clearFrame()
{
    QMutexLocker lock(&frameMutex_);
    pending_.reset();
}
