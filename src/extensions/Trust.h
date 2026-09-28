// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include "effects/Pack.h"

namespace genesis::extensions {

// Where an Extension came from. R7/R8: what an Extension may do follows from
// where it came from.
//
// The four origins, and the reading of R7/R8 each one comes from:
//
// - Builtin: first-party code that ships inside the host binary. It is the
//   host's own code, so it is trusted.
// - Curated: from the signed marketplace (R7: "a signed, versioned pipeline
//   with compatibility, consent, atomic install, health check, rollback"). It
//   passed that review, so it is trusted.
// - Developer: unsigned, local-only, from a developer's own machine
//   (architecture.md §11: "developer exceptions are visibly separate"). It is
//   visible in the store and may be loaded, but only after the user records
//   consent for that extension's id (see Consent.h) - it never went through the
//   signed pipeline, so the user must opt in explicitly.
// - User: installed from a file that did not go through the marketplace
//   pipeline and has no consent record. No signature, no review, no consent -
//   so it is refused outright.
enum class Origin { Builtin, Curated, Developer, User };

// Whether a Pack from this origin may be installed. Reads R7/R8 - do not invent
// policy; where the rules are silent, take the conservative reading and say so.
//
// Builtin and Curated are trusted; Developer and User are not. The conservative
// reading is that a Pack that bypassed the signed pipeline (Developer included
// - consent is a native-extension concept, and a Pack has no load step to gate)
// is refused rather than installed with reduced permissions.
bool is_trusted(Origin origin);

// Whether a native extension from this origin may be installed into the store.
// Builtin and Curated install freely; Developer installs "visible" - unsigned,
// local-only, and load-gated by recorded consent; User is refused.
bool may_install_native(Origin origin);

// Whether a native extension from this origin may be loaded into the process,
// given whether its id is on the developer-consent list. Builtin and Curated
// load freely; Developer loads only with recorded consent (`consented` is the
// caller's already-checked answer); User is refused.
bool may_load_native(Origin origin, bool consented);

// The operations a Pack from this origin is allowed, derived from the origin
// and the manifest. The sandbox that enforces this is T3's business; this is
// only the host's own policy, applied at install time.
//
// A format-2 Pack is self-contained: a manifest plus a shader the host compiles
// and runs. It never declares a filesystem or network capability, and the shader
// "cannot do arbitrary IO" (architecture.md §9), so `filesystem` and `network`
// are granted to no origin. The one capability a Pack exercises is `execute`:
// contributing the shader itself. That is granted to the origins whose shaders
// are the host's own or have been through the R7 review (Builtin and Curated),
// and withheld from Developer and User.
struct Permission
{
    bool filesystem = false;
    bool network = false;
    bool execute = false; // may contribute a shader/program at all
    bool operator==(const Permission &) const = default;
};
Permission permissions_for(Origin origin, const genesis::effects::Pack &pack);

} // namespace genesis::extensions
