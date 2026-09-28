# Genesis-0 — Licensing Decision Register

Status: **recorded**. Checked: 2026-10-01.

## Decision

First-party Genesis-0 code is licensed **GPL-3.0-or-later** — SPDX headers in
`src/`, `tests/`, `extensions/builtin/`, and `spikes/`, and the full text in
[`LICENSE`](../../LICENSE).

## Rationale

- Genesis-0 is a desktop application; a network-source obligation is
  unnecessary.
- Shipped assets (LUTs, title SVGs, effect packs) are generated in-house, so no
  third-party rights constrain the choice.
- A plugin/linking exception can be added later if closed local extensions
  become needed.

## Consequences

- Copyleft applies to linked/combined works, so a closed-source/paid Extension
  marketplace is not permitted by default.
- Third-party dependencies keep their own licenses (Qt LGPL/module terms,
  GStreamer/GES LGPL, FFmpeg, etc.); GPL-3.0-or-later does not override them.

This is a product/engineering record, not legal or store approval; qualified
counsel review remains a gate.
