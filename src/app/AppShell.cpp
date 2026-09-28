// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell: the composition root extracted from the old
// GenesisApp.cpp main(). It builds the project (argv load or the demo),
// constructs every controller, wires their signals into the one refresh path,
// registers the QML context, and pumps the event loop.

#include "app/AppShell.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <nlohmann/json.hpp>

#include "adapters/engine/EngineSession.h"
#include "adapters/engine/ges/Hardware.h"
#include "adapters/engine/ges/session/GesSession.h"
#include "adapters/engine/ges/media/Probe.h"
#include "adapters/engine/ges/scopes/Scopes.h"
#include "ai/vision/ExecutionProvider.h"
#include "api/Api.h"
#include "app/support/AppLog.h"
#include "app/Version.h"
#include "app/controllers/AiController.h"
#include "app/controllers/CutoutController.h"
#include "app/controllers/EnhanceController.h"
#include "app/controllers/AppSettings.h"
#include "app/controllers/EditController.h"
#include "app/controllers/EffectParamsController.h"
#include "app/controllers/ExtensionsController.h"
#include "app/render/BuildForEngine.h"
#include "app/controllers/ExportController.h"
#include "app/controllers/HardwareStatus.h"
#include "app/support/I18n.h"
#include "app/controllers/KeyframesController.h"
#include "app/controllers/MediaBinController.h"
#include "app/model/MonitorItem.h"
#include "app/controllers/PlaybackController.h"
#include "app/controllers/ProjectController.h"
#include "app/controllers/ProxyController.h"
#include "app/controllers/ReverseController.h"
#include "app/controllers/ScopesController.h"
#include "app/model/TimelineModel.h"
#include "app/controllers/TtsController.h"
#include "app/controllers/UpdateController.h"
#include "extensions/Config.h"
#include "extensions/Seed.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/Editor.h"
#include "render/Catalogue.h"
#include "render/Resolve.h"

using genesis::adapters::engine::FrameTexture;
using genesis::adapters::engine::GlHandles;
using genesis::app::app_log;
namespace ges_adapter = genesis::adapters::engine::ges;
namespace render = genesis::render;
using namespace genesis::project;
using genesis::core::Rational;

namespace {

// The marketplace-catalog fetch: a synchronous, bounded Qt GET on the calling
// thread (the UI thread for a panel's call), the same shape and threading as
// UpdateController's feed fetch. The catalog is a small JSON document, so a
// short transfer timeout bounds a stalled fetch instead of hanging the bridge.
std::optional<std::string> catalog_http_fetch(const std::string &url)
{
    QNetworkAccessManager manager;
    QNetworkRequest request{ QUrl(QString::fromStdString(url)) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(10 * 1000);
    QNetworkReply *reply = manager.get(request);
    if (reply == nullptr) {
        return std::nullopt;
    }
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return std::nullopt;
    }
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    return std::string(body.constData(), static_cast<std::size_t>(body.size()));
}

} // namespace

AppShell::AppShell(QGuiApplication &app, QString project_path, QString screenshot_path,
                   int exit_after_ms, QString screenshot_open)
    : app_(app),
      project_path_(std::move(project_path)),
      screenshot_path_(std::move(screenshot_path)),
      exit_after_ms_(exit_after_ms),
      screenshot_open_(std::move(screenshot_open))
{
}

AppShell::~AppShell() = default;

bool AppShell::setupProject()
{
    if (!project_path_.isEmpty()) {
        // argv[1] is a project document or a project folder. Fail loudly
        // rather than guess when it cannot load - the same posture the old
        // media-path branch took for a missing file.
        if (!project_->openProject(project_path_)) {
            app_log("[app] could not load %s\n", project_path_.toUtf8().constData());
            return false;
        }
        media_path_ = project_->path();
        return true;
    }

    // No argument: build the demo project from the spike's deterministic
    // fixture. Both candidate paths are checked so the app still runs on
    // a machine with either; failing loudly beats guessing when neither
    // exists.
    const std::filesystem::path fixture =
            std::filesystem::temp_directory_path() / "ges-spike" / "a.webm";
    const std::filesystem::path session_fixture =
            std::filesystem::temp_directory_path() / "opencode" / "clip.webm";
    std::filesystem::path chosen;
    if (std::filesystem::exists(fixture)) {
        chosen = fixture;
    } else if (std::filesystem::exists(session_fixture)) {
        chosen = session_fixture;
    } else {
        app_log("[app] no media: pass a document, or create %s\n", fixture.c_str());
        return false;
    }
    media_path_ = QString::fromStdString(chosen.string());

    // Probe the file for the metadata AddMedia needs.
    const auto probed = ges_adapter::probe(std::filesystem::path(media_path_.toStdString()));
    if (!probed) {
        app_log("[app] probe failed for %s\n", media_path_.toUtf8().constData());
        return false;
    }

    // Build a small demo project through the host command layer: one
    // media item, two clips of it placed back to back on the first
    // track.
    const auto added = editor_->apply(Command{ AddMedia{ std::move(*probed) } });
    if (!added || !added->created_id) {
        app_log("[app] AddMedia failed\n");
        return false;
    }
    const std::string media_id = *added->created_id;
    const std::string track_id = editor_->project().timelines[0].tracks[0].id;

    const auto clip1 =
            editor_->apply(Command{ AddClip{ media_id, track_id, Rational{ 0, 1 }, false } });
    if (!clip1 || !clip1->created_id) {
        app_log("[app] AddClip failed\n");
        return false;
    }
    // The second clip starts where the first ends, whatever duration
    // AddClip assigned it (the media length, or the five-second
    // unknown-length fallback) - reading it back keeps the two clips
    // exactly contiguous.
    const Rational clip2_start = editor_->project().timelines[0].clip(*clip1->created_id)->duration;
    const auto clip2 = editor_->apply(Command{ AddClip{ media_id, track_id, clip2_start, false } });
    if (!clip2) {
        app_log("[app] AddClip failed\n");
        return false;
    }

    // The demo project has no manifest or document of its own; name it
    // for the header and title the shell by its media path.
    project_->adopt(QStringLiteral("Demo"), media_path_);
    return true;
}

void AppShell::reloadEngine()
{
    controller_->reload(build_for_engine(
            editor_->project(), cache_root_path_,
            proxyController_->enabled() ? std::optional<std::filesystem::path>(cache_root_path_)
                                        : std::nullopt,
            cache_root_path_ / "masks", extension_catalogue_.get()));
}

void AppShell::refresh()
{
    timelineModel_->rebuild(editor_->project());
    mediaBin_->setItems(editor_->project());
    mediaBin_->generateMissingCaches();
    reverseController_->generateMissingReverses(editor_->project(), cache_root_path_);
    proxyController_->generateMissingProxies(editor_->project(), cache_root_path_);
    reloadEngine();
}

int AppShell::run()
{
    // The config directory the recents file lives in.
    config_dir_ = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (config_dir_.isEmpty()) {
        config_dir_ = QString::fromStdString(
                (std::filesystem::temp_directory_path() / "genesis-0-config").string());
    }

    // The run log: a persistent debug trail under the app's data directory,
    // the conventional home for data that is neither config nor cache. Opened
    // here so it spans the app and outlives every caller; a failed open is
    // never fatal - the run continues with stderr as the only sink.
    log_root_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (log_root_.isEmpty()) {
        log_root_ = QString::fromStdString(
                (std::filesystem::temp_directory_path() / "genesis-0-data").string());
    }
    run_log_ = genesis::workspace::LogFile::open(std::filesystem::path(log_root_.toStdString())
                                                 / "logs");
    if (run_log_) {
        genesis::app::set_log_file(&*run_log_);
    }
    app_log("[app] genesis-0 starting\n");

    editor_owner_ = std::make_unique<Editor>();
    editor_ = editor_owner_.get();

    // The project-load facade and the string catalogue, both QML-facing.
    project_ =
            new ProjectController(editor_, std::filesystem::path(config_dir_.toStdString()), &app_);
    i18n_ = new I18n(&app_);

    if (!setupProject()) {
        return 1;
    }

    // Where the rasterised titles land, beside the media caches. The title
    // rasteriser shares the MediaCache root and naming convention, so the
    // media bin and the titles live under the same cache directory. One path
    // is the source of truth; the media bin's QString is derived from it.
    cache_root_path_ = std::filesystem::path(
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation).toStdString());
    if (cache_root_path_.empty()) {
        cache_root_path_ = std::filesystem::temp_directory_path() / "genesis-0-cache";
    }
    const QString cache_root_str = QString::fromStdString(cache_root_path_.string());

    // Seed the installed-extension store from the builtin source before the
    // controller scans it, so a fresh (or freshly reset) store has its builtins
    // the first time the host runs. Best-effort: a problem is logged, never
    // fatal - the controller scans whatever is (or is not) there. The one store
    // root is shared by the seed, the controller and the config seams.
    const std::filesystem::path extensions_store =
            std::filesystem::path(log_root_.toStdString()) / "extensions";
    const std::vector<std::string> seed_problems =
            genesis::extensions::seed(extensions_store, genesis::extensions::builtin_root());
    for (const std::string &problem : seed_problems) {
        app_log("[app] extension seed: %s\n", problem.c_str());
    }

    // The extension controller: owns the host that scans the installed-
    // extension store under the app-data root and loads every loadable native
    // extension. It is built before the first timeline so its contributed pack
    // roots fold into the catalogue the engine resolves effects against.
    extensions_ = new ExtensionsController(editor_, extensions_store, &app_);
    // The official pack root is the seeded builtin extension's packs directory
    // in the store (plus any other extension-contributed packs); fall back to
    // the source-tree location when the store has not been seeded yet, so a
    // fresh developer build resolves packs before its first seed.
    std::vector<std::filesystem::path> pack_roots;
    const std::vector<std::filesystem::path> &store_pack_roots = extensions_->pack_roots();
    if (!store_pack_roots.empty()) {
        pack_roots = store_pack_roots;
    } else {
        const std::filesystem::path source = render::Catalogue::source_root();
        if (!source.empty()) {
            pack_roots.push_back(source);
        }
    }
    if (!pack_roots.empty()) {
        extension_catalogue_ = std::make_unique<render::Catalogue>(
                render::Catalogue::from_directories(std::move(pack_roots)));
    }

    render::BuiltTimeline built =
            build_for_engine(editor_->project(), cache_root_path_, std::nullopt,
                             cache_root_path_ / "masks", extension_catalogue_.get());

    // Read-only projection of the host project for the QML shell. Built by
    // value (QString/double copies), so it can outlive `editor` without
    // holding a reference into it. Rebuilt after every edit (see refresh()).
    timelineModel_ = new TimelineModel(editor_->project(), &app_);

    // The session the controller drives, on the host's side of the seam.
    session_ = std::make_unique<ges_adapter::GesSession>();
    controller_ = new PlaybackController(session_.get(), std::move(built));

    // The edit controller: UI intent -> host commands, applied through the
    // Editor. It owns the gesture/undo semantics; the shell only refreshes its
    // projection and reloads the engine when it reports a change.
    edit_ = new EditController(editor_, &app_);

    // The export controller: form values -> one blocking render, driven off
    // the UI thread (R4). It defaults its output next to the media the app
    // opened, and pauses playback before a run so the preview and the export
    // never contend for decoders or the hardware encoder.
    exporter_ = new ExportController(editor_, &app_);
    exporter_->setOutputPath(QString::fromStdString(
            (std::filesystem::path(media_path_.toStdString()).parent_path() / "export.webm")
                    .string()));
    exporter_->setCacheRoot(cache_root_path_);
    exporter_->setPausePlayback([this]() { controller_->pause(); });
    // Export lifecycle on the log: the run's start, then its outcome. The
    // controller has no start signal, so `running` turning true is it.
    QObject::connect(exporter_, &ExportController::runningChanged, &app_, [this]() {
        if (exporter_->running()) {
            app_log("[app] export started: %s\n", exporter_->outputPath().toUtf8().constData());
        }
    });
    QObject::connect(exporter_, &ExportController::finished, &app_,
                     [](bool ok, const QString &error, double seconds) {
                         if (ok) {
                             app_log("[app] export finished: %.3fs\n", seconds);
                         } else {
                             app_log("[app] export failed: %s\n", error.toUtf8().constData());
                         }
                     });

    // The media bin controller: owns the id -> cache-path map and the
    // off-thread poster/filmstrip generation. It reads the host project's
    // media directly (setItems) and hands the QML pane its cache lookups;
    // the pane's metadata comes from the TimelineModel projection instead.
    mediaBin_ = new MediaBinController(&app_);
    mediaBin_->setCacheRoot(cache_root_str);
    mediaBin_->setEditor(editor_);
    mediaBin_->setItems(editor_->project());
    mediaBin_->generateMissingCaches();

    // The reverse controller: owns the off-thread generation of the reversed
    // copies a preview's reversed clips read through. It flattens the project
    // itself and fills the reverse cache off the UI thread (R4); when a run
    // ends it reloads the engine timeline so a freshly generated reverse lands
    // on the next build.
    reverseController_ = new ReverseController(&app_);

    // The proxy controller: owns the off-thread generation of the whole-source
    // downscales a preview's forward-playing clips read through. It fills the
    // proxy cache off the UI thread (R4); when a run ends - or the enabled
    // toggle changes - the shell reloads the engine timeline so a fresh proxy
    // lands on the next build. Export never consults it and reads originals.
    proxyController_ = new ProxyController(&app_);

    QObject::connect(reverseController_, &ReverseController::finished, &app_,
                     [this]() { reloadEngine(); });
    QObject::connect(proxyController_, &ProxyController::finished, &app_,
                     [this]() { reloadEngine(); });
    reverseController_->generateMissingReverses(editor_->project(), cache_root_path_);
    proxyController_->generateMissingProxies(editor_->project(), cache_root_path_);

    // Turning proxies on kicks generation and re-resolves the timeline against
    // the proxy cache; turning it off re-resolves back to the originals. Both
    // directions take the same reload path.
    QObject::connect(proxyController_, &ProxyController::enabledChanged, &app_, [this]() {
        if (proxyController_->enabled()) {
            proxyController_->generateMissingProxies(editor_->project(), cache_root_path_);
        }
        reloadEngine();
    });

    // The keyframes controller: the pane's view state plus the thin mapping
    // from pane intent to key commands, applied through the same Editor so
    // every key edit is undoable. It owns no QML - only the selected clip's
    // projection and the add/remove/ease/move gestures the pane drives.
    keyframes_ = new KeyframesController(editor_, &app_);

    // The effect-parameter controller: the selected clip's applied effect
    // chain and its parameters' current values for the inspector, plus the
    // one-command setter. The shell sets its clip id (like the keyframes
    // controller's); a parameter edit reloads the engine through the same
    // projectEdited path as every other edit.
    effectParams_ = new EffectParamsController(editor_, &app_);

    // The AI captions controller: one "captions" request becomes a single
    // undoable batch of title clips over the selected clip, downloaded and
    // transcribed off the UI thread (R4). Models live under the app-data root
    // (never the cache root), so a download is a durable, consent-gated
    // install, not an evictable cache entry.
    captions_ = new AiController(editor_, &app_);
    captions_->setModelRoot(std::filesystem::path(log_root_.toStdString()) / "models");

    // The AI cutout controller: one "cutout" request becomes a single
    // undoable SetClipCutout over the selected clip, its masks downloaded and
    // generated off the UI thread (R4). Models live under the app-data root
    // (never the cache root); masks live under the cache root's masks folder,
    // the same folder the render path reads so generated masks are found
    // automatically.
    cutout_ = new CutoutController(editor_, &app_);
    cutout_->setModelRoot(std::filesystem::path(log_root_.toStdString()) / "models");
    cutout_->setMaskRoot(cache_root_path_ / "masks");

    // The AI enhance controller: one "enhance" request becomes a single
    // undoable ReplaceClipMedia over the selected clip, its model downloaded
    // and the upscaled copy encoded off the UI thread (R4). Models live under
    // the app-data root (never the cache root); the enhanced copy lands next to
    // the media the app opened, so a generated copy sits beside its source.
    enhance_ = new EnhanceController(editor_, &app_);
    enhance_->setModelRoot(std::filesystem::path(log_root_.toStdString()) / "models");
    enhance_->setOutputRoot(std::filesystem::path(media_path_.toStdString()).parent_path());

    // The AI text-to-speech controller: one "speak" request becomes a single
    // undoable narration clip in the bin and on the timeline, the model
    // downloaded, unpacked and the text synthesized off the UI thread (R4).
    // Models live under the app-data root (never the cache root), like the
    // other AI controllers; the generated WAV lands next to the media the app
    // opened, so a narration sits beside its source.
    tts_ = new TtsController(editor_, &app_);
    tts_->setModelRoot(std::filesystem::path(log_root_.toStdString()) / "models");
    tts_->setOutputRoot(std::filesystem::path(media_path_.toStdString()).parent_path());
    // Cloned voice recordings persist under the app-data root, so a captured
    // voice survives a restart; the controller re-lists them on start.
    tts_->setVoiceRoot(std::filesystem::path(log_root_.toStdString()) / "voices");

    // No QML drives the captions pane yet, so the shell follows selection the
    // way KeyframesPane.qml does for the keyframes controller: the selected
    // clip id becomes the controller's target, and a cleared selection clears
    // it.
    QObject::connect(edit_, &EditController::selectionChanged, &app_, [this]() {
        captions_->setClipId(edit_->selectedClipId());
        cutout_->setClipId(edit_->selectedClipId());
        enhance_->setClipId(edit_->selectedClipId());
    });

    // A committed stroke re-masks its clip: the cutout controller debounces
    // the burst and regenerates once the painting pauses, so the timeline's
    // undo state (one AddCutoutStroke) stays separate from the mask cache.
    QObject::connect(edit_, &EditController::cutoutStrokeAdded, cutout_,
                     &CutoutController::scheduleRegenerate);

    // The scopes controller: the pane's view state plus the off-thread
    // sampling of the engine's scope frames. Its frame source is wired below,
    // alongside the Scopes readback adapter, once the GL context exists.
    scopesController_ = new ScopesController(&app_);

    // The settings facade the SettingsDialog binds to: cache root, decode
    // preference, scopes interval and the proxy toggle, each forwarded to its
    // real owner.
    settings_ = new AppSettings(mediaBin_, controller_, scopesController_, proxyController_, &app_);

    // The notify-only update controller the Settings section binds to. It only
    // parses and verifies the feed, then compares versions - download and apply
    // stay the platform channel's job (docs/decisions/auto-update.md §3(d)).
    // The feed URL persists to <config>/updates.json like the recents file.
    updates_ = new UpdateController(QString(), &app_);
    updates_->setConfigDir(std::filesystem::path(config_dir_.toStdString()));

    // The hardware-status controller: a read-only projection of the engine's
    // GPU probe plus the playback decode preference, exposed to QML as `gpu`.
    // The probe is a bounded one-shot capability check (renderer string from a
    // throwaway offscreen GL context, decoder registry inspection, encoder
    // READY probes), so it runs once at startup and the indicator never calls
    // back into the engine.
    const ges_adapter::HardwareInfo gpu_info = ges_adapter::probe_hardware();
    const std::string ai_mode(genesis::ai::ai_execution_mode());
    hardware_ = new HardwareStatus(&app_);
    hardware_->setProbe(QString::fromStdString(gpu_info.renderer), gpu_info.software_gl,
                        gpu_info.hardware_decode, gpu_info.hardware_encode,
                        QString::fromStdString(gpu_info.encode_element));
    hardware_->setDecodePreference(controller_->decodePreference());
    hardware_->setAi(QString::fromUtf8(ai_mode.c_str()));
    QObject::connect(controller_, &PlaybackController::decodePreferenceChanged, hardware_,
                     [this]() { hardware_->setDecodePreference(controller_->decodePreference()); });
    app_log("[app] gpu: renderer=%s software_gl=%d decode=%s encode=%s (%s) "
            "ai=%s\n",
            gpu_info.renderer.c_str(), gpu_info.software_gl ? 1 : 0,
            gpu_info.hardware_decode ? "hardware" : "software",
            gpu_info.hardware_encode ? "hardware" : "software",
            gpu_info.encode_element.empty() ? "none" : gpu_info.encode_element.c_str(),
            ai_mode.c_str());

    // The host API services the extension bridge reaches: the app-owned verbs
    // (export, scopes, selection, playhead, media, gpu) wired to the real
    // controllers, now that every one exists. The extension controller keeps
    // its store seam and adopts the rest, so a panel calling export.run reaches
    // this runner instead of a typed refusal.
    genesis::api::Services services;
    services.export_run = [exporter = exporter_](const genesis::api::ExportRun &run) {
        if (!run.spec.output.empty()) {
            exporter->setOutputPath(QString::fromStdString(run.spec.output));
        }
        if (run.spec.preset) {
            exporter->setPreset(QString::fromStdString(*run.spec.preset));
        }
        if (run.spec.width) {
            exporter->setWidth(static_cast<int>(*run.spec.width));
        }
        if (run.spec.height) {
            exporter->setHeight(static_cast<int>(*run.spec.height));
        }
        if (run.spec.rate_num) {
            exporter->setRateNum(static_cast<qint64>(*run.spec.rate_num));
        }
        if (run.spec.rate_den) {
            exporter->setRateDen(static_cast<qint64>(*run.spec.rate_den));
        }
        exporter->startExport();
        return genesis::api::Response{ genesis::api::Reply{ genesis::api::Started{
                "export", run.path, exporter->outputPath().toStdString() } } };
    };
    services.export_cancel = [exporter = exporter_](const genesis::api::ExportCancel &) {
        exporter->cancelExport();
        return genesis::api::Response{ genesis::api::Reply{ genesis::api::Done{ } } };
    };
    services.export_status = [exporter = exporter_](const genesis::api::ExportStatus &request) {
        genesis::api::ExportStatusView view;
        view.job = request.job;
        view.running = exporter->running();
        view.progress = exporter->progress();
        view.status = exporter->status().toStdString();
        view.error = exporter->error().toStdString();
        return genesis::api::Response{ genesis::api::Reply{ std::move(view) } };
    };
    services.scopes_snapshot =
            [scopes = scopesController_](const genesis::api::ScopesSnapshot &request) {
                const auto ints = [](const QVector<int> &values) {
                    nlohmann::json out = nlohmann::json::array();
                    for (const int value : values) {
                        out.push_back(value);
                    }
                    return out;
                };
                nlohmann::json out = nlohmann::json::object();
                out["idle"] = scopes->idle();
                out["mode"] = scopes->mode();
                if (request.kind == "waveform") {
                    out["kind"] = "waveform";
                    out["lumaMin"] = ints(scopes->waveformLumaMin());
                    out["lumaMax"] = ints(scopes->waveformLumaMax());
                    out["redMin"] = ints(scopes->waveformRedMin());
                    out["redMax"] = ints(scopes->waveformRedMax());
                    out["greenMin"] = ints(scopes->waveformGreenMin());
                    out["greenMax"] = ints(scopes->waveformGreenMax());
                    out["blueMin"] = ints(scopes->waveformBlueMin());
                    out["blueMax"] = ints(scopes->waveformBlueMax());
                } else if (request.kind == "vectorscope") {
                    out["kind"] = "vectorscope";
                    out["size"] = genesis::adapters::engine::kScopeLevels;
                    out["density"] = ints(scopes->vectorscope());
                } else {
                    out["kind"] = "histogram";
                    out["levels"] = ints(scopes->histogram());
                }
                return genesis::api::Response{ genesis::api::Reply{ std::move(out) } };
            };
    services.edit_selection = [edit = edit_] { return edit->selectedClipId().toStdString(); };
    services.preview_time = [controller = controller_] { return controller->position(); };
    services.media_list = [editor = editor_]() -> std::vector<genesis::project::MediaItem> {
        return editor->project().media;
    };
    services.gpu_status = [hardware = hardware_]() {
        genesis::api::GpuStatusView view;
        view.renderer = hardware->renderer().toStdString();
        view.software_gl = hardware->softwareGl();
        view.decode = hardware->decode().toStdString();
        view.encode = hardware->encode().toStdString();
        view.encode_element = hardware->encodeElement().toStdString();
        view.ai = hardware->ai().toStdString();
        return view;
    };

    // The marketplace-catalog fetch, wired to the same bounded Qt GET the feed
    // check uses. A panel's `extensions.catalog` call runs this on the UI
    // thread, so the short transfer timeout is the safety bound against a
    // stalled host.
    services.extensions_catalog = genesis::api::extensions_catalog_seam(catalog_http_fetch);

    // The config profile seams: settings read/write through the settings facade
    // and the update controller's feed URL, extensions through the shared
    // store. Export carries the running build's version; import honors the
    // never-elevate-trust rules inside import_profile (this seam only supplies
    // the settings and the store). A getter that reports nothing (an unset
    // feed URL) omits the field from the exported profile, matching the
    // "only carry what the host reports" contract.
    genesis::extensions::ConfigSeam config_seam;
    config_seam.get_proxies_enabled = [settings = settings_]() -> std::optional<bool> {
        return settings->proxiesEnabled();
    };
    config_seam.get_decode_preference = [settings = settings_]() -> std::optional<int> {
        return settings->decodePreference();
    };
    config_seam.get_scopes_interval_ms = [settings = settings_]() -> std::optional<int> {
        return settings->scopesIntervalMs();
    };
    config_seam.get_feed_url = [updates = updates_]() -> std::optional<std::string> {
        const std::string url = updates->feedUrl().toStdString();
        if (url.empty()) {
            return std::nullopt;
        }
        return url;
    };
    config_seam.set_proxies_enabled = [settings = settings_](bool value) {
        settings->setProxiesEnabled(value);
    };
    config_seam.set_decode_preference = [settings = settings_](int value) {
        settings->setDecodePreference(value);
    };
    config_seam.set_scopes_interval_ms = [settings = settings_](int value) {
        settings->setScopesIntervalMs(value);
    };
    config_seam.set_feed_url = [updates = updates_](const std::string &value) {
        updates->setFeedUrl(QString::fromStdString(value));
    };
    services.config_export = genesis::api::config_export_seam(
            extensions_store, config_seam, std::string(genesis::app::kAppVersion));
    services.config_import = genesis::api::config_import_seam(extensions_store, config_seam);

    extensions_->setHostServices(std::move(services));

    // After every applied edit, refresh() rebuilds the projection, the engine
    // timeline and reloads the session, so the host project, the UI and the
    // engine can never drift apart. The bin re-points at the (possibly
    // changed) media list on the same tick. Key edits feed the same path, so
    // a key change reloads the engine exactly like a move or trim does.
    QObject::connect(edit_, &EditController::projectEdited, &app_, [this]() { refresh(); });
    QObject::connect(keyframes_, &KeyframesController::projectEdited, &app_,
                     [this]() { refresh(); });
    QObject::connect(effectParams_, &EffectParamsController::projectEdited, &app_,
                     [this]() { refresh(); });
    QObject::connect(captions_, &AiController::projectEdited, &app_, [this]() { refresh(); });
    QObject::connect(cutout_, &CutoutController::projectEdited, &app_, [this]() { refresh(); });
    QObject::connect(enhance_, &EnhanceController::projectEdited, &app_, [this]() { refresh(); });
    QObject::connect(tts_, &TtsController::projectEdited, &app_, [this]() { refresh(); });
    QObject::connect(mediaBin_, &MediaBinController::projectEdited, &app_, [this]() { refresh(); });
    QObject::connect(extensions_, &ExtensionsController::projectEdited, &app_,
                     [this]() { refresh(); });
    // A project load swaps the project inside the shared Editor wholesale, so
    // a stale clip selection would point into the old project. Drop it, then
    // run the one refresh path exactly like an edit would.
    QObject::connect(project_, &ProjectController::projectChanged, &app_, [this]() {
        // A new project has replaced the old: name it on the
        // log so a run's trail records what was open and when
        // it changed.
        app_log("[app] project: %s (%s)\n", project_->name().toUtf8().constData(),
                project_->path().toUtf8().constData());
        edit_->clearSelection();
        refresh();
    });
    // A failed Open is surfaced on the log; the start screen also reads the
    // return value of openProject so it can leave the current project
    // untouched on a bad path.
    QObject::connect(project_, &ProjectController::loadFailed, &app_, [](const QString &message) {
        app_log("[app] load failed: %s\n", message.toUtf8().constData());
    });
    // A failed Save is surfaced the same way; the shell's Save As dialog
    // keeps the failure path off the happy path by reading the return value.
    QObject::connect(project_, &ProjectController::saveFailed, &app_, [](const QString &message) {
        app_log("[app] save failed: %s\n", message.toUtf8().constData());
    });

    // Key edits change the shared Editor's undo/redo/dirty state; re-emit the
    // EditController's historyChanged so the toolbar and title bar (which read
    // edit.canUndo/canRedo/dirty) stay in step without learning about the
    // keyframes controller.
    QObject::connect(keyframes_, &KeyframesController::historyChanged, edit_,
                     &EditController::historyChanged);
    QObject::connect(effectParams_, &EffectParamsController::historyChanged, edit_,
                     &EditController::historyChanged);
    QObject::connect(captions_, &AiController::historyChanged, edit_,
                     &EditController::historyChanged);
    QObject::connect(cutout_, &CutoutController::historyChanged, edit_,
                     &EditController::historyChanged);
    QObject::connect(enhance_, &EnhanceController::historyChanged, edit_,
                     &EditController::historyChanged);
    QObject::connect(tts_, &TtsController::historyChanged, edit_, &EditController::historyChanged);
    QObject::connect(mediaBin_, &MediaBinController::historyChanged, edit_,
                     &EditController::historyChanged);
    QObject::connect(extensions_, &ExtensionsController::historyChanged, edit_,
                     &EditController::historyChanged);
    // The title bar reads the project's own dirty flag; forward every
    // history change (edit, keyframes, and the other controllers) to it so it
    // re-reads after each edit, and ProjectController re-reads it itself
    // after a save.
    QObject::connect(edit_, &EditController::historyChanged, project_,
                     &ProjectController::dirtyChanged);

    // The running app routes its audio to the system's default output; a
    // headless run (tests, export) never constructs this session. The probe
    // reports whether a device actually opened, so the log is honest on a
    // machine with no sound card.
    const bool audio_out = session_->set_audio_output(true);
    app_log("[app] audio output: %s\n", audio_out ? "on" : "off");

    QQmlApplicationEngine qml;
    qml.rootContext()->setContextProperty(QStringLiteral("controller"), controller_);
    qml.rootContext()->setContextProperty(QStringLiteral("timelineModel"), timelineModel_);
    qml.rootContext()->setContextProperty(QStringLiteral("edit"), edit_);
    // Named `exporter` (not `export` - a reserved word in QML/JS).
    qml.rootContext()->setContextProperty(QStringLiteral("exporter"), exporter_);
    qml.rootContext()->setContextProperty(QStringLiteral("mediaBin"), mediaBin_);
    qml.rootContext()->setContextProperty(QStringLiteral("keyframes"), keyframes_);
    qml.rootContext()->setContextProperty(QStringLiteral("effectParams"), effectParams_);
    qml.rootContext()->setContextProperty(QStringLiteral("scopes"), scopesController_);
    qml.rootContext()->setContextProperty(QStringLiteral("extensions"), extensions_);
    qml.rootContext()->setContextProperty(QStringLiteral("captions"), captions_);
    qml.rootContext()->setContextProperty(QStringLiteral("cutout"), cutout_);
    qml.rootContext()->setContextProperty(QStringLiteral("enhance"), enhance_);
    qml.rootContext()->setContextProperty(QStringLiteral("tts"), tts_);
    qml.rootContext()->setContextProperty(QStringLiteral("settings"), settings_);
    qml.rootContext()->setContextProperty(QStringLiteral("updates"), updates_);
    qml.rootContext()->setContextProperty(QStringLiteral("gpu"), hardware_);
    qml.rootContext()->setContextProperty(QStringLiteral("project"), project_);
    qml.rootContext()->setContextProperty(QStringLiteral("i18n"), i18n_);

    // Load through a component rather than the engine so a failure can be
    // REPORTED. QQmlApplicationEngine::load returns void and, on this Qt,
    // prints nothing at all when a scene fails to resolve - the app said only
    // "QML failed to load", which names neither the file nor the line. A
    // component hands back the errors.
    QQmlComponent component(
            &qml,
            QUrl::fromLocalFile(QStringLiteral(GENESIS_QML_DIR) + QStringLiteral("/Main.qml")));
    if (component.isError()) {
        for (const QQmlError &error : component.errors()) {
            app_log("[qml] %s\n", error.toString().toUtf8().constData());
        }
        return 1;
    }
    QObject *root = component.create();
    if (root == nullptr) {
        app_log("[app] QML created no root object\n");
        return 1;
    }

    // The MonitorItem was created by the QML scene; hand it to the controller
    // and start once the scene (and its GL context) is up.
    // `root`, not `qml.rootObjects().first()`: a component's create() does not
    // register with the engine, so rootObjects() stays empty on this path.
    auto *window = qobject_cast<QQuickWindow *>(root);
    auto *monitor = window ? window->findChild<MonitorItem *>() : nullptr;
    if (!monitor) {
        app_log("[app] MonitorItem not found in the scene\n");
        return 1;
    }
    controller_->setMonitor(monitor);

    // The scopes readback adapter, built once the controller has acquired the
    // app's GL context and emitted its handles. This composition root is the
    // only place that constructs a ges::Scopes: the controller stays
    // engine-neutral and just hands the handles over. The frame observer is
    // wired here too - it submits each presented frame for the non-blocking
    // scopes readback (keeping the texture alive for it) and stages it on the
    // monitor - so the controller owns no engine-side adapter.
    std::unique_ptr<ges_adapter::Scopes> scopes;
    QObject::connect(controller_, &PlaybackController::glHandlesReady, &app_,
                     [this, &scopes, monitor](GlHandles handles) {
                         scopes = std::make_unique<ges_adapter::Scopes>(handles);
                         scopesController_->setFrameSource([&scopes] { return scopes->latest(); });
                         controller_->setFrameObserver([&scopes, monitor](FrameTexture frame) {
                             scopes->read_back(frame.texture_id, frame.width, frame.height,
                                               frame.keep_alive);
                             monitor->stageFrame(std::move(frame));
                         });
                     });
    controller_->start();

    // Presentation diagnostic: report the committed-node count (and how many
    // frames the engine handed over) once a second, so a headless observer can
    // tell whether updatePaintNode is committing textured nodes.
    QTimer *diag = new QTimer(&app_);
    QObject::connect(diag, &QTimer::timeout, [monitor, controller = controller_]() {
        app_log("[app] rendered=%d framesSeen=%llu\n", monitor->rendered(),
                static_cast<unsigned long long>(controller->framesSeen()));
    });
    diag->start(1000);

    // Dev smoke hooks: capture the window to a PNG (--screenshot) and/or quit
    // after a budget (--exit-after). Gated so a normal launch is untouched.
    if (!screenshot_path_.isEmpty()) {
        startScreenshotCapture(window);
    } else if (exit_after_ms_ > 0) {
        QTimer::singleShot(exit_after_ms_, &app_, [this]() { app_.quit(); });
    }

    const int exit_code = app_.exec();
    app_log("[app] shutting down: exit=%d\n", exit_code);
    return exit_code;
}

void AppShell::startScreenshotCapture(QQuickWindow *window)
{
    // The launch screen (z:100) covers the editor; dismiss it so the capture
    // shows the shell. An optional --screenshot-open opens one dialog through
    // the same runAction the menu shortcuts use, so a dialog surface can be
    // captured too. Both are QML-declared functions, reachable through the
    // meta-object.
    if (!screenshot_open_.isEmpty()) {
        QMetaObject::invokeMethod(window, "runAction", Q_ARG(QVariant, screenshot_open_));
    }
    if (auto *start = window->findChild<QObject *>(QStringLiteral("startScreen"))) {
        QMetaObject::invokeMethod(start, "dismiss");
    }

    // Grab once - on the --exit-after deadline (so engine frames have time to
    // reach the monitor) or, without one, on the first rendered frame with a
    // safety net - then save and quit.
    auto capture = [this, window]() {
        if (screenshot_captured_) {
            return;
        }
        screenshot_captured_ = true;
        const QImage image = window->grabWindow();
        if (!image.isNull() && image.save(screenshot_path_)) {
            app_log("[app] screenshot saved: %s (%dx%d)\n", screenshot_path_.toUtf8().constData(),
                    image.width(), image.height());
        } else {
            app_log("[app] screenshot failed: %s\n", screenshot_path_.toUtf8().constData());
        }
        app_.quit();
    };

    if (exit_after_ms_ > 0) {
        QTimer::singleShot(exit_after_ms_, window, capture);
    } else {
        QObject::connect(
                window, &QQuickWindow::afterRendering, window,
                [capture]() { QTimer::singleShot(0, capture); }, Qt::SingleShotConnection);
        QTimer::singleShot(5000, window, capture);
    }
}
