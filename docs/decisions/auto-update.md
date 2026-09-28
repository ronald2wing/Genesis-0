# Genesis-0 — Auto-Update Decision

Status: Implemented — Linux notify+confirm v1; macOS/Windows deferred. Checked: 2026-10-04.

Date: 2026-09-30.

Deciders: pending project owner.

Scope: whether the application updates itself, what it verifies before applying bytes, and which
platform channels own updates. This record does not design an updater; it fixes the policy the
implementation is held to.

## Terminology (enforced)

- **Update** — replacing the running application with a newer build.
- **Platform channel** — the mechanism that already owns application updates on a platform: the
  package manager and distro repository on Linux, Flatpak for a Flatpak build, an app store on
  mobile, or nothing for a portable build.
- **Self-updater** — application code that downloads an application package and installs it over
  itself.
- **Notify-only** — the application checks for a new version and tells the user, and hands off to
  the platform channel; it downloads and applies nothing.

*Plugin* is not used for product features (see `docs/architecture.md` §5, R6); it appears here only
where the platform's own technical term is meant.

## 1. Context

Genesis-0 has no updater today and no packaging format selected (`docs/decisions/packaging.md`).
The question is forced now because the reference editor — the system being re-derived — ships a self-updater, and
the rewrite must decide whether to carry it, replace it, or drop it. Two facts make this more than a
UX question:

1. The application will be GPL-3.0-or-later, and its distribution is already gated on the
   platform channel (`docs/architecture.md` §16; `docs/decisions/packaging.md:14-27`). An updater
   that bypasses the channel can defeat the licence and store reasoning the packaging decision
   rests on.
2. The reference editor's updater verifies a **hash, not a signature**, and fetches both the manifest and the
   bytes over HTTPS from the same origin (the reference host crate's `src/updates.rs:7-24,37,327,380`). That is
   weaker than the signature-and-trusted-identity pipeline Genesis-0 already specifies for
   Extensions (`docs/architecture.md` §11, §13). The source of an application update is a
   higher-value target than a single Extension, so the weaker bar is the wrong default.

## 2. What the reference editor does today (evidence)

The reference editor's update path is the reference host crate's `src/updates.rs`:

- Releases are read from the GitHub API for the reference editor (`updates.rs:37,45`).
- Each release carries a `manifest.json` naming a per-platform package with a URL, a size, and a
  SHA-256 (`updates.rs:7-15,327,340-378`). Bytes are checked against the manifest digest before use.
- Only releases at or after `FIRST_SWITCHABLE` (`0.2.5`) are offered (`updates.rs:41,141`).
- The install action is split by platform (`updates.rs:210-238,269-291`): Windows runs an installer;
  macOS copies the app out of the DMG over the bundle; an AppImage overwrites the file it was
  started from; a `.deb`, `.rpm` or pacman package is handed to the desktop's installer; a Flatpak
  is left to Flatpak; store and mobile builds are fixed; a distro package or a build-from-tree is
  not self-updated. `relaunch` starts the new one when the platform needs the old process gone.

The model downloader (the reference host crate's `src/models.rs`) is the stronger pattern and is the one worth
copying: no weights ship in the bundle, downloads happen on demand, every row carries a SHA-256 and
a download without one is refused, Hugging Face upstreams are pinned to a commit, bytes land in a
`.part` file and are renamed only after the digest passes, and the mirror is tried before upstream
(`models.rs:12-23,39-56,185-195`).

**What is not claimed.** Genesis-0 has no updater and no release channel. Everything in §2 is
The reference editor's behaviour, cited as the precedent being decided about — not Genesis-0 capability.

## 3. Decision(s)

### (a) No self-updater; the platform channel owns updates

The application never downloads an application package. Linux distro packages, Flatpak, and app
stores update the app; a portable or build-from-tree copy updates by replacing it.

- **Cost.** Lowest. No update code, no update threat surface, no signing of application packages
  beyond what each channel already requires.
- **Benefit.** No origin can push an application binary into a Genesis-0 install except the channel
  the user already trusts. The GPL and store reasoning in packaging stays intact.
- **Cost.** A portable build has no update path at all; the user must fetch a new download.

### (b) Notify-only

The application checks a release manifest, compares versions, and tells the user a newer build
exists, with a link to the channel (or a "copy new AppImage over this one" instruction). It
downloads and applies nothing.

- **Cost.** One network call, one manifest parser, one notification surface. Small.
- **Benefit.** A portable build gets discoverability without a self-install path; the channel still
  applies bytes. A compromised check origin can only misinform, not install.
- **Cost.** The user leaves the app to update; stale installs persist longer.

### (c) Self-updater, hash-verified (the reference editor's model)

Download the platform package, check it against a manifest SHA-256, apply it, relaunch.

- **Cost.** All of §2: per-platform install actions, relaunch handling, a downloader, a release
  pipeline, and an update surface that must never half-apply.
- **Benefit.** One-click updates on channels that have no better mechanism.
- **Risk that decides it.** HTTPS plus a same-origin manifest is not a signature. A compromised
  release account can serve a manifest and matching bytes. If Genesis-0 carries (c), it must add a
  detached signature over the manifest ("expected digest, signed by a key whose public half ships
  with the app"), or it is strictly weaker than its own Extension pipeline.

### (d) Hybrid by channel (recommended)

The channel decides, and the application does the least that channel allows:

- A distro package, a Flatpak, or a store/mobile build: **no self-update** (option a); the channel
  owns it. The application may run (b) and label the build "updated by your system".
- A clearly portable build (one self-contained file, no installer): **notify-only** (b), or
  self-update only from a signed manifest (c plus signature).
- Any self-update path that does exist requires a **signature over the manifest**, not a bare hash,
  before bytes are applied. A bare hash is acceptable for models (`models.rs`), where provenance is
  lower-value than an application binary, but not for the application itself.

## 4. What "verified" means before bytes are applied

Genesis-0 already distinguishes two jobs, and the updater must not blur them:

| Property | Extension pipeline (`docs/architecture.md` §11, §13) | Application update (this record) |
|---|---|---|
| Integrity | content digests | content digests (SHA-256) |
| Origin | signature + trusted identity | **must match the Extension bar** — signature, not hash |
| Consent | permissions re-consent on update | release notes + user-initiated apply |
| Layout | safe extraction, atomic versioned install, rollback | atomic replace, keep old until new runs |

An application binary is the most privileged artifact the project ships. The Extension pipeline's
own rule — "a signed package is not 'safe'; a process boundary alone is not a sandbox"
(`docs/architecture.md` §10) — applies with more force to the app itself. The decision is therefore
not "hash or signature"; it is "signature, or do not self-update."

## 5. Licensing consequence

`docs/decisions/licensing.md` is the register of record for the licence decision: first-party and
incorporated reference code are GPL-3.0-or-later, and linked/combined works must be GPL-3.0
(`docs/architecture.md` §16; `docs/decisions/packaging.md:14-24`). App stores are a hard gate for
the same copyleft reason (`packaging.md:25-27`).

Two consequences for an updater:

1. **A self-updater must offer corresponding source, and must not silently move the user to a build
   the user did not choose.** GPL is a copyleft with a source-offer obligation on distribution; an
   opaque auto-update channel that installs a build the project has not published as source
   contradicts the licence's intent and the store analysis the packaging record rests on.
2. **Channel-owned updates (a) are the licence-clean default.** The distro, Flatpak, and store
   already handle the source offer for the build they ship.

This record is downstream of the licensing decision and does not restate it.

## 6. Consequences

- If (d) is chosen: the host gains a `notify` path and a per-channel policy table, and the "apply"
  capability exists only behind a signed manifest and only for portable builds.
- If (a) is chosen: no update code, and the portability story must say "download a new build".
- The Extension update pipeline (§11) already repeats compatibility, permissions, and re-consent;
  the application updater must not be given a shortcut around a stronger pipeline that already
  exists next to it.
- A release must be able to describe itself offline — version, channel, build kind — for the app to
  know whether it may self-update at all. This is a small addition to the build metadata, not a new
  subsystem.

## 7. Revisit triggers

1. A portable, installer-free build becomes a supported first-class channel, making (b)/(c) matter.
2. A release-signing key and rotation policy exist (`docs/history/implementation.md:188`,
   `docs/history/rewrite.md:485-491`), which is the prerequisite for any (c) that is not weaker than
   §11.
3. A platform channel proves unable to deliver updates within a usable cadence, forcing (c).
4. The `docs/decisions/licensing.md` body is written and states an obligation the updater design
   must satisfy.
5. Flatpak (or another channel) is selected for a platform, fixing that platform to (a).

## 8. Open questions for the owner

1. **Does any platform ship a portable, channel-free build** for which an update path is expected,
   or is every build owned by a platform channel (making (a) sufficient)?
2. **Is a signed application update in scope at all**, or is a signature requirement accepted as a
   reason not to build one?
3. **May the application make an outbound version check** (notify-only) on first run and
   periodically, or must it stay silent unless the user asks? This is a privacy/consent call.
4. **Who holds the release signing key** and where does it live (`docs/history/implementation.md:188`
   says external, mechanism unset)?
5. **The `docs/decisions/licensing.md` body records the licensing decision.** Many documents cite
   it; confirm the GPL-3.0-or-later consequences recorded in `docs/architecture.md` §16 and
   `docs/decisions/packaging.md` stay the register of record.

## 9. Evidence index

| Fact | Location |
|---|---|
| No updater, no packaging format selected | `docs/decisions/packaging.md` |
| The reference editor's update semantics: GitHub releases, manifest + SHA-256, `FIRST_SWITCHABLE`, platform split, relaunch, no signature | the reference host crate's `src/updates.rs:7-24,37,41,210-238,269-291,327,340-380` |
| Model download is the stronger, hash-mandatory, mirror-first pattern | the reference host crate's `src/models.rs:12-23,39-56,185-195` |
| Extension install pipeline: signature + identity + digests, atomic, rollback, re-consent | `docs/architecture.md` §11 (`:222-234`), §13 (`:248-253`) |
| A signed package is not "safe"; process boundary is not a sandbox | `docs/architecture.md` §10 (`:211-220`) |
| Pack trust: `execute` only, User origin refused | `src/extensions/Trust.cpp:8-30`, `Trust.h:26,44-54` |
| Atomic install, side-by-side versions, staging | `src/extensions/Install.h:16-36` |
| GPL-3.0-or-later consequences (first-party + reference GPL; stores a gate) | `docs/architecture.md` §16 (`:269-281`); `docs/decisions/packaging.md:14-27` |
| Licensing register | `docs/decisions/licensing.md` |
| Release hardening: signed releases, keys external | `docs/history/implementation.md:182-201`; `docs/history/rewrite.md:485-491` |
| App stores widely considered GPL-incompatible | `docs/architecture.md` §16; `docs/dependencies.md` §"Build-time vs runtime dependencies" |

See `docs/decisions/packaging.md` for the channel constraints this record sits under, and
`docs/architecture.md` §11/§13 for the update pipeline the application updater is compared to.

## 10. Implementation reconciliation (2026-10-03)

The **host core is implemented** behind `GENESIS_UPDATER` (default **OFF**; ON requires OpenSSL
3.x), in `src/workspace/update/`: a signed release manifest (`Manifest.{h,cpp}`), Ed25519
verification via OpenSSL EVP (`Signature.{h,cpp}`), streamed download with a digest check, staged
install, atomic swap, and `confirm`/`rollback` (`Updater.{h,cpp}`). Gated OFF, a default checkout
compiles the sources but reports `disabled` — the verifier resolves to `Unavailable` and every
install is refused before a byte is fetched. The crypto-free `updater` and `apply` suites run in the
default build; `updater_crypto` (Ed25519 sign/verify, seeded in-process, no key on disk) is added
only with `GENESIS_UPDATER=ON` and OpenSSL.

The app now **checks and applies**. The `updates` surface is both notify and explicit-confirm
self-update (option (b), plus (c)/(d) behind a signed manifest):

- **A notify + explicit-confirm apply surface exists.** `UpdateController` is projected to QML as the
  `updates` context property and bound by `Settings` -> `Updates`: "Check now" fetches the release
  feed, verifies the signed manifest, and tells the user whether a newer build exists; "Download &
  install update" (enabled only once a check found a newer signed version) calls `apply(true)`.
  `confirm` is the user's explicit consent to download and replace the running install, mirroring the
  consent gates on extensions and AI model downloads; a false value refuses before a byte is
  fetched. The same call is exposed over the host API as the `updates.apply` verb.
- **The apply flow (Linux v1) is the helper-swap.** `src/workspace/update/Apply.{h,cpp}` backs up the
  running install, moves the verified staged version in, writes the `current`/`backup` markers, then
  relaunches from a detached helper that runs once the parent exits. A rename failure during the swap
  restores the backup before returning, so a half-applied install is never left behind;
  `rollback(root, target)` restores the backup and clears the markers. A single-file AppImage swap is
  the same rename, but v1 assumes the directory-tree install layout.
- **Anti-downgrade / anti-replay protection is enforced by a monotonic version plus a persisted
  record.** The updater keeps a `last-seen-version` marker in the update root: the newest version
  that has actually run. A manifest whose version is **older than or equal to** that record is
  refused before a signature is checked or a byte is fetched — an older version reads as a
  downgrade, an equal one as a replay. The check gates both the install pipeline
  (`src/workspace/update/Updater.cpp`) and the apply gate (`src/workspace/update/Apply.cpp`), and
  `confirm()` advances the record once the swapped-in version has run. On a fresh install the record
  is seeded from the running application version where a caller supplies it (the `install` entry
  takes an optional `running_version`), so the running version is never silently regressed even on
  the first check. The comparison is the one saturating `compare_versions`, moved to
  `src/workspace/update/Version.h` (the app re-exports it as `genesis::app::compare_versions`) so
  the notify-only surface and the apply gate share a single implementation.
- **No release key minted.** The embedded `kReleasePublicKey` is all zeros, so even an enabled build
  verifies nothing until a key exists (revisit trigger 2).

Remaining follow-ons and limits, kept visible:

- **macOS/Windows installer flows.** v1 swaps a directory tree in place (the portable Linux layout).
  The per-platform install actions the reference editor splits on (Windows installer, macOS bundle copy over the
  DMG, `.deb`/`.rpm`/pacman hand-off, Flatpak) are not implemented; a non-Linux or channel-owned
  build stays notify-only and the channel applies the bytes.
- **Timestamp/nonce binding remains future work.** The signed payload (`version`, `url`, `sha256`)
  carries no timestamp or nonce, and the manifest's `notes` field is unsigned informational text, so
  there is no signed freshness to bind against: equality of the signed version is the replay signal,
  and a re-served older manifest (whose version is fixed by its signature) is caught by the version
  check. A content-hash seen-set is not adopted because there is no signed date/nonce field to
  anchor one and the version string is the only signed monotonic signal; binding a fresh timestamp
  or nonce would require extending `Manifest`/`signed_payload()`, a breaking change to the on-disk
  contract, and is deliberately deferred.
- **Rollback is the explicit escape hatch, not a silent downgrade.** A deliberate move back to an
  earlier build is the operator-initiated `rollback()` (install path) / `rollback(root, target)`
  (apply path), which is distinct from the attacker-driven downgrade the monotonic gate refuses.
  Rollback does not lower the `last-seen-version` record, so it reopens no automatic downgrade
  window.

The apply path exists only behind a signed manifest and an explicit user confirmation; the
channel-owned default (a) still stands for distro/Flatpak/store builds, and any self-update remains
held to the signature-over-manifest bar of §4.
