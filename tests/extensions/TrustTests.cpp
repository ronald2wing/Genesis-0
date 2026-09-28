// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <string>

#include "extensions/Trust.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace ext = genesis::extensions;

using ext::Decision;
using ext::Origin;

// --- decide() truth table (entry-based) ---

void test_decide_builtin_with_entry_loads()
{
    check(ext::decide(Origin::Builtin, true, false) == Decision::Load,
          "Builtin with entry -> Load");
}

void test_decide_builtin_without_entry_loads()
{
    check(ext::decide(Origin::Builtin, false, false) == Decision::Load,
          "Builtin without entry -> Load");
}

void test_decide_curated_with_entry_loads()
{
    check(ext::decide(Origin::Curated, true, false) == Decision::Load,
          "Curated with entry -> Load");
}

void test_decide_curated_without_entry_loads()
{
    check(ext::decide(Origin::Curated, false, false) == Decision::Load,
          "Curated without entry -> Load");
}

void test_decide_developer_with_entry_consented_loads()
{
    check(ext::decide(Origin::Developer, true, true) == Decision::Load,
          "Developer with entry consented -> Load");
}

void test_decide_developer_with_entry_not_consented_needs_consent()
{
    check(ext::decide(Origin::Developer, true, false) == Decision::NeedsConsent,
          "Developer with entry not consented -> NeedsConsent");
}

void test_decide_developer_without_entry_loads()
{
    check(ext::decide(Origin::Developer, false, false) == Decision::Load,
          "Developer without entry -> Load (data-only loads freely)");
    check(ext::decide(Origin::Developer, false, true) == Decision::Load,
          "Developer without entry consented -> Load");
}

void test_decide_user_with_entry_refused()
{
    check(ext::decide(Origin::User, true, false) == Decision::Refuse, "User with entry -> Refuse");
    check(ext::decide(Origin::User, true, true) == Decision::Refuse,
          "User with entry consented -> still Refuse");
}

void test_decide_user_without_entry_refused()
{
    check(ext::decide(Origin::User, false, false) == Decision::Refuse,
          "User without entry -> Refuse");
    check(ext::decide(Origin::User, false, true) == Decision::Refuse,
          "User without entry consented -> still Refuse");
}

// --- Wrapper behavior preservation ---

void test_is_trusted()
{
    check(ext::is_trusted(Origin::Builtin), "Builtin is trusted");
    check(ext::is_trusted(Origin::Curated), "Curated is trusted");
    check(!ext::is_trusted(Origin::Developer), "Developer is NOT trusted");
    check(!ext::is_trusted(Origin::User), "User is NOT trusted");
}

void test_may_install_native()
{
    check(ext::may_install_native(Origin::Builtin), "Builtin may install native");
    check(ext::may_install_native(Origin::Curated), "Curated may install native");
    check(ext::may_install_native(Origin::Developer), "Developer may install native");
    check(!ext::may_install_native(Origin::User), "User may NOT install native");
}

void test_may_load_native()
{
    check(ext::may_load_native(Origin::Builtin, false), "Builtin loads native");
    check(ext::may_load_native(Origin::Builtin, true),
          "Builtin loads native (consented irrelevant)");
    check(ext::may_load_native(Origin::Curated, false), "Curated loads native");
    check(!ext::may_load_native(Origin::Developer, false),
          "Developer does NOT load native without consent");
    check(ext::may_load_native(Origin::Developer, true), "Developer loads native with consent");
    check(!ext::may_load_native(Origin::User, false), "User does NOT load native");
    check(!ext::may_load_native(Origin::User, true), "User does NOT load native (consent ignored)");
}

void test_may_override()
{
    check(ext::may_override(Origin::Builtin), "Builtin may override");
    check(ext::may_override(Origin::Curated), "Curated may override");
    // Developer may override: overrides are declarative/QML/asset swaps, not
    // arbitrary host-code execution. The security boundary is consent, not
    // origin — a Developer extension can override surfaces on their own
    // machine under recorded consent.
    check(ext::may_override(Origin::Developer), "Developer may override");
    check(!ext::may_override(Origin::User), "User may NOT override");
}

void test_permissions_for()
{
    genesis::effects::Pack dummy{ };
    auto perm_builtin = ext::permissions_for(Origin::Builtin, dummy);
    check(perm_builtin.execute, "Builtin pack gets execute");
    check(!perm_builtin.filesystem, "No pack origin gets filesystem");
    check(!perm_builtin.network, "No pack origin gets network");

    auto perm_developer = ext::permissions_for(Origin::Developer, dummy);
    check(!perm_developer.execute, "Developer pack does NOT get execute");
}

} // namespace

int main()
{
    test_decide_builtin_with_entry_loads();
    test_decide_builtin_without_entry_loads();
    test_decide_curated_with_entry_loads();
    test_decide_curated_without_entry_loads();
    test_decide_developer_with_entry_consented_loads();
    test_decide_developer_with_entry_not_consented_needs_consent();
    test_decide_developer_without_entry_loads();
    test_decide_user_with_entry_refused();
    test_decide_user_without_entry_refused();
    test_is_trusted();
    test_may_install_native();
    test_may_load_native();
    test_may_override();
    test_permissions_for();

    return genesis::test::summary();
}