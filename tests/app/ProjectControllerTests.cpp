// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The project controller's suite: drives the open path headlessly and pins the
// one-shot "document changed on load" warning - a document carrying top-level
// keys this build does not model sets it, a clean document clears it, and the
// dismiss affordance clears it too. No GUI and no GStreamer; a real Editor and
// a temp document file exercise the controller exactly as QML calls it.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <QCoreApplication>
#include <QString>

#include "../support/Checks.h"
#include "../support/Fs.h"

#include "app/controllers/ProjectController.h"
#include "project/Editor.h"

namespace {

using genesis::test::check;
using genesis::test::summary;
using genesis::test::write_text_file;

// A minimal, loadable document: one timeline with one lane, no clips. `extra`
// fields ride along as unknown top-level keys.
std::string document_json(std::string_view extra)
{
    std::string json = R"({
        "name": "Fixture", "version": 1,
        )";
    json += extra;
    json += R"("media": [],
        "timelines": [
            { "id": "TL1", "name": "Timeline 1",
              "tracks": [{ "id": "T1", "visible": true, "muted": false }], "clips": [] }
        ],
        "activeTimelineId": "TL1"
    })";
    return json;
}

struct Harness
{
    std::filesystem::path root =
            std::filesystem::temp_directory_path() / "genesis-project-controller-test";
    std::filesystem::path doc = root / "project.json";
    genesis::project::Editor editor;
    ProjectController controller{ &editor, root / "config" };

    Harness()
    {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "config");
    }

    ~Harness() { std::filesystem::remove_all(root); }
};

void test_unknown_keys_set_the_warning()
{
    Harness harness;
    write_text_file(harness.doc,
                    document_json("\"timelineStyle\": \"rounded\", \"futureFeature\": 1,\n"));

    check(harness.controller.loadWarning().isEmpty(), "a fresh controller holds no warning");
    check(harness.controller.openProject(QString::fromStdString(harness.doc.string())), "opens");
    check(!harness.controller.loadWarning().isEmpty(), "unknown top-level keys set the warning");
}

void test_a_clean_document_clears_the_warning()
{
    Harness harness;
    write_text_file(harness.doc, document_json("\"timelineStyle\": \"rounded\",\n"));
    check(harness.controller.openProject(QString::fromStdString(harness.doc.string())), "opens");
    check(!harness.controller.loadWarning().isEmpty(), "the unknown key sets the warning");

    write_text_file(harness.doc, document_json(""));
    check(harness.controller.openProject(QString::fromStdString(harness.doc.string())), "reopens");
    check(harness.controller.loadWarning().isEmpty(), "a clean document clears the warning");
}

void test_dismiss_clears_the_warning()
{
    Harness harness;
    write_text_file(harness.doc, document_json("\"futureFeature\": 1,\n"));
    check(harness.controller.openProject(QString::fromStdString(harness.doc.string())), "opens");
    check(!harness.controller.loadWarning().isEmpty(), "the unknown key sets the warning");

    harness.controller.clearLoadWarning();
    check(harness.controller.loadWarning().isEmpty(), "the dismiss affordance clears the warning");
}

void test_new_project_clears_the_warning()
{
    Harness harness;
    write_text_file(harness.doc, document_json("\"futureFeature\": 1,\n"));
    check(harness.controller.openProject(QString::fromStdString(harness.doc.string())), "opens");
    check(!harness.controller.loadWarning().isEmpty(), "the unknown key sets the warning");

    check(harness.controller.newProject(QStringLiteral("Fresh")), "starts a new project");
    check(harness.controller.loadWarning().isEmpty(), "a fresh project holds no warning");
}

void test_the_warning_signal_fires_on_change()
{
    Harness harness;
    int emissions = 0;
    QObject::connect(&harness.controller, &ProjectController::loadWarningChanged,
                     [&emissions] { ++emissions; });

    write_text_file(harness.doc, document_json("\"futureFeature\": 1,\n"));
    check(harness.controller.openProject(QString::fromStdString(harness.doc.string())), "opens");
    check(emissions == 1, "setting the warning emits once");

    // Reopening the same document sets the identical warning, which does not
    // re-emit: the one-shot state is idempotent, not a fresh signal per open.
    check(harness.controller.openProject(QString::fromStdString(harness.doc.string())), "reopens");
    check(emissions == 1, "an identical warning does not re-emit");

    harness.controller.clearLoadWarning();
    check(emissions == 2, "clearing emits once");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_unknown_keys_set_the_warning();
    test_a_clean_document_clears_the_warning();
    test_dismiss_clears_the_warning();
    test_new_project_clears_the_warning();
    test_the_warning_signal_fires_on_change();

    return summary();
}
