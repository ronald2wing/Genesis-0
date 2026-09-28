// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The manifest parse/validate suite: entry presence, capability-id assignment
// and uniqueness, overrides, effect parsing, and a full media-pack-shaped
// manifest. Pure parse/validate, no library or store.

#include <cstdio>
#include <string>

#include "extensions/ExtensionManifest.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace ext = genesis::extensions;

// A data-only manifest (no `entry`), used as a base for capability tests.
std::string data_only(const std::string &id)
{
    return "[extension]\nid = \"" + id
            + "\"\nname = \"Demo\"\n"
              "version = 1\n";
}

// A native manifest with an entry.
std::string native_with_entry(const std::string &id)
{
    return "[extension]\nid = \"" + id
            + "\"\nname = \"Native\"\nversion = 1\napi = 1\n"
              "entry = \"libfixture.so\"\n";
}

// --- entry presence ---

void test_native_with_entry_ok()
{
    const ext::NativeLoadResult loaded =
            ext::load_native_manifest(native_with_entry("test.native"));
    check(loaded.manifest.has_value(), "native with entry parses");
    if (loaded.manifest) {
        check(loaded.manifest->entry.has_value(), "entry is present");
    }
}

void test_data_only_without_entry_ok()
{
    const ext::NativeLoadResult loaded = ext::load_native_manifest(data_only("test.data"));
    check(loaded.manifest.has_value(), "data-only without entry parses");
    if (loaded.manifest) {
        check(!loaded.manifest->entry.has_value(), "entry is absent");
    }
}

void test_data_only_with_entry_ok()
{
    const std::string with_entry = "[extension]\nid = \"test.dataentry\"\nname = \"Test\"\n"
                                   "version = 1\nentry = \"lib.so\"\n";
    const ext::NativeLoadResult loaded = ext::load_native_manifest(with_entry);
    // entry presence is valid for any extension — kind no longer gates it.
    check(loaded.manifest.has_value(), "data-only with entry parses");
    if (loaded.manifest) {
        check(loaded.manifest->entry.has_value(), "entry is present");
    }
}

// --- capability-id assignment and uniqueness ---

void test_default_ids()
{
    const ext::NativeLoadResult loaded = ext::load_native_manifest(
            data_only("demo.menu")
            + "\n[[menu]]\npath = \"Tools/A\"\nlabel = \"A\"\ncall = \"a\"\n"
              "\n[[menu]]\npath = \"Tools/B\"\nlabel = \"B\"\ncall = \"b\"\n"
              "\n[[panel]]\nlabel = \"P\"\nqml = \"P.qml\"\n"
              "\n[[dialog]]\nlabel = \"D\"\nqml = \"D.qml\"\n");
    check(loaded.manifest.has_value(), "a manifest without explicit ids parses");
    if (!loaded.manifest) {
        return;
    }
    const ext::ExtensionManifest &m = *loaded.manifest;
    check(m.menus.size() == 2 && m.menus[0].id == "demo.menu:0" && m.menus[1].id == "demo.menu:1",
          "menus get the per-kind index default");
    check(m.panels.size() == 1 && m.panels[0].id == "demo.menu:0",
          "panels restart their index at 0");
    check(m.dialogs.size() == 1 && m.dialogs[0].id == "demo.menu:0",
          "dialogs restart their index at 0");
}

void test_explicit_ids_round_trip()
{
    const ext::NativeLoadResult loaded = ext::load_native_manifest(
            data_only("demo.menu")
            + "\n[[menu]]\nid = \"demo.tools\"\npath = \"Tools/A\"\nlabel = \"A\"\n"
              "call = \"a\"\n"
              "\n[[panel]]\nid = \"demo.menu:7\"\nlabel = \"P\"\nqml = \"P.qml\"\n"
              "\n[[dialog]]\nid = \"acme.dialog\"\nlabel = \"D\"\nqml = \"D.qml\"\n");
    check(loaded.manifest.has_value(), "explicit ids parse");
    if (!loaded.manifest) {
        return;
    }
    const ext::ExtensionManifest &m = *loaded.manifest;
    check(m.menus[0].id == "demo.tools", "a namespaced menu id round-trips");
    check(m.panels[0].id == "demo.menu:7", "a default-form panel id round-trips");
    check(m.dialogs[0].id == "acme.dialog", "a namespaced dialog id round-trips");
}

void test_invalid_id_forms_refused()
{
    // A default-form id whose suffix is not digits.
    const ext::NativeLoadResult bad_suffix =
            ext::load_native_manifest(data_only("demo.menu")
                                      + "\n[[panel]]\nid = \"demo.menu:x\"\nlabel = \"P\"\n"
                                        "qml = \"P.qml\"\n");
    check(!bad_suffix.manifest.has_value(), "a non-digit default-form id is refused");

    // A bare un-namespaced id (no dot).
    const ext::NativeLoadResult no_ns =
            ext::load_native_manifest(data_only("demo.menu")
                                      + "\n[[panel]]\nid = \"nocolon\"\nlabel = \"P\"\n"
                                        "qml = \"P.qml\"\n");
    check(!no_ns.manifest.has_value(), "an un-namespaced, un-default id is refused");

    // An id with a space and capital (not a valid segment).
    const ext::NativeLoadResult bad_segment =
            ext::load_native_manifest(data_only("demo.menu")
                                      + "\n[[panel]]\nid = \"Bad Id\"\nlabel = \"P\"\n"
                                        "qml = \"P.qml\"\n");
    check(!bad_segment.manifest.has_value(), "an id with a space is refused");
}

void test_within_kind_uniqueness_cross_kind_allowed()
{
    // Two menus default to distinct ids (index 0 and 1), so this is a real
    // explicit duplicate, not the default.
    const ext::NativeLoadResult dup_menu = ext::load_native_manifest(
            data_only("demo.menu")
            + "\n[[menu]]\nid = \"demo.menu:0\"\npath = \"Tools/A\"\nlabel = \"A\"\n"
              "call = \"a\"\n"
              "\n[[menu]]\nid = \"demo.menu:0\"\npath = \"Tools/B\"\nlabel = \"B\"\n"
              "call = \"b\"\n");
    check(!dup_menu.manifest.has_value(), "a duplicate menu id within one extension is refused");

    // The same id across kinds is allowed: each kind's namespace is separate.
    const ext::NativeLoadResult cross_kind = ext::load_native_manifest(
            data_only("demo.menu")
            + "\n[[menu]]\nid = \"demo.menu:0\"\npath = \"Tools/A\"\nlabel = \"A\"\n"
              "call = \"a\"\n"
              "\n[[panel]]\nid = \"demo.menu:0\"\nlabel = \"P\"\nqml = \"P.qml\"\n");
    check(cross_kind.manifest.has_value(), "the same id across menu and panel is allowed");
}

// --- overrides ---

void test_overrides_round_trip()
{
    const ext::NativeLoadResult loaded = ext::load_native_manifest(
            data_only("override.me")
            + "overrides = [\"action:base.menu:0\", \"panel:base.panel\", "
              "\"dialog:base.dialog:2\"]\n");
    check(loaded.manifest.has_value(), "override entries parse");
    if (!loaded.manifest) {
        return;
    }
    const ext::ExtensionManifest &m = *loaded.manifest;
    check(m.overrides.size() == 3, "three overrides are read");
    if (m.overrides.size() == 3) {
        check(m.overrides[0].kind == ext::CapabilityKind::Action
                      && m.overrides[0].id == "base.menu:0",
              "the action override round-trips");
        check(m.overrides[1].kind == ext::CapabilityKind::Panel
                      && m.overrides[1].id == "base.panel",
              "the panel override round-trips");
        check(m.overrides[2].kind == ext::CapabilityKind::Dialog
                      && m.overrides[2].id == "base.dialog:2",
              "the dialog override round-trips");
    }
}

void test_effect_override_parses()
{
    const ext::NativeLoadResult loaded = ext::load_native_manifest(
            data_only("override.effect") + "overrides = [\"effect:genesis.glow\"]\n");
    check(loaded.manifest.has_value(), "an effect override parses");
    if (!loaded.manifest) {
        return;
    }
    const ext::ExtensionManifest &m = *loaded.manifest;
    check(m.overrides.size() == 1, "one effect override is read");
    if (m.overrides.size() == 1) {
        check(m.overrides[0].kind == ext::CapabilityKind::Effect, "the kind is Effect");
        check(m.overrides[0].id == "genesis.glow", "the effect id round-trips");
    }
}

void test_override_syntax_refused()
{
    // An unknown kind.
    const ext::NativeLoadResult bad_kind = ext::load_native_manifest(
            data_only("override.me") + "overrides = [\"widget:base.panel\"]\n");
    check(!bad_kind.manifest.has_value(), "an unknown override kind is refused");

    // An empty id (nothing after the first colon).
    const ext::NativeLoadResult empty_id =
            ext::load_native_manifest(data_only("override.me") + "overrides = [\"panel:\"]\n");
    check(!empty_id.manifest.has_value(), "an override with an empty id is refused");

    // A bare word with no colon (no kind separator).
    const ext::NativeLoadResult no_colon =
            ext::load_native_manifest(data_only("override.me") + "overrides = [\"base.panel\"]\n");
    check(!no_colon.manifest.has_value(), "an override without a kind separator is refused");

    // A valid kind but an invalid capability id (not a segment).
    const ext::NativeLoadResult bad_id = ext::load_native_manifest(
            data_only("override.me") + "overrides = [\"panel:Bad Id\"]\n");
    check(!bad_id.manifest.has_value(), "an override id in an invalid form is refused");
}

void test_overrides_parse_without_origin_gate()
{
    const ext::NativeLoadResult loaded = ext::load_native_manifest(
            data_only("developer.thing") + "overrides = [\"panel:base.panel\"]\n");
    check(loaded.manifest.has_value(), "an overrides declaration parses with no origin gate");
    if (loaded.manifest) {
        check(loaded.manifest->overrides.size() == 1, "and the entry is read");
    }
}

// --- effects ---

void test_effect_blocks_parse()
{
    const std::string pack_manifest = "[extension]\n"
                                      "id = \"test.pack\"\n"
                                      "name = \"Pack\"\n"
                                      "version = 2\n"
                                      "\n"
                                      "[[effect]]\n"
                                      "id = \"test.pack\"\n"
                                      "name = \"Pack Effect\"\n"
                                      "kind = \"effect\"\n";
    const ext::NativeLoadResult loaded = ext::load_native_manifest(pack_manifest);
    check(loaded.manifest.has_value(), "a manifest with [[effect]] parses");
    if (!loaded.manifest) {
        return;
    }
    const ext::ExtensionManifest &m = *loaded.manifest;
    check(m.effects.size() == 1, "one effect parsed");
    if (m.effects.size() == 1) {
        check(m.effects[0].id == "test.pack", "effect id round-trips");
        check(m.effects[0].name == "Pack Effect", "effect name round-trips");
        check(m.effects[0].kind == genesis::effects::Kind::Effect, "effect kind round-trips");
    }
}

void test_full_media_pack_parses()
{
    const ext::NativeLoadResult loaded = ext::load_native_manifest(R"(
        [extension]
        id = "acme.media-pack"
        name = "Acme Media Pack"
        version = 3
        requires = ["genesis.packs"]

        [[effect]]
        id = "acme.vhs"
        name = "VHS"
        kind = "effect"
        category = "Retro"
        order = 40
        intensity = "strength"
        description = "Tape wobble, chroma bleed and scanlines."

        [[effect.param]]
        key = "strength"
        label = "Strength"
        min = 0
        max = 100
        default = 50

        [[effect]]
        id = "acme.glitch"
        name = "Glitch"
        kind = "effect"
        category = "Retro"
        order = 41
    )");
    check(loaded.manifest.has_value(), "a full media-pack manifest parses");
    if (!loaded.manifest) {
        return;
    }
    const ext::ExtensionManifest &m = *loaded.manifest;
    check(m.id == "acme.media-pack", "pack id round-trips");
    check(m.version == 3, "version round-trips");
    check(m.dependencies.size() == 1 && m.dependencies[0] == "genesis.packs",
          "requires round-trips");
    check(m.effects.size() == 2, "two effects parsed");
    if (m.effects.size() == 2) {
        check(m.effects[0].id == "acme.vhs", "first effect id");
        check(m.effects[0].parameters.size() == 1 && m.effects[0].parameters[0].key == "strength",
              "first effect parameter parsed from scoped sub-table");
        check(m.effects[1].id == "acme.glitch", "second effect id");
    }
}

} // namespace

int main()
{
    test_native_with_entry_ok();
    test_data_only_without_entry_ok();
    test_data_only_with_entry_ok();
    test_default_ids();
    test_explicit_ids_round_trip();
    test_invalid_id_forms_refused();
    test_within_kind_uniqueness_cross_kind_allowed();
    test_overrides_round_trip();
    test_effect_override_parses();
    test_override_syntax_refused();
    test_overrides_parse_without_origin_gate();
    test_effect_blocks_parse();
    test_full_media_pack_parses();

    return genesis::test::summary();
}