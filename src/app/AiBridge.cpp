// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The AI bridge: the ai.* JSON-RPC methods over the four controllers. The
// function bodies were extracted from AppShell.cpp to keep the composition
// root lean.

#include "app/AiBridge.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <QString>

#include <nlohmann/json.hpp>

#include "ai/ModelManifest.h"
#include "ai/ModelStore.h"
#include "app/controllers/AiController.h"
#include "app/controllers/CutoutController.h"
#include "app/controllers/EnhanceController.h"
#include "app/controllers/TtsController.h"
#include "app/net/QtFetcher.h"

namespace ai = genesis::ai;

namespace {

// True when `kind` names one of the four AI kinds.
bool valid_ai_kind(std::string_view kind)
{
    return kind == "captions" || kind == "cutout" || kind == "enhance" || kind == "tts";
}

// The catalogue id(s) a kind runs, in install order. Cutout lists both matting
// models plus the smart-brush halves, so `ai.models` and `ai.download` cover
// everything the kind can install.
std::vector<std::string_view> model_ids_for_kind(std::string_view kind)
{
    if (kind == "captions") {
        return { "whisper" };
    }
    if (kind == "cutout") {
        return { "rvm", "isnet", "slimsam-encoder", "slimsam-decoder" };
    }
    if (kind == "enhance") {
        return { "real-esrgan" };
    }
    if (kind == "tts") {
        return { "kokoro", "pocket-tts" };
    }
    return { };
}

// The kind that owns this catalogue id, or null when the id is not one of the
// four kinds' models.
const char *kind_of_model(std::string_view id)
{
    if (id == "whisper") {
        return "captions";
    }
    if (id == "rvm" || id == "isnet" || id == "slimsam-encoder" || id == "slimsam-decoder") {
        return "cutout";
    }
    if (id == "real-esrgan") {
        return "enhance";
    }
    if (id == "kokoro" || id == "pocket-tts") {
        return "tts";
    }
    return nullptr;
}

// The pinned catalogue entry with this id, or null when none matches (the same
// lookup the controllers use, kept file-local so the seam reads one table).
const ai::ModelEntry *find_model(std::string_view id)
{
    for (const ai::ModelEntry &row : ai::model_catalogue()) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

// Whether a model file exists on disk. A cheap existence check, the same the
// controllers use on the UI thread (the authoritative digest verify runs inside
// ai::install on the worker).
bool model_file_present(const std::filesystem::path &root, const ai::ModelEntry &entry)
{
    return std::filesystem::is_regular_file(ai::model_path(root, entry));
}

// A refusing reply: a typed ApiError.
genesis::api::Response ai_fail(genesis::api::ErrorCode code, std::string message)
{
    return genesis::api::Response{ std::unexpected(
            genesis::api::ApiError{ code, std::move(message) }) };
}

// The reply every fire-and-forget ai.* verb returns on acceptance.
genesis::api::Response ai_done()
{
    return genesis::api::Response{ genesis::api::Reply{ genesis::api::Done{ } } };
}

// The shared slot for a kind, created on first download.
std::shared_ptr<AiDownloadSlot> slot_for(const std::shared_ptr<AiDownloadSlots> &ai_slots,
                                         std::string_view kind)
{
    auto it = ai_slots->find(std::string(kind));
    if (it != ai_slots->end()) {
        return it->second;
    }
    const auto slot = std::make_shared<AiDownloadSlot>();
    ai_slots->emplace(std::string(kind), slot);
    return slot;
}

// The slot for a kind, or null when the kind has never been downloaded.
std::shared_ptr<AiDownloadSlot> find_slot(const std::shared_ptr<AiDownloadSlots> &ai_slots,
                                          std::string_view kind)
{
    const auto it = ai_slots->find(std::string(kind));
    return it == ai_slots->end() ? nullptr : it->second;
}

} // anonymous namespace

genesis::api::Response ai_service(const genesis::api::AiRequest &request, AiController *captions,
                                  CutoutController *cutout, EnhanceController *enhance,
                                  TtsController *tts, const std::filesystem::path &model_root,
                                  const std::shared_ptr<AiDownloadSlots> &ai_slots)
{
    namespace api = genesis::api;
    return std::visit(
            [&](const auto &alternative) -> api::Response {
                using T = std::decay_t<decltype(alternative)>;

                if constexpr (std::is_same_v<T, api::AiModels>) {
                    api::AiModelsView view;
                    for (const ai::ModelEntry &row : ai::model_catalogue()) {
                        const char *kind = kind_of_model(row.id);
                        if (kind == nullptr) {
                            continue;
                        }
                        api::AiModelInfo info;
                        info.kind = kind;
                        info.model = row.id;
                        info.installed = model_file_present(model_root, row);
                        info.size_bytes = row.size;
                        info.license = row.license;
                        info.url = row.url;
                        view.models.push_back(std::move(info));
                    }
                    // The tts kind's selectable voices: the Kokoro speakers
                    // (once the runtime has reported its count, else a single
                    // default) plus the captured cloned voices. The picker
                    // needs this metadata because the bridge has no push
                    // channel for the controller's speakerCount/clonedVoices.
                    const int speakers = tts->speakerCount();
                    const int speaker_total = speakers > 1 ? speakers : 1;
                    for (int i = 0; i < speaker_total; ++i) {
                        api::AiVoiceInfo voice;
                        voice.name = speakers > 1 ? "Speaker " + std::to_string(i + 1) : "Default";
                        voice.index = i;
                        voice.cloned = false;
                        view.voices.push_back(std::move(voice));
                    }
                    for (const QString &name : tts->clonedVoices()) {
                        api::AiVoiceInfo voice;
                        voice.name = name.toStdString();
                        voice.index = -1;
                        voice.cloned = true;
                        view.voices.push_back(std::move(voice));
                    }
                    return api::Response{ api::Reply{ std::move(view) } };
                }

                if constexpr (std::is_same_v<T, api::AiDownload>) {
                    if (!valid_ai_kind(alternative.kind)) {
                        return ai_fail(api::ErrorCode::Invalid,
                                       "unknown ai kind '" + alternative.kind + "'");
                    }
                    const std::shared_ptr<AiDownloadSlot> slot =
                            slot_for(ai_slots, alternative.kind);
                    if (slot->active.load(std::memory_order_relaxed)) {
                        return ai_fail(api::ErrorCode::Busy,
                                       "a download for '" + alternative.kind
                                               + "' is already running");
                    }
                    slot->reap();

                    std::vector<ai::ModelEntry> entries;
                    for (const std::string_view id : model_ids_for_kind(alternative.kind)) {
                        if (const ai::ModelEntry *row = find_model(id)) {
                            entries.push_back(*row);
                        }
                    }
                    if (entries.empty()) {
                        return ai_fail(api::ErrorCode::Failed,
                                       "the '" + alternative.kind + "' kind has no models");
                    }

                    // Reset the slot for the new run, then hand the install to
                    // a worker so the calling thread never blocks on a transfer.
                    slot->cancel.store(false, std::memory_order_relaxed);
                    slot->progress.store(0.0, std::memory_order_relaxed);
                    slot->failed.store(false, std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> lock(slot->error_mutex);
                        slot->error.clear();
                    }
                    slot->active.store(true, std::memory_order_relaxed);
                    slot->worker = std::thread([slot, entries = std::move(entries), model_root]() {
                        const ai::Fetcher fetch{ QtFetcher(&slot->cancel) };
                        bool ok = true;
                        for (std::size_t i = 0; i < entries.size(); ++i) {
                            const ai::InstallResult result = ai::install(
                                    model_root, entries[i], fetch,
                                    [slot, i, total = entries.size()](std::uintmax_t bytes,
                                                                      std::uintmax_t expected) {
                                        const double local = expected > 0
                                                ? std::clamp(
                                                          static_cast<double>(bytes)
                                                                  / static_cast<double>(expected),
                                                          0.0, 1.0)
                                                : 0.0;
                                        slot->progress.store((static_cast<double>(i) + local)
                                                                     / static_cast<double>(total),
                                                             std::memory_order_relaxed);
                                    });
                            if (!result.ok()) {
                                ok = false;
                                std::lock_guard<std::mutex> lock(slot->error_mutex);
                                slot->error = result.problems.empty() ? "the model download failed"
                                                                      : result.problems.front();
                                break;
                            }
                        }
                        slot->progress.store(1.0, std::memory_order_relaxed);
                        slot->failed.store(!ok, std::memory_order_relaxed);
                        slot->active.store(false, std::memory_order_relaxed);
                    });
                    return ai_done();
                }

                if constexpr (std::is_same_v<T, api::AiStatus>) {
                    api::AiStatusView view;
                    const auto entry = [&](const std::string &kind, const QString &state,
                                           const QString &stage, double progress,
                                           const QString &error) {
                        api::AiStatusEntry out;
                        out.kind = kind;
                        out.progress = progress;
                        const std::string controller_state =
                                api::ai_status_state(state.toStdString(), stage.toStdString());
                        const std::shared_ptr<AiDownloadSlot> slot = find_slot(ai_slots, kind);
                        if (slot && slot->active.load(std::memory_order_relaxed)) {
                            // A standalone download wins over the controller's
                            // state: it reports "downloading" with the install
                            // progress.
                            out.state = "downloading";
                            out.progress = slot->progress.load(std::memory_order_relaxed);
                        } else if (controller_state == "idle" && slot
                                   && slot->failed.load(std::memory_order_relaxed)) {
                            // A failed download surfaces its problem once the
                            // controller is idle again.
                            out.state = "failed";
                            out.error = slot->last_error();
                        } else {
                            out.state = controller_state;
                            if (out.state == "failed") {
                                out.error = error.toStdString();
                            }
                        }
                        return out;
                    };
                    view.entries.push_back(entry("captions", captions->state(), captions->stage(),
                                                 captions->progress(), captions->error()));
                    view.entries.push_back(entry("cutout", cutout->state(), cutout->stage(),
                                                 cutout->progress(), cutout->error()));
                    view.entries.push_back(entry("enhance", enhance->state(), enhance->stage(),
                                                 enhance->progress(), enhance->error()));
                    view.entries.push_back(entry("tts", tts->state(), tts->stage(), tts->progress(),
                                                 tts->error()));
                    return api::Response{ api::Reply{ std::move(view) } };
                }

                if constexpr (std::is_same_v<T, api::AiRun>) {
                    const std::string &kind = alternative.kind;
                    if (!valid_ai_kind(kind)) {
                        return ai_fail(api::ErrorCode::Invalid, "unknown ai kind '" + kind + "'");
                    }

                    if (kind == "captions") {
                        if (!captions->available()) {
                            return ai_fail(api::ErrorCode::NotAvailable,
                                           captions->statusText().toStdString());
                        }
                        if (!alternative.clip_id || alternative.clip_id->empty()) {
                            return ai_fail(api::ErrorCode::Invalid,
                                           "ai.run captions requires clipId");
                        }
                        if (captions->running()) {
                            return ai_fail(api::ErrorCode::Busy,
                                           "a captions run is already in flight");
                        }
                        if (const ai::ModelEntry *entry = find_model("whisper");
                            entry != nullptr && !model_file_present(model_root, *entry)) {
                            return ai_fail(api::ErrorCode::Failed,
                                           "the whisper model is not installed; call ai.download "
                                           "first");
                        }
                        captions->setClipId(QString::fromStdString(*alternative.clip_id));
                        captions->generateCaptions();
                        return ai_done();
                    }

                    if (kind == "cutout") {
                        if (!cutout->available()) {
                            return ai_fail(api::ErrorCode::NotAvailable,
                                           cutout->statusText().toStdString());
                        }
                        if (!alternative.params.is_object()
                            || !alternative.params.contains("subject")) {
                            return ai_fail(api::ErrorCode::Invalid,
                                           "ai.run cutout requires params.subject");
                        }
                        const nlohmann::json &subject_json = alternative.params["subject"];
                        if (!subject_json.is_string()) {
                            return ai_fail(api::ErrorCode::Invalid,
                                           "ai.run cutout subject must be a string");
                        }
                        const std::string subject = subject_json.get<std::string>();
                        if (subject != "person" && subject != "object") {
                            return ai_fail(
                                    api::ErrorCode::Invalid,
                                    "ai.run cutout subject must be \"person\" or \"object\"");
                        }
                        if (cutout->running()) {
                            return ai_fail(api::ErrorCode::Busy,
                                           "a cutout run is already in flight");
                        }
                        const char *model_id = subject == "person" ? "rvm" : "isnet";
                        if (const ai::ModelEntry *entry = find_model(model_id);
                            entry != nullptr && !model_file_present(model_root, *entry)) {
                            return ai_fail(api::ErrorCode::Failed,
                                           "the " + subject
                                                   + " model is not installed; call ai.download "
                                                     "first");
                        }
                        if (alternative.clip_id && !alternative.clip_id->empty()) {
                            cutout->setClipId(QString::fromStdString(*alternative.clip_id));
                        }
                        cutout->applyCutout(QString::fromStdString(subject));
                        return ai_done();
                    }

                    if (kind == "enhance") {
                        if (!enhance->available()) {
                            return ai_fail(api::ErrorCode::NotAvailable,
                                           enhance->statusText().toStdString());
                        }
                        if (!alternative.clip_id || alternative.clip_id->empty()) {
                            return ai_fail(api::ErrorCode::Invalid,
                                           "ai.run enhance requires clipId");
                        }
                        if (enhance->running()) {
                            return ai_fail(api::ErrorCode::Busy,
                                           "an enhance run is already in flight");
                        }
                        if (const ai::ModelEntry *entry = find_model("real-esrgan");
                            entry != nullptr && !model_file_present(model_root, *entry)) {
                            return ai_fail(api::ErrorCode::Failed,
                                           "the real-esrgan model is not installed; call "
                                           "ai.download first");
                        }
                        enhance->setClipId(QString::fromStdString(*alternative.clip_id));
                        if (alternative.params.contains("factor")) {
                            const nlohmann::json &factor = alternative.params["factor"];
                            if (!factor.is_number_integer()) {
                                return ai_fail(api::ErrorCode::Invalid,
                                               "ai.run enhance factor must be an integer");
                            }
                            enhance->setFactor(factor.get<int>());
                        }
                        enhance->applyEnhance();
                        return ai_done();
                    }

                    // kind == "tts"
                    if (!tts->available()) {
                        return ai_fail(api::ErrorCode::NotAvailable,
                                       tts->statusText().toStdString());
                    }
                    if (!alternative.params.is_object() || !alternative.params.contains("text")) {
                        return ai_fail(api::ErrorCode::Invalid, "ai.run tts requires params.text");
                    }
                    const nlohmann::json &text_json = alternative.params["text"];
                    if (!text_json.is_string()) {
                        return ai_fail(api::ErrorCode::Invalid, "ai.run tts text must be a string");
                    }
                    const std::string text = text_json.get<std::string>();
                    if (text.empty()) {
                        return ai_fail(api::ErrorCode::Invalid,
                                       "ai.run tts requires non-empty params.text");
                    }
                    if (tts->running()) {
                        return ai_fail(api::ErrorCode::Busy, "a tts run is already in flight");
                    }
                    if (const ai::ModelEntry *entry = find_model("kokoro");
                        entry != nullptr && !model_file_present(model_root, *entry)) {
                        return ai_fail(api::ErrorCode::Failed,
                                       "the kokoro model is not installed; call ai.download first");
                    }
                    tts->setText(QString::fromStdString(text));
                    if (alternative.params.contains("voice")) {
                        const nlohmann::json &voice = alternative.params["voice"];
                        if (voice.is_number_integer()) {
                            // A Kokoro speaker id; clears any cloned selection.
                            tts->selectClonedVoice(QString());
                            tts->setVoice(voice.get<int>());
                        } else if (voice.is_string()) {
                            // A cloned voice, selected by name (the picker's
                            // `voices` entry with `cloned = true`).
                            tts->selectClonedVoice(
                                    QString::fromStdString(voice.get<std::string>()));
                        } else {
                            return ai_fail(api::ErrorCode::Invalid,
                                           "ai.run tts voice must be an integer or a string");
                        }
                    }
                    if (alternative.params.contains("speed")) {
                        const nlohmann::json &speed = alternative.params["speed"];
                        if (!speed.is_number()) {
                            return ai_fail(api::ErrorCode::Invalid,
                                           "ai.run tts speed must be a number");
                        }
                        tts->setSpeed(speed.get<double>());
                    }
                    tts->speak();
                    if (tts->awaitingConsent()) {
                        // The tarball is installed but not yet unpacked: the
                        // API call is the consent, so auto-confirm and let the
                        // worker unpack it.
                        tts->confirmDownload();
                    }
                    return ai_done();
                }

                if constexpr (std::is_same_v<T, api::AiCancel>) {
                    if (!valid_ai_kind(alternative.kind)) {
                        return ai_fail(api::ErrorCode::Invalid,
                                       "unknown ai kind '" + alternative.kind + "'");
                    }
                    if (const std::shared_ptr<AiDownloadSlot> slot =
                                find_slot(ai_slots, alternative.kind)) {
                        slot->cancel.store(true, std::memory_order_relaxed);
                    }
                    if (alternative.kind == "captions") {
                        captions->cancel();
                    } else if (alternative.kind == "cutout") {
                        cutout->cancel();
                    } else if (alternative.kind == "enhance") {
                        enhance->cancel();
                    } else {
                        tts->cancel();
                    }
                    return ai_done();
                }

                static_assert(api::detail::is_ai_request_v<T>, "ai request has no handler");
            },
            request.value);
}