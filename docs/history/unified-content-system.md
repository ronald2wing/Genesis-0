# Unified Content System (Historical Record)

> Status: historical. This plan was executed (with a naming reversal, noted below). It is a record
> of intent, not a description of the current tree. Do not read as current design.

Scope: redesign the two parallel systems (declarative **packs** under
`src/effects/` + `src/render/Catalogue.*`, and **native extensions** under
`src/extensions/`) into one system with one manifest, one store,
one discovery path, one trust model, and declared contributions for every
content type.

> **Naming reversal (applied after execution).** This plan originally named the
> unified unit a "content package" and the `Content*` classes. That was reversed:
> the single installable unit is now called an **extension** (matching the
> product's `extensions.*` surface), and the `kind` field that split "native"
> vs "content" vs "pack" was removed entirely — the presence of `entry` in the
> manifest is the one signal for "carries native code". A media pack is simply
> an extension that ships assets. The `Content*` class names reverted to
> `Extension*` (`ExtensionHost`, `ExtensionsController`, `ExtensionManifest`,
> `ExtensionRecord`); `docs/content-system.md` reverted to `docs/extensions.md`.
> The mechanical work (manifest merge, trust table, `[[effect]]` blocks, scan
> dedup, `from_records`) survives unaltered; only the *name* changed.

The current contract lives in [`docs/extensions.md`](../extensions.md); the
still-open capability to replace core behavior is designed in
[`docs/proposals/level-3-replace-behavior.md`](../proposals/level-3-replace-behavior.md).
The pre-reversal stage-by-stage plan body is retained in git history and is not
reproduced here.