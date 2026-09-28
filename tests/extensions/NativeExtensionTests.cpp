// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "extensions/NativeExtension.h"

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

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

namespace ext = genesis::extensions;

// The fixture libraries are built side by side with this executable in the
// runtime output directory; resolve that directory from the running binary so
// the suite needs no path baked in at configure time.
std::filesystem::path fixture_dir()
{
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::canonical("/proc/self/exe", ec);
    return ec ? std::filesystem::path{ } : exe.parent_path();
}

std::filesystem::path fixture(const std::string &name)
{
    return fixture_dir() / ("libextension_fixture_" + name + ".so");
}

// A host whose call_json answers "version" with a fixed reply and whose log
// collects messages, so the round-trip and the callbacks are observable.
struct FakeHost
{
    std::vector<std::string> logs;

    ext::NativeExtension::Host build()
    {
        ext::NativeExtension::Host host;
        host.call_json = [](const std::string &method, const std::string &, std::string &out) {
            if (method == "version") {
                out = "pong";
                return 0;
            }
            return -1;
        };
        host.log = [this](const std::string &message) { logs.push_back(message); };
        return host;
    }
};

void test_loads_invokes_and_notifies()
{
    FakeHost fake;
    auto loaded = ext::NativeExtension::load(fixture("ok"), "ext.fixture", 1, ext::Origin::Builtin,
                                             false, fake.build());
    check(loaded.has_value(), "a well-behaved extension loads");
    if (!loaded) {
        std::printf("  (load refused: %s)\n", loaded.error().c_str());
        return;
    }
    check(loaded->id() == "ext.fixture", "the loaded extension reports its id");

    // invoke routes a version call through the host bridge and echoes the
    // action, proving both the invoke path and the bridge round-trip.
    const auto result = loaded->invoke("greet", "{}");
    check(result.has_value(), "invoke succeeds");
    if (result) {
        check(*result == "greet:pong", "invoke echoes the action and the host's version reply");
    }

    // The notification callbacks reach the injected log sink.
    loaded->on_project_opened("/tmp/project.json");
    loaded->on_shutdown();
    check(fake.logs.size() == 2, "both callbacks logged");
    if (fake.logs.size() == 2) {
        check(contains(fake.logs[0], "project opened"), "on_project_opened logged");
        check(contains(fake.logs[1], "shutdown"), "on_shutdown logged");
    }
}

void test_abi_mismatch_refused()
{
    FakeHost fake;
    const auto loaded = ext::NativeExtension::load(fixture("abi"), "ext.fixture", 1,
                                                   ext::Origin::Builtin, false, fake.build());
    check(!loaded.has_value(), "an ABI-mismatching extension is refused");
    if (!loaded) {
        check(contains(loaded.error(), "declined to load"),
              "the refusal names the entry's decline");
    }
}

void test_wrong_id_refused()
{
    FakeHost fake;
    const auto loaded = ext::NativeExtension::load(fixture("wrong"), "ext.fixture", 1,
                                                   ext::Origin::Builtin, false, fake.build());
    check(!loaded.has_value(), "a mismatching id is refused");
    if (!loaded) {
        check(contains(loaded.error(), "does not match"), "the refusal names the id mismatch");
    }
}

void test_no_entry_refused()
{
    FakeHost fake;
    const auto loaded = ext::NativeExtension::load(fixture("noentry"), "ext.fixture", 1,
                                                   ext::Origin::Builtin, false, fake.build());
    check(!loaded.has_value(), "a library with no entry is refused");
    if (!loaded) {
        check(contains(loaded.error(), "exports no genesis_extension_entry"),
              "the refusal names the missing entry");
    }
}

void test_user_origin_refused_before_load()
{
    FakeHost fake;
    const auto loaded = ext::NativeExtension::load(fixture("ok"), "ext.fixture", 1,
                                                   ext::Origin::User, false, fake.build());
    check(!loaded.has_value(), "a User extension is refused");
    if (!loaded) {
        check(contains(loaded.error(), "not trusted"), "the refusal names the untrusted origin");
    }
}

void test_developer_requires_consent()
{
    FakeHost fake;
    const auto refused = ext::NativeExtension::load(fixture("ok"), "ext.fixture", 1,
                                                    ext::Origin::Developer, false, fake.build());
    check(!refused.has_value(), "a Developer extension is refused without consent");
    if (!refused) {
        check(contains(refused.error(), "no recorded consent"),
              "the refusal names the missing consent");
    }

    FakeHost consented_host;
    const auto allowed = ext::NativeExtension::load(
            fixture("ok"), "ext.fixture", 1, ext::Origin::Developer, true, consented_host.build());
    check(allowed.has_value(), "a consented Developer extension loads");
}

void test_api_mismatch_refused()
{
    FakeHost fake;
    const auto loaded = ext::NativeExtension::load(fixture("ok"), "ext.fixture", 2,
                                                   ext::Origin::Builtin, false, fake.build());
    check(!loaded.has_value(), "an API mismatch is refused");
    if (!loaded) {
        check(contains(loaded.error(), "targets API"), "the refusal names the API gap");
    }
}

void test_missing_library_refused()
{
    FakeHost fake;
    const auto loaded = ext::NativeExtension::load(fixture("does_not_exist"), "ext.fixture", 1,
                                                   ext::Origin::Builtin, false, fake.build());
    check(!loaded.has_value(), "a missing library is refused");
    if (!loaded) {
        check(contains(loaded.error(), "cannot load the library"),
              "the refusal names the load failure");
    }
}

void test_no_invoke_contributed()
{
    // An extension that reports no invoke (a valid descriptor: `invoke` may be
    // NULL) loads fine, but invoking it fails cleanly rather than crashing.
    FakeHost fake;
    auto loaded = ext::NativeExtension::load(fixture("noinvoke"), "ext.fixture", 1,
                                             ext::Origin::Builtin, false, fake.build());
    check(loaded.has_value(), "an extension without invoke still loads");
    if (loaded) {
        const auto result = loaded->invoke("greet", "{}");
        check(!result.has_value(), "invoke on a no-invoke extension fails");
        if (!result) {
            check(contains(result.error(), "no invoke action"),
                  "the failure names the missing invoke");
        }
    }
}

} // namespace

int main()
{
    const std::filesystem::path dir = fixture_dir();
    if (dir.empty()) {
        std::printf("cannot resolve the fixture directory from /proc/self/exe\n");
        return 1;
    }

    test_loads_invokes_and_notifies();
    test_abi_mismatch_refused();
    test_wrong_id_refused();
    test_no_entry_refused();
    test_user_origin_refused_before_load();
    test_developer_requires_consent();
    test_api_mismatch_refused();
    test_missing_library_refused();
    test_no_invoke_contributed();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
