// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/TtsController.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <QMetaObject>
#include <QString>
#include <QThread>
#include <QUrl>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app/net/QtFetcher.h"
#include "project/Editor.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"

namespace ai = genesis::ai;
namespace jobs = genesis::jobs;
using genesis::core::Rational;

namespace {

// The directory a model tarball unpacks into: <root>/<id>/<version>/unpacked.
// A sibling of the installed tarball, so the two never collide with a
// single-file model's layout.
std::filesystem::path extract_dir(const std::filesystem::path &root, const ai::ModelEntry &entry)
{
    return root / entry.id / entry.version / "unpacked";
}

// Whether `dir` already holds the model files for `kind`. The pinned Kokoro
// variant ships `model.int8.onnx` (a full-precision variant ships
// `model.onnx`); the pinned PocketTts variant ships `lm_flow.int8.onnx`.
bool has_model_file(const std::filesystem::path &dir, ai::tts::ModelKind kind)
{
    std::error_code ec;
    if (kind == ai::tts::ModelKind::Kokoro) {
        return std::filesystem::is_regular_file(dir / "model.int8.onnx", ec)
                || std::filesystem::is_regular_file(dir / "model.onnx", ec);
    }
    return std::filesystem::is_regular_file(dir / "lm_flow.int8.onnx", ec);
}

// The default extractor: unpacks a .tar.bz2 into `dest_dir` by execing the
// system `tar`. The tarball's single top-level directory is stripped so the
// model files land directly in `dest_dir`. Extraction is a host concern, so it
// stays here rather than in the engine-free provider; a test injects a fake
// instead, so no tar (or tarball) runs in CI.
ai::tts::ExtractResult extract_tar_bz2(const std::filesystem::path &tarball,
                                       const std::filesystem::path &dest_dir)
{
    ai::tts::ExtractResult result;
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        result.error = "cannot create the extraction directory";
        return result;
    }

    const std::string tarball_str = tarball.string();
    const std::string dest_str = dest_dir.string();
    const std::string strip = "--strip-components=1";

    const pid_t pid = fork();
    if (pid < 0) {
        result.error = "cannot start the extractor";
        return result;
    }
    if (pid == 0) {
        // --no-same-owner stops tar from chowning extracted files to the
        // archive's recorded uid/gid (root on many model tarballs). GNU tar
        // strips a leading '/' from entry paths by default, so no
        // --absolute-names is passed: the archive is trusted (digest-verified
        // by ai::install before this runs) but is never allowed to write
        // absolute paths.
        const char *argv[] = { "tar",
                               "-xjf",
                               tarball_str.c_str(),
                               "-C",
                               dest_str.c_str(),
                               strip.c_str(),
                               "--no-same-owner",
                               nullptr };
        execvp("tar", const_cast<char *const *>(argv));
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        result.error = "the extractor did not finish";
        return result;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        result.error = "the model archive could not be unpacked";
        return result;
    }

    result.ok = true;
    result.dir = dest_dir;
    return result;
}

// A byte count as a short human string ("132 MB"), for the consent prompt.
QString human_size(std::uintmax_t bytes)
{
    const double value = static_cast<double>(bytes);
    if (bytes >= 1024 * 1024 * 1024) {
        return QStringLiteral("%1 GB").arg(value / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(value / (1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024) {
        return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

// QML hands cloneVoice() a file:// URL (FileDialog's selectedFile); the copy
// needs a plain absolute path. A non-URL string passes through, so a caller
// that already has a path need not wrap it.
std::string local_path(const QString &value)
{
    const QUrl url(value);
    return url.isLocalFile() ? url.toLocalFile().toStdString() : value.toStdString();
}

} // namespace

TtsController::TtsController(genesis::project::Editor *editor, QObject *parent)
    : QObject(parent), editor_(editor)
{
    // The two pinned speech entries: Kokoro for the multi-speaker voices and
    // PocketTts for zero-shot voice cloning. Resolved once so the consent
    // prompt and every run agree on the model.
    for (const ai::ModelEntry &row : ai::model_catalogue()) {
        if (row.id == "kokoro") {
            model_entry_ = row;
        } else if (row.id == "pocket-tts") {
            pocket_entry_ = row;
        }
    }
}

TtsController::~TtsController()
{
    // A controller destroyed mid-run must not free its members under the
    // worker: ask it to stop, then join. Cancelling the scheduler job is a
    // no-op before the first run (`runId_` is 0 and never minted).
    cancelRequested_.store(true);
    scheduler_.cancel(runId_);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    if (speakersThread_.joinable()) {
        speakersThread_.join();
    }
}

void TtsController::setText(const QString &text)
{
    if (text_ == text) {
        return;
    }
    text_ = text;
    emit textChanged();
}

void TtsController::setVoice(int voice)
{
    if (voice_ == voice) {
        return;
    }
    voice_ = voice;
    emit voiceChanged();
}

void TtsController::setSpeed(double speed)
{
    if (speed_ == speed) {
        return;
    }
    speed_ = speed;
    emit speedChanged();
}

void TtsController::setVoiceRoot(const std::filesystem::path &root)
{
    voice_root_ = root;
    // Re-list the captured voices from the directory, so a restart shows the
    // voices the user already cloned.
    clonedVoices_.clear();
    std::error_code ec;
    if (!voice_root_.empty() && std::filesystem::is_directory(voice_root_, ec)) {
        for (const std::filesystem::directory_entry &item :
             std::filesystem::directory_iterator(voice_root_, ec)) {
            if (ec) {
                break;
            }
            if (item.path().extension() == ".wav") {
                clonedVoices_.append(QString::fromStdString(item.path().stem().string()));
            }
        }
    }
    if (!clonedVoices_.contains(clonedVoice_)) {
        clonedVoice_.clear();
        emit clonedVoiceChanged();
    }
    emit clonedVoicesChanged();
}

void TtsController::refreshSpeakers()
{
    if (!available()) {
        return;
    }
    if (speakersThread_.joinable()) {
        return; // one refresh at a time
    }
    // Loading the Kokoro graph is expensive, so it runs off the UI thread; the
    // count is posted back and parked on the controller's thread.
    speakersThread_ = std::thread(
            [this, root = model_root_, entry = model_entry_, counter = speakerCounter_]() {
                std::int32_t count = 0;
                if (entry && !root.empty()) {
                    const std::filesystem::path dest = extract_dir(root, *entry);
                    if (has_model_file(dest, ai::tts::ModelKind::Kokoro)) {
                        const SpeakerCounter count_fn =
                                counter ? counter : SpeakerCounter{ [](std::string_view dir) {
                                    return ai::tts::SherpaTtsProvider{ }.num_speakers(dir);
                                } };
                        count = count_fn(dest.string());
                    }
                }
                QMetaObject::invokeMethod(
                        this,
                        [this, count]() {
                            if (speakerCount_ == count) {
                                return;
                            }
                            speakerCount_ = count;
                            emit speakerCountChanged();
                        },
                        Qt::QueuedConnection);
            });
}

void TtsController::cloneVoice(const QString &source_url)
{
    if (!available()) {
        refuse(QStringLiteral("disabled: sherpa-onnx TTS runtime not compiled in "
                              "(rebuild with GENESIS_AI_TTS=ON)"));
        return;
    }
    const std::string source = local_path(source_url);
    std::error_code ec;
    if (source.empty() || !std::filesystem::is_regular_file(source, ec)) {
        refuse(QStringLiteral("Choose a WAV recording to clone."));
        return;
    }
    if (voice_root_.empty()) {
        refuse(QStringLiteral("No voice directory is configured."));
        return;
    }
    std::filesystem::create_directories(voice_root_, ec);
    if (ec) {
        refuse(QStringLiteral("Cannot create the voice directory."));
        return;
    }

    // Name from the source stem, made unique against the captured voices so a
    // re-clone of the same recording keeps both.
    std::string stem = std::filesystem::path(source).stem().string();
    if (stem.empty()) {
        stem = "voice";
    }
    QString name = QString::fromStdString(stem);
    int suffix = 2;
    while (clonedVoices_.contains(name)) {
        name = QString::fromStdString(stem) + "-" + QString::number(suffix++);
    }

    const std::filesystem::path dest = voice_root_ / (name.toStdString() + ".wav");
    if (!std::filesystem::copy_file(source, dest, ec) || ec) {
        refuse(QStringLiteral("Could not copy the reference recording."));
        return;
    }

    clonedVoices_.append(name);
    clonedVoice_ = name;
    emit clonedVoicesChanged();
    emit clonedVoiceChanged();
}

void TtsController::removeClonedVoice(const QString &name)
{
    const int index = clonedVoices_.indexOf(name);
    if (index < 0) {
        return;
    }
    std::error_code ec;
    if (!voice_root_.empty()) {
        std::filesystem::remove(voice_root_ / (name.toStdString() + ".wav"), ec);
    }
    clonedVoices_.removeAt(index);
    if (clonedVoice_ == name) {
        clonedVoice_.clear();
        emit clonedVoiceChanged();
    }
    emit clonedVoicesChanged();
}

void TtsController::selectClonedVoice(const QString &name)
{
    if (clonedVoice_ == name) {
        return;
    }
    clonedVoice_ = name;
    emit clonedVoiceChanged();
}

void TtsController::refuse(const QString &reason)
{
    setError(reason);
    setState(QStringLiteral("Failed"));
    emit finished(false, reason);
}

bool TtsController::modelReady(const ai::ModelEntry &entry, ai::tts::ModelKind kind) const
{
    if (model_root_.empty()) {
        return false;
    }
    // A cheap existence check (not the full digest verify): the authoritative
    // SHA-256 check runs inside ai::install on the worker, off the UI thread.
    std::error_code ec;
    const bool tarball = std::filesystem::is_regular_file(ai::model_path(model_root_, entry), ec);
    return tarball && has_model_file(extract_dir(model_root_, entry), kind);
}

void TtsController::speak()
{
    if (running_) {
        return;
    }
    if (!available()) {
        refuse(QStringLiteral("disabled: sherpa-onnx TTS runtime not compiled in "
                              "(rebuild with GENESIS_AI_TTS=ON)"));
        return;
    }
    const QString trimmed = text_.trimmed();
    if (trimmed.isEmpty()) {
        refuse(QStringLiteral("Type something to speak first."));
        return;
    }

    // Resolve the voice: a selected cloned voice runs the PocketTts model
    // against its captured reference WAV; otherwise the Kokoro model speaks
    // with the chosen speaker id.
    const ai::ModelEntry *entry = nullptr;
    ai::tts::ModelKind kind = ai::tts::ModelKind::Kokoro;
    std::string reference_wav;
    if (!clonedVoice_.isEmpty()) {
        if (!pocket_entry_) {
            refuse(QStringLiteral("No voice-cloning model is available."));
            return;
        }
        entry = &*pocket_entry_;
        kind = ai::tts::ModelKind::PocketTts;
        reference_wav = (voice_root_ / (clonedVoice_.toStdString() + ".wav")).string();
        if (!std::filesystem::is_regular_file(reference_wav)) {
            refuse(QStringLiteral("The cloned voice recording is missing."));
            return;
        }
    } else {
        if (!model_entry_) {
            refuse(QStringLiteral("No speech model is available."));
            return;
        }
        entry = &*model_entry_;
    }

    // Fix the run's parameters now, so the worker and the apply read the same
    // text, voice and tunables even if the user types more mid-run. The WAV
    // name carries a session counter, so consecutive narrations never collide.
    runEntry_ = *entry;
    runKind_ = kind;
    runReferenceWav_ = std::move(reference_wav);
    runText_ = trimmed;
    runVoice_ = voice_;
    runSpeed_ = speed_ > 0.0 ? speed_ : 1.0;

    const std::filesystem::path parent =
            output_root_.empty() ? std::filesystem::temp_directory_path() : output_root_;
    runWavPath_ = parent / ("speech-" + std::to_string(++speech_counter_) + ".wav");

    if (!modelReady(runEntry_, runKind_)) {
        // First run: the model is not on disk, so ask before downloading it.
        consentName_ = QString::fromStdString(runEntry_.id + " " + runEntry_.version);
        consentSize_ = human_size(runEntry_.size);
        consentLicense_ = QString::fromStdString(runEntry_.license);
        consentUrl_ = QString::fromStdString(runEntry_.url);
        setError(QString());
        setState(QStringLiteral("Consent"));
        emit consentChanged();
        return;
    }
    startRun();
}

void TtsController::confirmDownload()
{
    if (state_ != QStringLiteral("Consent")) {
        return;
    }
    startRun();
}

void TtsController::declineDownload()
{
    if (state_ != QStringLiteral("Consent")) {
        return;
    }
    consentName_.clear();
    consentSize_.clear();
    consentLicense_.clear();
    consentUrl_.clear();
    setState(QStringLiteral("Idle"));
    emit consentChanged();
}

void TtsController::cancel()
{
    if (!running_) {
        return;
    }
    // Flag the job on the scheduler (cancel wins over the result when the
    // worker completes) and set the atomic the worker polls. `running` stays
    // true until finishSpeech lands, so the pane keeps showing progress while
    // the worker confirms the stop.
    scheduler_.cancel(runId_);
    cancelRequested_.store(true, std::memory_order_relaxed);
}

void TtsController::startRun()
{
    // Queue the run on the scheduler, then start it. With max_concurrent = 1
    // and the `running_` guard holding off a second submit, this is the one
    // Running job. The scheduler lives on this (the controller's) thread; the
    // worker below never touches it, only the queued handlers do.
    runId_ = scheduler_.submit(jobs::JobRequest{
            jobs::SynthesizeSpeechRequest{ runEntry_.id, runVoice_, runText_.toStdString(),
                                           runSpeed_, runWavPath_.parent_path().string() } });
    scheduler_.acquire();

    cancelRequested_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        pendingFraction_ = 0.0;
        pendingStage_.clear();
    }
    progressQueued_.store(false);
    setProgress(0.0);
    setStage(QString());
    setError(QString());
    setState(QStringLiteral("Running"));
    setRunning(true);

    // Downloading, unpacking and synthesizing block, so they must not run on
    // the UI thread. The worker owns the resolved parameters and the seams by
    // value and reaches back into the controller only through the cancellation
    // flag and queued invokes, so no controller member is touched off-thread.
    // The thread is joined in finishSpeech, so it is never reassigned while
    // still running. The default fetcher/extractor/synthesizer are built here,
    // on the worker, so a QNetworkAccessManager is created on the thread that
    // uses it.
    workerThread_ = std::thread([this, root = model_root_, entry = runEntry_, kind = runKind_,
                                 reference_wav = runReferenceWav_, text = runText_.toStdString(),
                                 voice = runVoice_, speed = runSpeed_, wav_path = runWavPath_,
                                 installer = installer_, extractor = extractor_,
                                 synthesizer = synthesizer_]() {
        const ai::Fetcher fetch = ai::Fetcher{ QtFetcher(&cancelRequested_) };

        const Installer install = installer
                ? installer
                : Installer{ [](const std::filesystem::path &model_root,
                                const ai::ModelEntry &model_entry, const ai::Fetcher &fetch,
                                const ai::Progress &progress) {
                      return ai::install(model_root, model_entry, fetch, progress);
                  } };
        const Extractor extract = extractor ? extractor : Extractor{ &extract_tar_bz2 };
        const Synthesizer synthesize = synthesizer
                ? synthesizer
                : Synthesizer{ [](std::string_view model_dir, ai::tts::ModelKind model_kind,
                                  std::string_view spoken,
                                  const ai::tts::SynthesizeOptions &options, std::string_view out,
                                  const ai::tts::ProgressFn &progress,
                                  const ai::tts::AbortFn &abort) {
                      return ai::tts::SherpaTtsProvider{ }.synthesize(
                              model_dir, model_kind, spoken, options, out, progress, abort);
                  } };

        // Stage 1: install the model (downloading it when absent). The
        // digest verify runs inside ai::install, off the UI thread.
        const ai::InstallResult installed =
                install(root, entry, fetch, [this](std::uintmax_t bytes, std::uintmax_t total) {
                    const double fraction = total > 0
                            ? std::clamp(static_cast<double>(bytes) / static_cast<double>(total),
                                         0.0, 1.0)
                            : 0.0;
                    reportProgress(0.4 * fraction, QStringLiteral("downloading model"));
                });
        if (!installed.ok()) {
            const bool cancelled = cancelRequested_.load(std::memory_order_relaxed);
            const std::string reason = installed.problems.empty() ? "the model download failed"
                                                                  : installed.problems.front();
            postFinish(false, cancelled ? "cancelled" : reason, { }, 0.0, 0);
            return;
        }

        // Stage 2: unpack the tarball into its directory, reusing an
        // already-unpacked copy so a re-run does not re-extract.
        const std::filesystem::path dest = extract_dir(root, entry);
        if (!has_model_file(dest, kind)) {
            reportProgress(0.45, QStringLiteral("extracting model"));
            const ai::tts::ExtractResult extracted = extract(installed.installed.value(), dest);
            if (!extracted.ok) {
                const bool cancelled = cancelRequested_.load(std::memory_order_relaxed);
                postFinish(false, cancelled ? "cancelled" : extracted.error, { }, 0.0, 0);
                return;
            }
        }

        // Stage 3: synthesize the text into the WAV. The cloned-voice path
        // carries the reference recording through the options; the Kokoro
        // path carries the speaker id.
        ai::tts::SynthesizeOptions options;
        options.speed = static_cast<float>(speed);
        if (kind == ai::tts::ModelKind::PocketTts) {
            options.reference_wav = reference_wav;
        } else {
            options.voice = voice;
        }
        const ai::tts::SynthesizeOutcome outcome = synthesize(
                dest.string(), kind, text, options, wav_path.string(),
                [this](double fraction, std::string_view stage) {
                    reportProgress(
                            0.5 + 0.5 * std::clamp(fraction, 0.0, 1.0),
                            QString::fromUtf8(stage.data(), static_cast<qsizetype>(stage.size())));
                },
                [this]() { return cancelRequested_.load(std::memory_order_relaxed); });

        if (!outcome.ok) {
            const bool cancelled = cancelRequested_.load(std::memory_order_relaxed)
                    || outcome.error == "cancelled";
            postFinish(false, cancelled ? "cancelled" : outcome.error, { }, 0.0, 0);
            return;
        }
        postFinish(true, { }, outcome.wav_path, outcome.duration, outcome.sample_rate);
    });
}

void TtsController::reportProgress(double fraction, const QString &stage)
{
    // Runs on the worker thread. Park the latest fraction and stage, then post
    // at most one queued update: if a handoff is already in flight, the next
    // report only overwrites the parked values and queues nothing, so a run
    // that reports faster than the UI paints never piles up one event per
    // report.
    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        pendingFraction_ = fraction;
        pendingStage_ = stage;
    }
    if (!progressQueued_.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(
                this,
                [this]() {
                    progressQueued_.store(false, std::memory_order_release);
                    double fraction = 0.0;
                    QString stage;
                    {
                        std::lock_guard<std::mutex> lock(progressMutex_);
                        fraction = pendingFraction_;
                        stage = pendingStage_;
                    }
                    scheduler_.report_progress(runId_,
                                               jobs::Progress{ fraction, stage.toStdString() });
                    setProgress(fraction);
                    setStage(stage);
                },
                Qt::QueuedConnection);
    }
}

void TtsController::postFinish(bool ok, const std::string &error, const std::string &wav_path,
                               double duration, std::uint32_t sample_rate)
{
    // The worker is returning; hand the outcome to the controller's thread so
    // the scheduler transition and the apply stay on the one thread that owns
    // them.
    QMetaObject::invokeMethod(
            this,
            [this, ok, error, wav_path, duration, sample_rate]() {
                finishSpeech(ok, QString::fromStdString(error), wav_path, duration, sample_rate);
            },
            Qt::QueuedConnection);
}

void TtsController::finishSpeech(bool ok, const QString &error, const std::string &wav_path,
                                 double duration, std::uint32_t sample_rate)
{
    // Runs on the controller's thread. Drive the scheduler to its terminal
    // state, fold the outcome into the properties and emit the signals, then
    // join the worker - which has already posted this call and is returning -
    // so the next run can safely reuse workerThread_.
    bool final_ok = ok;
    QString final_error = error;

    if (ok) {
        QString refusal;
        if (applySpeech(wav_path, duration, sample_rate, refusal)) {
            scheduler_.complete(runId_,
                                jobs::JobResult{ jobs::SpeechResult{ wav_path, duration } });
            setProgress(1.0);
            setState(QStringLiteral("Done"));
            setError(QString());
        } else {
            final_ok = false;
            final_error = refusal;
            scheduler_.fail(runId_, refusal.toStdString());
            setState(QStringLiteral("Failed"));
            setError(refusal);
        }
    } else if (final_error == QStringLiteral("cancelled")) {
        // cancel() already flagged the job, so completing it now lands it
        // Cancelled (cancel wins, the result is discarded).
        scheduler_.complete(runId_, jobs::JobResult{ });
        setState(QStringLiteral("Cancelled"));
        setError(QStringLiteral("cancelled"));
    } else {
        scheduler_.fail(runId_, final_error.toStdString());
        setState(QStringLiteral("Failed"));
        setError(final_error);
    }

    setRunning(false);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    emit finished(final_ok, final_error);
}

bool TtsController::applySpeech(const std::string &wav_path, double duration,
                                std::uint32_t sample_rate, QString &refusal)
{
    // The narration becomes NewMedia (audio, generated by speech) and one
    // AddClipAtFirstFree at the timeline start, wrapped in a single Batch so
    // the whole import is one undo step. The media id is minted by peek_id
    // before the batch applies, so the two commands agree without the scan
    // drifting off the editor's real counter.
    genesis::project::NewMedia item;
    item.path = wav_path;
    item.name = std::filesystem::path(wav_path).filename().string();
    if (duration > 0.0) {
        if (const auto rational = Rational::approximate(duration)) {
            item.duration = *rational;
        }
    }
    item.kind = genesis::project::MediaKind::Audio;
    item.has_audio = true;
    item.audio_codec = "pcm_s16le";
    genesis::project::AudioTrack track;
    track.index = 0;
    track.codec = "pcm_s16le";
    track.channels = 1;
    track.sample_rate = sample_rate;
    item.audio_tracks.push_back(std::move(track));
    item.origin = genesis::project::MediaOrigin::Speech;

    if (editor_ == nullptr) {
        refusal = QStringLiteral("no project");
        return false;
    }
    const std::string media_id = editor_->peek_id("m");
    genesis::project::Batch batch;
    batch.commands.push_back(
            genesis::project::Command{ genesis::project::AddMedia{ std::move(item) } });
    batch.commands.push_back(genesis::project::Command{
            genesis::project::AddClipAtFirstFree{ media_id, Rational::ZERO } });
    const auto result = editor_->apply(genesis::project::Command{ std::move(batch) });
    if (!result) {
        refusal = QString::fromStdString(std::string(result.error().message()));
        return false;
    }
    // A tolerated no-op (nothing changed) still succeeds: no undo step, no
    // failure. A real apply notifies the shell to refresh and the toolbar to
    // re-read its undo/redo/dirty state.
    emit historyChanged();
    emit projectEdited();
    return true;
}

void TtsController::setState(const QString &value)
{
    if (state_ == value) {
        return;
    }
    state_ = value;
    emit stateChanged();
}

void TtsController::setRunning(bool value)
{
    if (running_ == value) {
        return;
    }
    running_ = value;
    emit runningChanged();
}

void TtsController::setProgress(double value)
{
    if (progress_ == value) {
        return;
    }
    progress_ = value;
    emit progressChanged();
}

void TtsController::setStage(const QString &value)
{
    if (stage_ == value) {
        return;
    }
    stage_ = value;
    emit stageChanged();
}

void TtsController::setError(const QString &value)
{
    if (error_ == value) {
        return;
    }
    error_ = value;
    emit errorChanged();
}
