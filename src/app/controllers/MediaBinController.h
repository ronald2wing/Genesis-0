// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's media bin controller: the view state for the bin pane
// (search, sort, selection) plus the off-thread cache generation that turns
// each imported file into the poster and filmstrip the cards draw. It owns no
// media and no engine session - only the mapping from a media id to its cache
// paths, the worker thread that fills those paths (R4), and the marshalling of
// progress/completion back to the controller's thread. The QML pane binds to
// the properties below and calls the Q_INVOKABLEs; the controller itself never
// touches QML.
//
// Two data sources feed one card: the media metadata (name, duration, kind,
// missing flag) comes from the TimelineModel projection, and the cache
// lookups (posterSource, waveformPeaks) come from here. apply() is the pure
// filter/sort over the projection, so it stays testable without a project.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <atomic>
#include <filesystem>
#include <map>
#include <string>
#include <thread>

namespace genesis::project {
class Editor;
struct Command;
struct Project;
} // namespace genesis::project

class MediaBinController : public QObject
{
    Q_OBJECT

    // The search box's text. Written from QML; read by apply()'s callers.
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    // Sort order: 0 = name, 1 = duration, 2 = import order.
    Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    // The selected bin item's id, "" for none.
    Q_PROPERTY(QString selectedMediaId READ selectedMediaId NOTIFY selectionChanged)
    // True while a worker fills caches. Gates the Generate button.
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)
    // Cache jobs done and the total for the current run.
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int total READ total NOTIFY totalChanged)
    // Bumped once per completed run, so bindings that read a poster or
    // waveform re-run when the worker fills it in.
    Q_PROPERTY(int cacheVersion READ cacheVersion NOTIFY cacheUpdated)
    // Where caches live; set by the host before setItems.
    Q_PROPERTY(QString cacheRoot READ cacheRoot WRITE setCacheRoot NOTIFY cacheRootChanged)

public:
    explicit MediaBinController(QObject *parent = nullptr);
    ~MediaBinController() override;

    QString searchText() const { return searchText_; }
    int sortMode() const { return sortMode_; }
    QString selectedMediaId() const { return selectedMediaId_; }
    bool generating() const { return generating_; }
    int progress() const { return progress_; }
    int total() const { return total_; }
    int cacheVersion() const { return cacheVersion_; }
    QString cacheRoot() const { return cacheRoot_; }

    void setSearchText(const QString &value);
    void setSortMode(int value);
    void setCacheRoot(const QString &value);

    // Rebuilds the id -> cache-path map from the host project. The bin's
    // metadata is projected by TimelineModel; this keeps only what the cache
    // lookups need (source, kind, and the three cache paths).
    void setItems(const genesis::project::Project &project);

    // The bin's cards, filtered and sorted. `items` is the TimelineModel
    // media projection; the query matches name or path (case-insensitive),
    // and sortMode picks name / duration / import order. Pure: no project
    // state is read, so it is testable with a hand-built list.
    Q_INVOKABLE QVariantList apply(const QVariantList &items, const QString &query,
                                   int sortMode) const;

    Q_INVOKABLE void select(const QString &mediaId);
    Q_INVOKABLE void clearSelection();

    // The thumbnail source for a card: a file:// URL to the generated poster
    // for video, the source itself for an image, empty for audio or a video
    // whose poster has not been generated yet. `version` is the cacheVersion
    // at call time; ignored here, but named so bindings re-run on cacheUpdated.
    Q_INVOKABLE QString posterSource(const QString &mediaId, int version) const;

    // An audio item's waveform, downsampled to a bounded number of buckets of
    // { min, max }, decoded straight from its .peaks cache. Empty when there
    // is no cache (the card then offers Generate).
    Q_INVOKABLE QVariantList waveformPeaks(const QString &mediaId, int version) const;

    // Documented no-op: no audio-decode adapter exists in the host to produce
    // a .peaks cache, so waveform generation is out of scope for this
    // increment. The card draws an existing cache or a Generate affordance.
    Q_INVOKABLE void generateWaveform(const QString &mediaId);

    // Spawns the worker that fills every missing poster and filmstrip, off
    // the UI thread (R4). No-ops while already generating.
    Q_INVOKABLE void generateMissingCaches();

    // The thread the most recent run's worker used, or 0 before any run. For
    // the tests to prove the generation actually left the controller's thread.
    quintptr workerThreadId() const { return workerThreadId_.load(); }

    // ---- Missing-media recovery, one undo step per command. ----
    // The host Editor the media commands are applied through. The composition
    // root sets it once, next to setItems; the controller never owns it. Null
    // disables the three mutating invokables (they return false), so a
    // cache-only controller stays usable.
    void setEditor(genesis::project::Editor *editor);
    genesis::project::Editor *editor() const { return editor_; }

    // Relinks a bin item to a new file on disk (UpdateMediaPath). `newPath`
    // may be a file:// URL (the FileDialog's selectedFile) or a plain path.
    // False when there is no editor, the id is empty, or the path is unchanged.
    Q_INVOKABLE bool relinkMedia(const QString &mediaId, const QString &newPath);

    // Fills a template slot in place (FillSlot): probes `filePath` and swaps
    // the slot's identity behind its id, keeping every clip that references it.
    // False when there is no editor, the probe fails, the id is unknown, or the
    // item is not a placeholder.
    Q_INVOKABLE bool fillSlot(const QString &mediaId, const QString &filePath);

    // Marks a bin item as a template slot, or back to ordinary media
    // (SetMediaPlaceholder). False when there is no editor, the id is empty, or
    // the flag already holds.
    Q_INVOKABLE bool setMediaPlaceholder(const QString &mediaId, bool placeholder);

    // Whether a bin item is currently a template slot (FillSlot's precondition).
    Q_INVOKABLE bool isPlaceholder(const QString &mediaId) const;

signals:
    void searchTextChanged();
    void sortModeChanged();
    void selectionChanged();
    void generatingChanged();
    void progressChanged();
    void totalChanged();
    void cacheUpdated();
    void cacheRootChanged();
    // Undo/redo/dirty changed, so the toolbar stays in step with a media edit.
    void historyChanged();
    // A media command applied: the shell rebuilds its projection and reloads
    // the engine, exactly as it does for EditController::projectEdited.
    void projectEdited();

private:
    struct Item
    {
        std::string id;
        std::string source;
        // MediaKind token: 0 video, 1 audio, 2 image (the QML bridge's kind
        // is a string; the cache worker only needs these three).
        int kindToken = 0;
        std::filesystem::path poster;
        std::filesystem::path filmstrip;
        std::filesystem::path waveform;
        bool want_poster = false;
        bool want_filmstrip = false;
    };

    // The worker's progress callback (on the worker thread): parks the latest
    // done count and posts at most one queued update, mirroring
    // ExportController::reportProgress's latest-wins discipline.
    void reportProgress(int done, int total);

    // Runs on the controller's thread when the worker finishes: folds the run
    // into the properties, joins the worker, and bumps cacheVersion so the
    // cards re-read their posters and waveforms.
    void finishGeneration();

    // Applies one media command through the editor (one undo step) and
    // notifies; false when there is no editor, it refused, or it was a
    // tolerated no-op.
    bool applyMedia(const genesis::project::Command &command);

    genesis::project::Editor *editor_ = nullptr;

    QString searchText_;
    int sortMode_ = 0;
    QString selectedMediaId_;
    bool generating_ = false;
    int progress_ = 0;
    int total_ = 0;
    int cacheVersion_ = 0;
    QString cacheRoot_;

    // id -> item. Read only on the controller's thread; the worker works on
    // a snapshot taken at start, so setItems can never race it.
    std::map<std::string, Item> items_;

    std::atomic<bool> cancelRequested_{ false };
    std::atomic<quintptr> workerThreadId_{ 0 };
    std::atomic<int> pendingDone_{ 0 };
    std::atomic<bool> progressQueued_{ false };

    std::thread workerThread_;
};
