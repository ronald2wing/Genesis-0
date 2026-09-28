// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell: the composition root that builds every app controller,
// wires their signals into the one refresh path, registers them with the QML
// engine, and runs the event loop. main() is only the GStreamer/GES and Qt
// bootstrap around it; everything past the QGuiApplication is here.

#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include <QString>

#include "workspace/Logs.h"

class QGuiApplication;
class QQuickWindow;

class AiController;
class CutoutController;
class EnhanceController;
class AppSettings;
class EditController;
class EffectParamsController;
class ExportController;
class ExtensionsController;
class HardwareStatus;
class I18n;
class KeyframesController;
class MediaBinController;
class PlaybackController;
class ProjectController;
class ProxyController;
class ReverseController;
class ScopesController;
class TimelineModel;
class TtsController;
class UpdateController;

namespace genesis::adapters::engine::ges {
class GesSession;
} // namespace genesis::adapters::engine::ges

namespace genesis::project {
class Editor;
} // namespace genesis::project

namespace genesis::render {
class Catalogue;
} // namespace genesis::render

// Owns every object the running app needs for its whole life: the Editor, the
// controller set, the GES session and the run log. main() constructs it with
// the app (the QObject parent of every controller) and the optional project
// path from argv[1], then calls run(). The controllers stay QObject children
// of the app, so their lifetime and destruction order are unchanged; only
// their construction and wiring moved out of main.
class AppShell
{
public:
    // `project_path` is the document/folder from argv (empty -> demo). The
    // remaining arguments are the dev-only smoke flags: `screenshot_path` (if
    // set) captures the window to that PNG and quits; `exit_after_ms` bounds
    // the wait (or is a clean auto-quit smoke when screenshot_path is empty);
    // `screenshot_open` invokes Main.qml's runAction(<action>) first so a
    // dialog can be captured.
    AppShell(QGuiApplication &app, QString project_path, QString screenshot_path = { },
             int exit_after_ms = 0, QString screenshot_open = { });
    ~AppShell();

    AppShell(const AppShell &) = delete;
    AppShell &operator=(const AppShell &) = delete;

    // Builds the project (argv load or the demo), constructs and wires every
    // controller, registers the QML context and pumps the event loop. Returns
    // the process exit code: 1 on any startup failure, app.exec() otherwise.
    int run();

private:
    // Loads argv[1] or builds the demo project; on failure logs why and
    // returns false. Fills media_path_ with where the header and the export
    // default point.
    bool setupProject();

    // The one reload path for cache-driven timeline rebuilds: re-resolve with
    // the reverse cache and, when proxies are enabled, the proxy cache. A
    // member so the reverse finish, the proxy finish and the enabled toggle
    // all rebuild identically instead of drifting apart.
    void reloadEngine();

    // The one refresh path after every applied edit or project load: rebuild
    // the timeline projection, then rebuild the engine timeline and reload the
    // session (which preserves the playhead and playing state).
    void refresh();

    // The dev smoke capture: dismiss the launch screen, optionally open a
    // dialog, then grab the window (first frame or the --exit-after deadline)
    // and quit. No-op when no --screenshot was passed.
    void startScreenshotCapture(QQuickWindow *window);

    QGuiApplication &app_;
    QString project_path_;
    QString screenshot_path_;
    int exit_after_ms_ = 0;
    QString screenshot_open_;
    bool screenshot_captured_ = false;

    // App directories: the recents config root and the run-log data root.
    QString config_dir_;
    QString log_root_;
    std::optional<genesis::workspace::LogFile> run_log_;

    // The path the header and the export default next to.
    QString media_path_;

    // Where the title PNGs, reverse copies, proxies and media caches live.
    // One path is the source of truth; the QString handed to the media bin is
    // derived from it once.
    std::filesystem::path cache_root_path_;

    // Heap-allocated so its address - held by every controller - is stable
    // across a project load, which swaps the project inside it.
    std::unique_ptr<genesis::project::Editor> editor_owner_;
    genesis::project::Editor *editor_ = nullptr;

    // The effect catalogue the engine resolves extension-contributed packs
    // against, built once at startup over the builtin source root plus every
    // loaded extension's pack directory. Declared before `session_` so it is
    // destroyed after the session, which holds a bare pointer to it.
    std::unique_ptr<genesis::render::Catalogue> extension_catalogue_;

    std::unique_ptr<genesis::adapters::engine::ges::GesSession> session_;

    ProjectController *project_ = nullptr;
    I18n *i18n_ = nullptr;
    TimelineModel *timelineModel_ = nullptr;
    PlaybackController *controller_ = nullptr;
    EditController *edit_ = nullptr;
    ExportController *exporter_ = nullptr;
    MediaBinController *mediaBin_ = nullptr;
    ReverseController *reverseController_ = nullptr;
    ProxyController *proxyController_ = nullptr;
    KeyframesController *keyframes_ = nullptr;
    EffectParamsController *effectParams_ = nullptr;
    ExtensionsController *extensions_ = nullptr;
    ScopesController *scopesController_ = nullptr;
    HardwareStatus *hardware_ = nullptr;
    AppSettings *settings_ = nullptr;
    AiController *captions_ = nullptr;
    CutoutController *cutout_ = nullptr;
    EnhanceController *enhance_ = nullptr;
    TtsController *tts_ = nullptr;
    UpdateController *updates_ = nullptr;
};
