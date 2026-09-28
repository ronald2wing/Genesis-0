// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// QML interaction regression tests: loads each QML surface offscreen, drives
// synthetic mouse events onto named controls, and asserts the right host
// invokable was called.  The fakes record every call into a QStringList so
// the test assertions are plain string comparisons.  This suite is a dev-host
// regression net: it runs only when Qt >= 6.5 is present (CI on Ubuntu 24.04
// ships 6.4 and skips Qt suites), and it pins bugs the smoke gate cannot catch
// because smoke opens each surface against a fresh empty project while these
// cases need the fake model data plus gesture-level interaction.

#include "tests/support/Checks.h"
#include "tests/support/Wait.h"
#include "tests/app/fakes/FakeEditController.h"
#include "tests/app/fakes/FakeTimelineModel.h"
#include "tests/app/fakes/FakeMediaBinController.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtMessageHandler>
#include <QtGlobal>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

static int g_warningCount = 0;
static int g_criticalCount = 0;

static void messageHandler(QtMsgType type, const QMessageLogContext & /*ctx*/, const QString &msg)
{
    // Two warnings are inherent to this harness, not product defects, so they
    // are excluded from the zero-message contract:
    //  - "QRhi is only compatible with default adaptation": the software
    //    scene-graph backend paired with QQuickRenderControl.
    //  - "No render target": QQuickRenderControl has no swapchain under the
    //    offscreen platform, so each frame logs this.
    const bool harnessNoise = msg.contains(QStringLiteral("QRhi is only compatible"))
            || msg.contains(QStringLiteral("No render target"));
    if (type == QtWarningMsg && !harnessNoise)
        ++g_warningCount;
    else if ((type == QtCriticalMsg || type == QtFatalMsg) && !harnessNoise)
        ++g_criticalCount;
    // Print so a failing test shows what message was emitted.
    std::fprintf(stderr, "QML: %s\n", qPrintable(msg));
}

// Finds a QQuickItem by objectName anywhere in the QObject tree, searching
// from the QQuickView downward (the view IS a QObject, and every QQuickItem
// in the scene is a QObject descendant somewhere).
static void dumpItems(QObject *parent, int depth = 0)
{
    const auto kids = parent->children();
    for (auto *kid : kids) {
        auto *qi = qobject_cast<QQuickItem *>(kid);
        if (qi) {
            QString indent(depth * 2, ' ');
            std::fprintf(stderr, "%sQQuickItem objName='%s' class=%s geom=%.0fx%.0f vis=%d\n",
                         indent.toStdString().c_str(), qPrintable(qi->objectName()),
                         qi->metaObject()->className(), qi->width(), qi->height(),
                         static_cast<int>(qi->isVisible()));
            dumpItems(qi, depth + 1);
        }
    }
}

static QQuickItem *findItemByName(QQuickWindow *window, const QString &name)
{
    // Delegates created by a view (GridView/ListView/Repeater) are QQuickItem
    // children of the view's contentItem but NOT QObject children of it, so
    // QObject::findChild cannot reach them. Walk the QQuickItem childItems()
    // tree instead, which is the visual hierarchy.
    QQuickItem *item = nullptr;
    genesis::test::wait_until(
            [&] {
                item = nullptr;
                std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
                    if (item || !it)
                        return;
                    if (it->objectName() == name) {
                        item = it;
                        return;
                    }
                    for (auto *ch : it->childItems())
                        walk(ch);
                };
                walk(window->contentItem());
                return item != nullptr;
            },
            5000);
    if (!item) {
        std::fprintf(stderr, "DEBUG: item '%s' not found in %lld children\n", qPrintable(name),
                     window->findChildren<QQuickItem *>().size());
        dumpItems(window);
    }
    return item;
}

static QPointF centerOf(QQuickItem *item)
{
    return item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0));
}

static void sendMouse(QQuickWindow *window, QEvent::Type type, QPointF scenePos,
                      Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QPointF local = window->contentItem()->mapFromScene(scenePos);
    QMouseEvent ev(type, local, window->mapToGlobal(local.toPoint()), button, buttons,
                   Qt::NoModifier);
    QCoreApplication::sendEvent(window, &ev);
}

// The deepest visible item under a scene point, walking the QQuickItem visual
// tree. QQuickWindow's own delivery does not reliably reach deeply nested items
// (a ToggleButton inside a Repeater inside a Column inside a Row), so the
// harness targets the item directly.
static QQuickItem *deepestItemAt(QQuickItem *item, QPointF scenePos)
{
    if (!item || !item->isVisible())
        return nullptr;
    const QPointF local = item->mapFromScene(scenePos);
    if (!item->contains(local))
        return nullptr;
    const auto kids = item->childItems();
    for (auto it = kids.crbegin(); it != kids.crend(); ++it) {
        if (QQuickItem *hit = deepestItemAt(*it, scenePos))
            return hit;
    }
    return item;
}

static void sendMouseDeep(QQuickWindow *window, QEvent::Type type, QPointF scenePos,
                          Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QQuickItem *target = deepestItemAt(window->contentItem(), scenePos);
    if (!target)
        target = window->contentItem();
    const QPointF local = target->mapFromScene(scenePos);
    QMouseEvent ev(type, local, window->mapToGlobal(local.toPoint()), button, buttons,
                   Qt::NoModifier);
    QCoreApplication::sendEvent(target, &ev);
}

// A loaded surface. The offscreen platform provides no GL context, so a plain
// QQuickView never runs the polish pass that instantiates item-view delegates.
// A QQuickRenderControl window with the software scene-graph backend does: the
// software backend renders to a QImage and needs no GL, and polishItems() runs
// the deferred polish that creates GridView / ListView / Repeater delegates.
struct Surface
{
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QQuickRenderControl> control;
    std::unique_ptr<QQuickWindow> window;
    std::unique_ptr<QQmlComponent> component;
    QQuickItem *root = nullptr;
};

static std::unique_ptr<Surface> loadSurface(const QUrl &url, FakeEditController &editFake,
                                            FakeTimelineModel &timelineModelFake,
                                            FakeMediaBinController &mediaBinFake)
{
    auto surface = std::make_unique<Surface>();
    surface->engine = std::make_unique<QQmlEngine>();
    surface->engine->rootContext()->setContextProperty("edit", &editFake);
    surface->engine->rootContext()->setContextProperty("timelineModel", &timelineModelFake);
    surface->engine->rootContext()->setContextProperty("mediaBin", &mediaBinFake);

    surface->control = std::make_unique<QQuickRenderControl>();
    surface->window = std::make_unique<QQuickWindow>(surface->control.get());
    surface->window->resize(1280, 720);

    surface->component = std::make_unique<QQmlComponent>(surface->engine.get(), url);
    if (surface->component->isError()) {
        genesis::test::check(
                false, std::string("surface failed to load: ") + url.toString().toStdString());
        for (const QQmlError &e : surface->component->errors())
            std::fprintf(stderr, "QML load error: %s\n", qPrintable(e.toString()));
        return nullptr;
    }
    QObject *obj = surface->component->create(surface->engine->rootContext());
    surface->root = qobject_cast<QQuickItem *>(obj);
    if (!surface->root) {
        genesis::test::check(false,
                             std::string("surface root is not a QQuickItem: ")
                                     + url.toString().toStdString());
        return nullptr;
    }
    surface->root->setParentItem(surface->window->contentItem());
    // findChild walks the QObject parent chain, not the QQuickItem parent
    // chain, so the root must also be a QObject child of the window.
    surface->root->setParent(surface->window->contentItem());
    surface->root->setWidth(1280);
    surface->root->setHeight(720);

    surface->window->show();
    surface->control->initialize();
    for (int i = 0; i < 5; ++i) {
        surface->control->polishItems();
        surface->control->beginFrame();
        surface->control->sync();
        surface->control->render();
        surface->control->endFrame();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return surface;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("LIBGL_ALWAYS_SOFTWARE", "1");
    // The offscreen platform has no GL context, so the scene graph must use the
    // software backend; it renders to a QImage and needs no GL. This must be
    // set before the QGuiApplication is constructed.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

    QGuiApplication app(argc, argv);

    qInstallMessageHandler(messageHandler);

    FakeEditController editFake;
    FakeTimelineModel timelineModelFake;
    FakeMediaBinController mediaBinFake;

    // ------------------------------------------------------------------
    // Case 1a: MediaBin Add button — click appends to first track
    // ------------------------------------------------------------------
    {
        auto surface = loadSurface(
                QStringLiteral("qrc:/qt/qml/GenesisAppTest/tests/app/qml/LoadMediaBin.qml"),
                editFake, timelineModelFake, mediaBinFake);
        if (surface) {
            auto *btn = findItemByName(surface->window.get(), "addButton");
            if (btn) {
                QPointF c = centerOf(btn);
                sendMouse(surface->window.get(), QEvent::MouseButtonPress, c, Qt::LeftButton,
                          Qt::LeftButton);
                sendMouse(surface->window.get(), QEvent::MouseButtonRelease, c, Qt::LeftButton,
                          Qt::NoButton);
                bool found = editFake.calls.contains("addClipAt:m1:V1:5");
                genesis::test::check(found, "1a: Add button click -> addClipAt:m1:V1:5");
            } else {
                genesis::test::check(false, "1a: addButton not found in MediaBin");
            }
        }
    }

    // ------------------------------------------------------------------
    // Case 1b: MediaBin card drag — MouseArea reports dragging true
    // ------------------------------------------------------------------
    {
        auto surface = loadSurface(
                QStringLiteral("qrc:/qt/qml/GenesisAppTest/tests/app/qml/LoadMediaBin.qml"),
                editFake, timelineModelFake, mediaBinFake);
        if (surface) {
            auto *ma = findItemByName(surface->window.get(), "cardMouse");
            if (ma) {
                QPointF c = centerOf(ma);
                sendMouse(surface->window.get(), QEvent::MouseButtonPress, c, Qt::LeftButton,
                          Qt::LeftButton);
                // Move +20px horizontally to cross the 8px drag threshold.
                QPointF dragPos(c.x() + 20, c.y());
                sendMouse(surface->window.get(), QEvent::MouseMove, dragPos, Qt::LeftButton,
                          Qt::LeftButton);
                bool dragging = ma->property("dragging").toBool();
                genesis::test::check(dragging, "1b: cardMouse dragging after 20px drag -> true");
                // Release.
                sendMouse(surface->window.get(), QEvent::MouseButtonRelease, dragPos,
                          Qt::LeftButton, Qt::NoButton);
            } else {
                genesis::test::check(false, "1b: cardMouse not found in MediaBin");
            }
        }
    }

    // ------------------------------------------------------------------
    // Case 1c: TimelineStrip lane drop — a media drop places a clip
    // ------------------------------------------------------------------
    {
        editFake.clearCalls();
        auto surface = loadSurface(
                QStringLiteral("qrc:/qt/qml/GenesisAppTest/tests/app/qml/LoadTimelineStrip.qml"),
                editFake, timelineModelFake, mediaBinFake);
        if (surface) {
            auto *drop = findItemByName(surface->window.get(), "laneDrop");
            if (drop) {
                // A drop at the lane's left edge (x=0) on the first lane (y=0)
                // maps to 0 seconds on track V1. The mime payload is the media
                // id the drag source carries.
                QMimeData mime;
                mime.setData(QStringLiteral("application/x-genesis-media-id"), "m1");
                const QPointF local(0, 0);
                // A DropArea only accepts a drop after a drag-enter; send both
                // so the internal containsDrag state is set before the drop.
                QDragEnterEvent enter(local.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton,
                                      Qt::NoModifier);
                QCoreApplication::sendEvent(drop, &enter);
                QDropEvent ev(local, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(drop, &ev);
                bool placed = editFake.calls.contains("addClipAt:m1:V1:0");
                genesis::test::check(placed, "1c: lane drop -> addClipAt:m1:V1:0");
            } else {
                genesis::test::check(false, "1c: laneDrop not found in TimelineStrip");
            }
        }
    }

    // ------------------------------------------------------------------
    // Case 1d: TimelineStrip lane drop on an EMPTY timeline (duration 0)
    // ------------------------------------------------------------------
    {
        editFake.clearCalls();
        // An empty timeline reports duration 0; the drop must still place the
        // clip (at the start) rather than refuse it. This is the first-clip
        // drop, the common beginner case.
        timelineModelFake.setDuration(0.0);
        auto surface = loadSurface(
                QStringLiteral("qrc:/qt/qml/GenesisAppTest/tests/app/qml/LoadTimelineStrip.qml"),
                editFake, timelineModelFake, mediaBinFake);
        if (surface) {
            auto *drop = findItemByName(surface->window.get(), "laneDrop");
            if (drop) {
                QMimeData mime;
                mime.setData(QStringLiteral("application/x-genesis-media-id"), "m1");
                const QPointF local(0, 0);
                QDragEnterEvent enter(local.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton,
                                      Qt::NoModifier);
                QCoreApplication::sendEvent(drop, &enter);
                QDropEvent ev(local, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(drop, &ev);
                bool placed = editFake.calls.contains("addClipAt:m1:V1:0");
                genesis::test::check(placed, "1d: empty-timeline drop -> addClipAt:m1:V1:0");
            } else {
                genesis::test::check(false, "1d: laneDrop not found in TimelineStrip");
            }
        }
        timelineModelFake.setDuration(10.0);
    }

    // ------------------------------------------------------------------
    // Case 2b: TimelineStrip clip body move — press on body begins move
    // ------------------------------------------------------------------
    {
        editFake.clearCalls();
        auto surface = loadSurface(
                QStringLiteral("qrc:/qt/qml/GenesisAppTest/tests/app/qml/LoadTimelineStrip.qml"),
                editFake, timelineModelFake, mediaBinFake);
        if (surface) {
            auto *cm = findItemByName(surface->window.get(), "clipMouse");
            if (cm) {
                // Press at center — well past the 6px edge trim zone.
                QPointF c = centerOf(cm);
                sendMouse(surface->window.get(), QEvent::MouseButtonPress, c, Qt::LeftButton,
                          Qt::LeftButton);
                bool beginMove = editFake.calls.contains("beginMove:c1");
                genesis::test::check(beginMove, "2b: clipMouse body press -> beginMove:c1");

                QPointF movePos(c.x() + 10, c.y());
                sendMouse(surface->window.get(), QEvent::MouseMove, movePos, Qt::LeftButton,
                          Qt::LeftButton);
                sendMouse(surface->window.get(), QEvent::MouseButtonRelease, movePos,
                          Qt::LeftButton, Qt::NoButton);
                bool endMove = editFake.calls.contains("endMove");
                genesis::test::check(endMove, "2b: clipMouse release -> endMove");
            } else {
                genesis::test::check(false, "2b: clipMouse not found in TimelineStrip");
            }
        }
    }

    // ------------------------------------------------------------------
    // Case 2c: TimelineStrip clip edge trim
    // ------------------------------------------------------------------
    {
        editFake.clearCalls();
        auto surface = loadSurface(
                QStringLiteral("qrc:/qt/qml/GenesisAppTest/tests/app/qml/LoadTimelineStrip.qml"),
                editFake, timelineModelFake, mediaBinFake);
        if (surface) {
            auto *cm = findItemByName(surface->window.get(), "clipMouse");
            if (cm) {
                // Press at left edge (x=2, within 6px).
                QPointF leftEdge = cm->mapToScene(QPointF(2.0, cm->height() / 2.0));
                sendMouse(surface->window.get(), QEvent::MouseButtonPress, leftEdge, Qt::LeftButton,
                          Qt::LeftButton);
                bool beginTrim = editFake.calls.contains(QStringLiteral("beginTrim:c1:0"));
                genesis::test::check(beginTrim, "2c: clipMouse left-edge press -> beginTrim:c1:0");

                sendMouse(surface->window.get(), QEvent::MouseButtonRelease, leftEdge,
                          Qt::LeftButton, Qt::NoButton);

                // Press at right edge.
                editFake.clearCalls();
                QPointF rightEdge = cm->mapToScene(QPointF(cm->width() - 2.0, cm->height() / 2.0));
                sendMouse(surface->window.get(), QEvent::MouseButtonPress, rightEdge,
                          Qt::LeftButton, Qt::LeftButton);
                bool beginTrim1 = editFake.calls.contains(QStringLiteral("beginTrim:c1:1"));
                genesis::test::check(beginTrim1,
                                     "2c: clipMouse right-edge press -> beginTrim:c1:1");

                sendMouse(surface->window.get(), QEvent::MouseButtonRelease, rightEdge,
                          Qt::LeftButton, Qt::NoButton);
            } else {
                genesis::test::check(false, "2c: clipMouse not found in TimelineStrip");
            }
        }
    }

    // ------------------------------------------------------------------
    // Case 2d: TimelineStrip track header toggles
    // ------------------------------------------------------------------
    {
        editFake.clearCalls();
        auto surface = loadSurface(
                QStringLiteral("qrc:/qt/qml/GenesisAppTest/tests/app/qml/LoadTimelineStrip.qml"),
                editFake, timelineModelFake, mediaBinFake);
        if (surface) {
            auto *visToggle = findItemByName(surface->window.get(), "visToggle");
            auto *muteToggle = findItemByName(surface->window.get(), "muteToggle");

            if (visToggle) {
                QPointF c = centerOf(visToggle);
                sendMouseDeep(surface->window.get(), QEvent::MouseButtonPress, c, Qt::LeftButton,
                              Qt::LeftButton);
                sendMouseDeep(surface->window.get(), QEvent::MouseButtonRelease, c, Qt::LeftButton,
                              Qt::NoButton);
                bool vis = editFake.calls.contains(QStringLiteral("setTrackFlag:V1:0:false"));
                genesis::test::check(vis, "2d: visToggle click -> setTrackFlag:V1:0:false");
            } else {
                genesis::test::check(false, "2d: visToggle not found in TimelineStrip");
            }

            if (muteToggle) {
                QPointF c = centerOf(muteToggle);
                sendMouseDeep(surface->window.get(), QEvent::MouseButtonPress, c, Qt::LeftButton,
                              Qt::LeftButton);
                sendMouseDeep(surface->window.get(), QEvent::MouseButtonRelease, c, Qt::LeftButton,
                              Qt::NoButton);
                bool mute = editFake.calls.contains(QStringLiteral("setTrackFlag:V1:1:true"));
                genesis::test::check(mute, "2d: muteToggle click -> setTrackFlag:V1:1:true");
            } else {
                genesis::test::check(false, "2d: muteToggle not found in TimelineStrip");
            }
        }
    }

    // ------------------------------------------------------------------
    // Case 3: TransportBar undo/redo/delete
    // ------------------------------------------------------------------
    {
        editFake.clearCalls();
        editFake.setCanUndo(true);
        editFake.setCanRedo(true);
        editFake.setSelectedClipId("c1");

        auto surface = loadSurface(
                QStringLiteral("qrc:/qt/qml/GenesisAppTest/tests/app/qml/LoadTransportBar.qml"),
                editFake, timelineModelFake, mediaBinFake);
        if (surface) {
            auto *undoBtn = findItemByName(surface->window.get(), "undoButton");
            auto *redoBtn = findItemByName(surface->window.get(), "redoButton");
            auto *deleteBtn = findItemByName(surface->window.get(), "deleteButton");

            if (undoBtn) {
                QPointF c = centerOf(undoBtn);
                sendMouse(surface->window.get(), QEvent::MouseButtonPress, c, Qt::LeftButton,
                          Qt::LeftButton);
                sendMouse(surface->window.get(), QEvent::MouseButtonRelease, c, Qt::LeftButton,
                          Qt::NoButton);
                genesis::test::check(editFake.calls.contains("undo"), "3: undoButton -> undo");
            } else {
                genesis::test::check(false, "3: undoButton not found in TransportBar");
            }

            if (redoBtn) {
                QPointF c = centerOf(redoBtn);
                sendMouse(surface->window.get(), QEvent::MouseButtonPress, c, Qt::LeftButton,
                          Qt::LeftButton);
                sendMouse(surface->window.get(), QEvent::MouseButtonRelease, c, Qt::LeftButton,
                          Qt::NoButton);
                genesis::test::check(editFake.calls.contains("redo"), "3: redoButton -> redo");
            } else {
                genesis::test::check(false, "3: redoButton not found in TransportBar");
            }

            if (deleteBtn) {
                QPointF c = centerOf(deleteBtn);
                sendMouse(surface->window.get(), QEvent::MouseButtonPress, c, Qt::LeftButton,
                          Qt::LeftButton);
                sendMouse(surface->window.get(), QEvent::MouseButtonRelease, c, Qt::LeftButton,
                          Qt::NoButton);
                genesis::test::check(editFake.calls.contains("removeSelected"),
                                     "3: deleteButton -> removeSelected");
            } else {
                genesis::test::check(false, "3: deleteButton not found in TransportBar");
            }
        }
    }

    // ------------------------------------------------------------------
    // Case 4: ClipList row click -> select
    // ------------------------------------------------------------------
    {
        editFake.clearCalls();
        auto surface = loadSurface(
                QStringLiteral("qrc:/qt/qml/GenesisAppTest/tests/app/qml/LoadClipList.qml"),
                editFake, timelineModelFake, mediaBinFake);
        if (surface) {
            auto *row = findItemByName(surface->window.get(), "clipRow");
            if (row) {
                QPointF c = centerOf(row);
                sendMouse(surface->window.get(), QEvent::MouseButtonPress, c, Qt::LeftButton,
                          Qt::LeftButton);
                sendMouse(surface->window.get(), QEvent::MouseButtonRelease, c, Qt::LeftButton,
                          Qt::NoButton);
                genesis::test::check(editFake.calls.contains("select:c1"),
                                     "4: clipRow click -> select:c1");
            } else {
                genesis::test::check(false, "4: clipRow not found in ClipList");
            }
        }
    }

    // Zero-QML-messages contract: the smoke gate asserts zero QML runtime
    // messages. Mirror it here so a broken surface never silently regresses.
    genesis::test::check(g_warningCount == 0, "zero QML warnings on surface load");
    genesis::test::check(g_criticalCount == 0, "zero QML criticals on surface load");

    return genesis::test::summary();
}