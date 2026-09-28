# UI icons

Chrome icons for the app's controls, bundled into the `genesis_app` binary via
`qt_add_resources` (prefix `/icons`, so QML refers to them as
`qrc:/icons/<name>.svg`). They are rendered with `Image { sourceSize: ... }`.

## Colour

The stroke colour is **baked into each file** rather than tinted at runtime.
Runtime tinting (`MultiEffect`/`ColorOverlay`) does not render under the
software-GL (llvmpipe) path the smoke gate uses, so a tinted icon would be
invisible in CI and on any machine without a working GL stack. Baking the
colour keeps the icons correct everywhere at zero runtime cost.

The palette is the dark-theme `Theme` ink:

- `#e8e8ee` — `textPrimary` (transport rest state)
- `#5a5a68` — `textDisabled` (transport disabled state, `-disabled` suffix)
- `#9a9aa8` — `textSecondary` (toggle unchecked)
- `#ffffff` — `onAccent` (toggle checked, `-checked` suffix)

A state that needs a different colour ships as a suffixed variant
(`undo-disabled.svg`, `snap-checked.svg`); the component picks the file.

## Sources

- `play.svg`, `pause.svg`, `undo.svg`, `redo.svg`, `delete.svg`, `split.svg`
  are from [Lucide](https://lucide.dev), used under the ISC License (text
  below).
- `snap.svg`, `ripple.svg` are authored in-house for this project and carry the
  repository's GPL-3.0-or-later license, matching the rest of the tree. They
  follow Lucide's 24x24 grid and 2px stroke so the two sets sit together.

## Lucide ISC License

Copyright (c) for portions of Lucide are held by Cole Bemis 2013-2022 as part of
Feather (MIT). All other copyright (c) for Lucide are held by Lucide
Contributors 2022.

Permission to use, copy, modify, and/or distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright notice
and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.
