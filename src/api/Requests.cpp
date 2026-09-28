// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "api/Requests.h"

#include <string_view>
#include <type_traits>
#include <variant>

namespace genesis::api {

std::string_view method_name(const Request &request)
{
    return std::visit(
            [](const auto &alternative) -> std::string_view {
                using T = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<T, Version>) {
                    return "version";
                } else if constexpr (std::is_same_v<T, ProjectCreate>) {
                    return "project.create";
                } else if constexpr (std::is_same_v<T, ProjectOpen>) {
                    return "project.open";
                } else if constexpr (std::is_same_v<T, ProjectClose>) {
                    return "project.close";
                } else if constexpr (std::is_same_v<T, ProjectGet>) {
                    return "project.get";
                } else if constexpr (std::is_same_v<T, ProjectDocument>) {
                    return "project.document";
                } else if constexpr (std::is_same_v<T, ProjectSave>) {
                    return "project.save";
                } else if constexpr (std::is_same_v<T, ProjectSetVideo>) {
                    return "project.setVideo";
                } else if constexpr (std::is_same_v<T, EditApply>) {
                    return "edit.apply";
                } else if constexpr (std::is_same_v<T, EditUndo>) {
                    return "edit.undo";
                } else if constexpr (std::is_same_v<T, EditRedo>) {
                    return "edit.redo";
                } else if constexpr (std::is_same_v<T, EditSelection>) {
                    return "edit.selection";
                } else if constexpr (std::is_same_v<T, MediaProbe>) {
                    return "media.probe";
                } else if constexpr (std::is_same_v<T, MediaImport>) {
                    return "media.import";
                } else if constexpr (std::is_same_v<T, MediaList>) {
                    return "media.list";
                } else if constexpr (std::is_same_v<T, CatalogueList>) {
                    return "catalogue.list";
                } else if constexpr (std::is_same_v<T, TemplateList>) {
                    return "template.list";
                } else if constexpr (std::is_same_v<T, TemplateFill>) {
                    return "template.fill";
                } else if constexpr (std::is_same_v<T, ExportRun>) {
                    return "export.run";
                } else if constexpr (std::is_same_v<T, ExportCancel>) {
                    return "export.cancel";
                } else if constexpr (std::is_same_v<T, ExportStatus>) {
                    return "export.status";
                } else if constexpr (std::is_same_v<T, ExportPresets>) {
                    return "export.presets";
                } else if constexpr (std::is_same_v<T, PreviewTime>) {
                    return "preview.time";
                } else if constexpr (std::is_same_v<T, ScopesSnapshot>) {
                    return "scopes.snapshot";
                } else if constexpr (std::is_same_v<T, GpuStatus>) {
                    return "gpu.status";
                } else if constexpr (std::is_same_v<T, UiPaint>) {
                    return "ui.paint";
                } else if constexpr (std::is_same_v<T, ExtensionsRequest>) {
                    return std::visit(
                            [](const auto &inner) -> std::string_view {
                                using U = std::decay_t<decltype(inner)>;
                                if constexpr (std::is_same_v<U, ExtensionsList>) {
                                    return "extensions.list";
                                } else if constexpr (std::is_same_v<U, ExtensionsRemove>) {
                                    return "extensions.remove";
                                } else if constexpr (std::is_same_v<U, ExtensionsEnable>) {
                                    return "extensions.enable";
                                } else if constexpr (std::is_same_v<U, ExtensionsDisable>) {
                                    return "extensions.disable";
                                } else if constexpr (std::is_same_v<U, ExtensionsRestore>) {
                                    return "extensions.restore";
                                } else if constexpr (std::is_same_v<U, ExtensionsInstall>) {
                                    return "extensions.install";
                                } else {
                                    static_assert(detail::is_extensions_request_v<U>,
                                                  "extension request has no method name");
                                }
                            },
                            alternative.value);
                } else if constexpr (std::is_same_v<T, ConfigExport>) {
                    return "config.export";
                } else if constexpr (std::is_same_v<T, ConfigImport>) {
                    return "config.import";
                } else if constexpr (std::is_same_v<T, ExtensionsCatalog>) {
                    return "extensions.catalog";
                } else if constexpr (std::is_same_v<T, UpdatesApply>) {
                    return "updates.apply";
                } else if constexpr (std::is_same_v<T, AiRequest>) {
                    return std::visit(
                            [](const auto &inner) -> std::string_view {
                                using U = std::decay_t<decltype(inner)>;
                                if constexpr (std::is_same_v<U, AiModels>) {
                                    return "ai.models";
                                } else if constexpr (std::is_same_v<U, AiDownload>) {
                                    return "ai.download";
                                } else if constexpr (std::is_same_v<U, AiStatus>) {
                                    return "ai.status";
                                } else if constexpr (std::is_same_v<U, AiRun>) {
                                    return "ai.run";
                                } else if constexpr (std::is_same_v<U, AiCancel>) {
                                    return "ai.cancel";
                                } else {
                                    static_assert(detail::is_ai_request_v<U>,
                                                  "ai request has no method name");
                                }
                            },
                            alternative.value);
                } else {
                    static_assert(detail::dependent_false_v<T>, "request method has no name");
                }
            },
            request.value);
}

} // namespace genesis::api
