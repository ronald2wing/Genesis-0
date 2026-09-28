// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "extensions/Manager.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

namespace ext = genesis::extensions;

const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-extension-manager-tests";

std::filesystem::path store_for(const std::string &name)
{
    const std::filesystem::path store = kBase / name;
    std::filesystem::remove_all(store);
    std::filesystem::create_directories(store);
    return store;
}

// A format-2 data-only native manifest (no `entry`, so no library is loaded).
std::string data_only_manifest(const std::string &id, std::uint32_t version,
                               const std::vector<std::string> &dependencies = { })
{
    std::string text = "format = 2\n\n[extension]\n";
    text += "id = \"" + id + "\"\n";
    text += "name = \"Data " + id + "\"\n";
    text += "kind = \"native\"\n";
    text += "version = " + std::to_string(version) + "\n";
    text += "api = 1\n";
    if (!dependencies.empty()) {
        text += "requires = [";
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            text += "\"" + dependencies[i] + "\"";
        }
        text += "]\n";
    }
    return text;
}

void write_extension(const std::filesystem::path &store, const std::string &id,
                     std::uint32_t version, const std::vector<std::string> &dependencies = { })
{
    const std::filesystem::path root = store / id / std::to_string(version);
    std::filesystem::create_directories(root);
    std::ofstream out(root / "extension.toml");
    out << data_only_manifest(id, version, dependencies);
}

void write_origins(const std::filesystem::path &store, const std::string &json_text)
{
    std::ofstream out(store / "origins.json");
    out << json_text;
}

// The id of the record with the given id, or nullptr when absent.
const ext::ExtensionRecord *find_record(const std::vector<ext::ExtensionRecord> &records,
                                        const std::string &id)
{
    for (const ext::ExtensionRecord &record : records) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

void test_read_dependency_graph()
{
    const std::filesystem::path store = store_for("graph");
    write_extension(store, "dep.child", 1, { "dep.base", "missing.one" });
    write_extension(store, "dep.base", 1);

    const ext::Graph graph = ext::read_dependency_graph(store);
    check(graph.requires_of.size() == 2, "both installed ids contribute edges");
    check(graph.requires_of.at("dep.child").size() == 2, "the dependent records both requires");
    check(graph.requires_of.at("dep.child")[0] == "dep.base"
                  && graph.requires_of.at("dep.child")[1] == "missing.one",
          "in manifest order");
    check(graph.requires_of.at("dep.base").empty(), "a leaf records no requires");

    check(graph.dependents.at("dep.base").size() == 1
                  && graph.dependents.at("dep.base")[0] == "dep.child",
          "the dependency records its dependent");
    check(graph.dependents.at("missing.one").size() == 1
                  && graph.dependents.at("missing.one")[0] == "dep.child",
          "a missing dependency is still keyed by its dependents");
}

void test_disable_propagates_to_dependents()
{
    const std::filesystem::path store = store_for("disable");
    write_extension(store, "dep.child", 1, { "dep.base" });
    write_extension(store, "dep.base", 1);
    write_origins(store, "{\"dep.child\": \"builtin\", \"dep.base\": \"builtin\"}");

    ext::Manager manager(store);
    check(manager.disable("dep.base"), "disabling the dependency succeeds");

    const std::vector<ext::ExtensionRecord> records = manager.list_extensions();
    const ext::ExtensionRecord *base = find_record(records, "dep.base");
    const ext::ExtensionRecord *child = find_record(records, "dep.child");
    check(base != nullptr && base->state == ext::ExtensionState::Disabled,
          "the dependency is disabled");
    check(child != nullptr && child->state == ext::ExtensionState::Disabled,
          "the dependent is disabled too");
    if (child != nullptr) {
        check(child->reason == "requires dep.base", "the dependent's reason names its parent");
    }
    if (base != nullptr) {
        check(base->reason.empty(), "a direct disable records no reason");
    }
}

void test_enable_does_not_reenable_dependents()
{
    const std::filesystem::path store = store_for("enable");
    write_extension(store, "dep.child", 1, { "dep.base" });
    write_extension(store, "dep.base", 1);
    write_origins(store, "{\"dep.child\": \"builtin\", \"dep.base\": \"builtin\"}");

    ext::Manager manager(store);
    (void)manager.disable("dep.base");
    check(manager.enable("dep.base"), "re-enabling the dependency succeeds");

    const std::vector<ext::ExtensionRecord> records = manager.list_extensions();
    const ext::ExtensionRecord *base = find_record(records, "dep.base");
    const ext::ExtensionRecord *child = find_record(records, "dep.child");
    check(base != nullptr && base->state == ext::ExtensionState::Enabled,
          "the dependency is enabled again");
    check(child != nullptr && child->state == ext::ExtensionState::Disabled,
          "the dependent stays disabled until explicitly re-enabled");
    if (child != nullptr) {
        check(child->reason == "requires dep.base", "its reason is preserved");
    }
}

void test_remove_refuses_builtin()
{
    const std::filesystem::path store = store_for("remove_builtin");
    write_extension(store, "dep.base", 1);
    write_origins(store, "{\"dep.base\": \"builtin\"}");

    ext::Manager manager(store);
    check(!manager.remove_extension("dep.base"), "a builtin id refuses removal");
    check(std::filesystem::exists(store / "dep.base"), "and its installed copy is untouched");
}

void test_remove_then_restore_curated()
{
    const std::filesystem::path store = store_for("remove_curated");
    write_extension(store, "ext.curated", 1);
    write_origins(store, "{\"ext.curated\": \"curated\"}");

    ext::Manager manager(store);
    check(manager.remove_extension("ext.curated"), "a curated id removes successfully");
    check(!std::filesystem::exists(store / "ext.curated"), "and its installed copy is deleted");

    check(manager.restore_extension("ext.curated"), "a curated id restores successfully");
}

void test_remove_developer_allowed()
{
    const std::filesystem::path store = store_for("remove_developer");
    write_extension(store, "ext.dev", 1);
    // No origins.json: absent id reads as Developer.
    ext::Manager manager(store);
    check(manager.remove_extension("ext.dev"), "a developer id removes successfully");
    check(!std::filesystem::exists(store / "ext.dev"), "and its installed copy is deleted");
}

void test_restore_refuses_builtin()
{
    const std::filesystem::path store = store_for("restore_builtin");
    write_extension(store, "dep.base", 1);
    write_origins(store, "{\"dep.base\": \"builtin\"}");

    ext::Manager manager(store);
    check(!manager.restore_extension("dep.base"), "a builtin id refuses restore");
}

void test_remove_disables_dependents()
{
    const std::filesystem::path store = store_for("remove_dependents");
    write_extension(store, "dep.child", 1, { "dep.base" });
    write_extension(store, "dep.base", 1);
    write_origins(store, "{\"dep.child\": \"curated\", \"dep.base\": \"curated\"}");

    ext::Manager manager(store);
    check(manager.remove_extension("dep.base"), "removing the curated dependency succeeds");

    const std::vector<ext::ExtensionRecord> records = manager.list_extensions();
    check(find_record(records, "dep.base") == nullptr, "the removed extension is no longer listed");
    const ext::ExtensionRecord *child = find_record(records, "dep.child");
    check(child != nullptr && child->state == ext::ExtensionState::Disabled,
          "its dependent is disabled");
    if (child != nullptr) {
        check(child->reason == "requires dep.base", "with the parent named as the reason");
    }
}

void test_list_enabled_first()
{
    const std::filesystem::path store = store_for("list_order");
    write_extension(store, "dep.enabled", 1);
    write_extension(store, "dep.disabled", 1);
    write_origins(store, "{\"dep.enabled\": \"builtin\", \"dep.disabled\": \"builtin\"}");
    {
        std::ofstream out(store / "disabled.json");
        out << "{\"ids\": [\"dep.disabled\"]}\n";
    }

    ext::Manager manager(store);
    const std::vector<ext::ExtensionRecord> records = manager.list_extensions();
    check(records.size() == 2, "both extensions are listed");
    if (records.size() == 2) {
        check(records[0].id == "dep.enabled" && records[0].state == ext::ExtensionState::Enabled,
              "enabled records come first");
        check(records[1].id == "dep.disabled" && records[1].state == ext::ExtensionState::Disabled,
              "disabled records come after");
    }
}

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);

    test_read_dependency_graph();
    test_disable_propagates_to_dependents();
    test_enable_does_not_reenable_dependents();
    test_remove_refuses_builtin();
    test_remove_then_restore_curated();
    test_remove_developer_allowed();
    test_restore_refuses_builtin();
    test_remove_disables_dependents();
    test_list_enabled_first();

    std::filesystem::remove_all(kBase);

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
