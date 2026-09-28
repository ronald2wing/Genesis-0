// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/MediaBinController.h"

#include <QMetaObject>
#include <QThread>
#include <QUrl>
#include <QVariantMap>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "adapters/engine/ges/media/Filmstrip.h"
#include "adapters/engine/ges/media/Probe.h"
#include "project/Editor.h"
#include "project/Project.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"
#include "render/MediaCache.h"
#include "render/Peaks.h"

namespace ges_adapter = genesis::adapters::engine::ges;
namespace render = genesis::render;
using genesis::project::Command;
using genesis::project::FillSlot;
using genesis::project::MediaKind;
using genesis::project::Project;
using genesis::project::SetMediaPlaceholder;
using genesis::project::UpdateMediaPath;

namespace {

// MediaKind -> the token Item::kindToken carries. The enum itself lives in the
// host model; the controller keeps only the three-way distinction its cache
// work needs.
constexpr int kVideo = 0;
constexpr int kAudio = 1;
constexpr int kImage = 2;

int media_kind_token(MediaKind kind)
{
    switch (kind) {
    case MediaKind::Video:
        return kVideo;
    case MediaKind::Audio:
        return kAudio;
    case MediaKind::Image:
        return kImage;
    }
    return kVideo;
}

// The most waveform buckets a card will draw. A bin card is a few hundred
// pixels wide, so more bars than this would collapse to sub-pixel slivers.
constexpr std::size_t kMaxWaveformBuckets = 40;

// Reads a whole file; empty on failure or an empty file.
std::vector<std::uint8_t> read_file(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return { };
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        return { };
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char *>(bytes.data()), size);
    return in.good() || in.eof() ? bytes : std::vector<std::uint8_t>{ };
}

// The source's size in bytes; 0 when it cannot be statted, which reads as
// "missing" and skips generation (a cache keyed to size 0 would never be
// fresh against a real file).
std::uintmax_t source_size(const std::filesystem::path &source)
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(source, ec);
    return ec ? 0 : size;
}

// QML hands the recovery invokables a file:// URL (FileDialog's selectedFile);
// the commands take a plain absolute path. A non-URL string passes through, so
// a caller that already has a path need not wrap it.
std::string local_path(const QString &value)
{
    const QUrl url(value);
    return url.isLocalFile() ? url.toLocalFile().toStdString() : value.toStdString();
}

// The two cache fillers, run on the worker thread. Each checks freshness
// first (cache_is_fresh) so a re-run costs a stat, not a re-encode, then calls
// the blocking GES sampler and records the key on success. A missing source
// or a failed sample leaves nothing behind: the card keeps showing its
// placeholder until the next run.

void ensure_poster(const std::filesystem::path &source, const std::filesystem::path &destination)
{
    const std::uintmax_t size = source_size(source);
    if (size == 0) {
        return;
    }
    render::CacheKey key;
    key.source = source;
    key.source_size = size;
    key.at = 0.0;
    if (render::cache_is_fresh(destination, key)) {
        return;
    }
    if (ges_adapter::make_poster(source, destination, 0.0)) {
        render::cache_record(destination, key);
    }
}

void ensure_filmstrip(const std::filesystem::path &source, const std::filesystem::path &destination)
{
    const std::uintmax_t size = source_size(source);
    if (size == 0) {
        return;
    }
    render::CacheKey key;
    key.source = source;
    key.source_size = size;
    key.columns = 8;
    key.tile_width = 160;
    key.start = 0.0;
    key.duration = 0.0;
    if (render::cache_is_fresh(destination, key)) {
        return;
    }
    ges_adapter::FilmstripRequest request;
    request.source = source;
    request.destination = destination;
    if (ges_adapter::make_filmstrip(request)) {
        render::cache_record(destination, key);
    }
}

} // namespace

MediaBinController::MediaBinController(QObject *parent) : QObject(parent) { }

MediaBinController::~MediaBinController()
{
    // A controller destroyed mid-run must not free its members under the
    // worker: ask it to stop, then join. The sampler is bounded by its own
    // per-frame timeouts, so the join cannot block forever.
    cancelRequested_.store(true);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void MediaBinController::setSearchText(const QString &value)
{
    if (searchText_ == value) {
        return;
    }
    searchText_ = value;
    emit searchTextChanged();
}

void MediaBinController::setSortMode(int value)
{
    if (sortMode_ == value) {
        return;
    }
    sortMode_ = value;
    emit sortModeChanged();
}

void MediaBinController::setCacheRoot(const QString &value)
{
    if (cacheRoot_ == value) {
        return;
    }
    cacheRoot_ = value;
    emit cacheRootChanged();
}

void MediaBinController::setItems(const Project &project)
{
    const std::filesystem::path root = cacheRoot_.isEmpty()
            ? std::filesystem::path()
            : std::filesystem::path(cacheRoot_.toStdString());
    std::map<std::string, Item> next;
    for (const genesis::project::MediaItem &media : project.media) {
        Item item;
        item.id = media.id;
        item.source = media.path;
        item.kindToken = media_kind_token(media.kind);
        if (!root.empty()) {
            const render::CachePaths paths =
                    render::cache_paths(root, std::filesystem::path(media.path));
            item.poster = paths.poster;
            item.filmstrip = paths.filmstrip;
            item.waveform = paths.waveform;
            // Only video needs a sampled poster/filmstrip. An image card draws
            // the source itself; an audio card draws its .peaks waveform.
            item.want_poster = media.kind == MediaKind::Video;
            item.want_filmstrip = media.kind == MediaKind::Video;
        }
        next.emplace(item.id, std::move(item));
    }
    items_ = std::move(next);
}

QVariantList MediaBinController::apply(const QVariantList &items, const QString &query,
                                       int sortMode) const
{
    const QString needle = query.trimmed().toLower();
    QVariantList out;
    out.reserve(items.size());
    for (const QVariant &value : items) {
        const QVariantMap item = value.toMap();
        if (!needle.isEmpty()) {
            const QString name = item.value(QStringLiteral("name")).toString().toLower();
            const QString path = item.value(QStringLiteral("path")).toString().toLower();
            if (!name.contains(needle) && !path.contains(needle)) {
                continue;
            }
        }
        out.append(item);
    }
    if (sortMode == 0) {
        std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap()
                           .value(QStringLiteral("name"))
                           .toString()
                           .compare(b.toMap().value(QStringLiteral("name")).toString(),
                                    Qt::CaseInsensitive)
                    < 0;
        });
    } else if (sortMode == 1) {
        // Longest first; a media item without a probed duration (-1) sinks to
        // the bottom rather than reading as longer than everything.
        std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value(QStringLiteral("duration")).toDouble()
                    > b.toMap().value(QStringLiteral("duration")).toDouble();
        });
    }
    // sortMode 2 keeps import order: the projection is already in it.
    return out;
}

void MediaBinController::select(const QString &mediaId)
{
    if (selectedMediaId_ == mediaId) {
        return;
    }
    selectedMediaId_ = mediaId;
    emit selectionChanged();
}

void MediaBinController::clearSelection()
{
    if (selectedMediaId_.isEmpty()) {
        return;
    }
    selectedMediaId_.clear();
    emit selectionChanged();
}

void MediaBinController::setEditor(genesis::project::Editor *editor)
{
    editor_ = editor;
}

bool MediaBinController::applyMedia(const genesis::project::Command &command)
{
    if (!editor_) {
        return false;
    }
    const auto result = editor_->apply(command);
    if (!result || !result->applied) {
        return false;
    }
    emit historyChanged();
    emit projectEdited();
    return true;
}

bool MediaBinController::relinkMedia(const QString &mediaId, const QString &newPath)
{
    if (mediaId.isEmpty() || newPath.isEmpty()) {
        return false;
    }
    UpdateMediaPath update;
    update.media_id = mediaId.toStdString();
    update.new_path = local_path(newPath);
    return applyMedia(Command{ std::move(update) });
}

bool MediaBinController::fillSlot(const QString &mediaId, const QString &filePath)
{
    if (!editor_ || mediaId.isEmpty() || filePath.isEmpty()) {
        return false;
    }
    const auto probed = ges_adapter::probe(std::filesystem::path(local_path(filePath)));
    if (!probed) {
        return false;
    }
    FillSlot fill;
    fill.media_id = mediaId.toStdString();
    fill.item = *probed;
    return applyMedia(Command{ std::move(fill) });
}

bool MediaBinController::setMediaPlaceholder(const QString &mediaId, bool placeholder)
{
    if (mediaId.isEmpty()) {
        return false;
    }
    SetMediaPlaceholder set;
    set.media_id = mediaId.toStdString();
    set.placeholder = placeholder;
    return applyMedia(Command{ std::move(set) });
}

bool MediaBinController::isPlaceholder(const QString &mediaId) const
{
    if (!editor_ || mediaId.isEmpty()) {
        return false;
    }
    const auto &media = editor_->project().media;
    const auto it = std::find_if(media.begin(), media.end(),
                                 [&mediaId](const genesis::project::MediaItem &item) {
                                     return item.id == mediaId.toStdString();
                                 });
    return it != media.end() && it->placeholder;
}

QString MediaBinController::posterSource(const QString &mediaId, int version) const
{
    (void)version; // named so bindings re-run on cacheUpdated
    const auto it = items_.find(mediaId.toStdString());
    if (it == items_.end()) {
        return QString();
    }
    const Item &item = it->second;
    if (item.kindToken == kImage) {
        return QUrl::fromLocalFile(QString::fromStdString(item.source)).toString();
    }
    if (item.kindToken == kVideo) {
        std::error_code ec;
        if (std::filesystem::exists(item.poster, ec)) {
            return QUrl::fromLocalFile(QString::fromStdString(item.poster.string())).toString();
        }
    }
    return QString();
}

QVariantList MediaBinController::waveformPeaks(const QString &mediaId, int version) const
{
    (void)version;
    QVariantList out;
    const auto it = items_.find(mediaId.toStdString());
    if (it == items_.end() || it->second.kindToken != kAudio) {
        return out;
    }
    std::error_code ec;
    if (!std::filesystem::exists(it->second.waveform, ec)) {
        return out;
    }
    const std::vector<std::uint8_t> bytes = read_file(it->second.waveform);
    if (bytes.empty()) {
        return out;
    }
    const std::optional<render::Peaks> peaks = render::Peaks::decode(bytes);
    if (!peaks || peaks->is_empty()) {
        return out;
    }
    const std::size_t count = peaks->len();
    const std::size_t factor =
            std::max<std::size_t>(1, (count + kMaxWaveformBuckets - 1) / kMaxWaveformBuckets);
    const render::Peaks coarse = peaks->coarser(factor);
    out.reserve(static_cast<qsizetype>(coarse.len()));
    for (std::size_t i = 0; i < coarse.len(); ++i) {
        QVariantMap bucket;
        bucket.insert(QStringLiteral("min"), coarse.min[i]);
        bucket.insert(QStringLiteral("max"), coarse.max[i]);
        out.append(bucket);
    }
    return out;
}

void MediaBinController::generateWaveform(const QString &mediaId)
{
    (void)mediaId;
    // Out of scope: no audio-decode adapter exists in the host to produce a
    // .peaks cache. See the header comment.
}

void MediaBinController::generateMissingCaches()
{
    if (generating_) {
        return;
    }
    int jobs = 0;
    for (const auto &[id, item] : items_) {
        (void)id;
        if (item.want_poster) {
            ++jobs;
        }
        if (item.want_filmstrip) {
            ++jobs;
        }
    }
    if (jobs == 0) {
        return;
    }

    // Snapshot the items by value, so the worker never reads a controller
    // member that setItems could replace on the UI thread mid-run.
    std::vector<Item> snapshot;
    snapshot.reserve(items_.size());
    for (const auto &[id, item] : items_) {
        (void)id;
        snapshot.push_back(item);
    }

    cancelRequested_.store(false);
    workerThreadId_.store(0);
    pendingDone_.store(0);
    progressQueued_.store(false);
    total_ = jobs;
    progress_ = 0;
    emit totalChanged();
    emit progressChanged();
    generating_ = true;
    emit generatingChanged();

    workerThread_ = std::thread([this, snapshot = std::move(snapshot)]() {
        workerThreadId_.store(reinterpret_cast<quintptr>(QThread::currentThread()));
        int done = 0;
        for (const Item &item : snapshot) {
            if (cancelRequested_.load(std::memory_order_relaxed)) {
                break;
            }
            if (item.want_poster) {
                ensure_poster(item.source, item.poster);
                reportProgress(++done, total_);
            }
            if (cancelRequested_.load(std::memory_order_relaxed)) {
                break;
            }
            if (item.want_filmstrip) {
                ensure_filmstrip(item.source, item.filmstrip);
                reportProgress(++done, total_);
            }
        }
        QMetaObject::invokeMethod(this, [this]() { finishGeneration(); }, Qt::QueuedConnection);
    });
}

void MediaBinController::reportProgress(int done, int total)
{
    (void)total; // total_ is fixed for the run and read on the UI thread
    pendingDone_.store(done, std::memory_order_relaxed);
    if (!progressQueued_.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(
                this,
                [this]() {
                    progressQueued_.store(false, std::memory_order_release);
                    const int done = pendingDone_.load(std::memory_order_relaxed);
                    if (progress_ != done) {
                        progress_ = done;
                        emit progressChanged();
                    }
                },
                Qt::QueuedConnection);
    }
}

void MediaBinController::finishGeneration()
{
    // Runs on the controller's thread. Join the worker - which has already
    // posted this call and is returning - then fold the outcome in. The
    // cacheVersion bump makes every posterSource/waveformPeaks binding re-run.
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    generating_ = false;
    emit generatingChanged();
    ++cacheVersion_;
    emit cacheUpdated();
}
