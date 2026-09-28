// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include "effects/Pack.h"

namespace genesis::extensions {

// Where an Extension came from. R7/R8: what an Extension may do follows from
// where it came from.
//
// An origin is a LOCAL trust label, recorded in the store when the extension
// is installed or seeded, and read back at scan time. It is not
// cryptographically verified: the machine owner (or a curator acting on it)
// applies it. It governs the consent gate for native code, not a proof of
// provenance.
//
// The four origins:
//
// - Builtin: first-party code that ships inside the host binary. It is the
//   host's own code, so it is trusted. Applied only by the seed step, never by
//   a user install.
// - Curated: a classification a curator applies to an install they have
//   vetted. It loads native code without a consent prompt - a local trust
//   decision made at install time, not a signature check. The signed
//   marketplace (R7) is a future mechanism that would back this label with a
//   verified catalog signature; until then "curated" and "developer" differ
//   only in that curated skips the consent prompt for entry.
// - Developer: unsigned, local-only, from a developer's own machine
//   (architecture.md §11: "developer exceptions are visibly separate"). It is
//   visible in the store and may be loaded, but only after the user records
//   consent for that extension's id (see Consent.h) - it never went through the
//   signed pipeline, so the user must opt in explicitly.
// - User: installed from a file that did not go through the marketplace
//   pipeline and has no consent record. No signature, no review, no consent -
//   so it is refused outright.
//
// Origin is a curation/consent label, NOT a security boundary for privileged
// capabilities. Anything that grants the power to replace core app behavior
// (see docs/proposals/level-3-replace-behavior.md §5) must gate on a verified
// catalog signature, not on an Origin value - an Origin is self-applicable by
// the machine owner.
enum class Origin { Builtin, Curated, Developer, User };

// The one policy decision for an extension. `consented` is the caller's
// already-checked developer-consent answer (Consent.h); it is only consulted
// for Developer. `has_entry` is the signal for a native extension (one that
// ships a shared library): when true, the extension carries native code and
// the trust gate applies; when false, it is data-only and loads freely
// (Builtin/Curated/Developer data loads; User data is still refused).
enum class Decision { Load, Refuse, NeedsConsent };

// The unified trust gate. Builtin and Curated are trusted, full stop. Developer
// with entry needs consent; Developer without entry (data-only) loads freely.
// User is refused regardless of entry presence. `has_entry` is the ONE signal
// for "carries native code" — there is no package-level kind differentiation.
Decision decide(Origin origin, bool has_entry, bool consented);

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

// Whether a native extension from this origin may declare an `overrides` entry
// (replace another extension's menu action / panel / dialog / effect pack).
// Overriding is a trust-gated capability: Builtin and Curated may override
// freely; Developer may override under recorded consent because overrides are
// declarative/QML/asset swaps, not arbitrary host-code execution (Level 3
// replacement is a separate, stricter gate). User is refused.
bool may_override(Origin origin);

// The operations a Pack from this origin is allowed, derived from the origin
// and the manifest. The sandbox that enforces this is T3's business; this is
// only the host's own policy, applied at install time.
//
// A Pack is self-contained: a manifest plus a shader the host compiles
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
