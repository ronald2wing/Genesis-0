# Title templates

These SVG title templates are **original Genesis-0 artwork**, created in-house
for Genesis-0. No third-party artwork is involved and no third-party rights
attach.

- **Source of truth:** the committed files in this directory. Edit them directly
  if changes are needed.
- **Format:** SVG, 1920x1080. Every editable slot is one literal `<text>`
  element (a real `</text>` close, never self-closed), styled with presentation
  attributes only — `font-family`, `font-size`, `font-weight`, `fill`,
  `text-anchor`.
- **Counts:** 20 templates across 8 design families, 47 editable slots in
  total.

## Families

| Family | Templates |
| --- | --- |
| Lower-Third Bar | `lower_third_bar_left`, `lower_third_bar_center`, `lower_third_bar_kicker` |
| Centered Minimal | `centered_minimal`, `centered_minimal_light`, `centered_minimal_line` |
| Glass Card | `glass_card_center`, `glass_card_left`, `glass_card_kicker` |
| Full-Width Band | `full_width_band`, `full_width_band_dual` |
| Kicker Stack | `kicker_stack_center`, `kicker_stack_left`, `kicker_stack_rule` |
| Split Title | `split_title`, `split_title_accent` |
| Side Rail | `side_rail_left`, `side_rail_right` |
| Gradient Accent | `gradient_accent_top`, `gradient_accent_band` |

## What the extractor reads (`src/render/TitleTemplates.{h,cpp}`)

Each `<text>` element becomes one `TitleSlot`. The following SVG attributes map
to `TextStyle`:

| SVG attribute | `TextStyle` field |
| --- | --- |
| `font-family` | `font_family` (quotes normalised to double quotes) |
| `font-size` (px) | `font_size` (px / 1080, the reference frame height) |
| `font-weight` (bold/normal/number) | `font_weight` (100..=900) |
| `font-style` (italic) | `italic` |
| `fill` (`#rgb`/`#rrggbb`/`#rrggbbaa`) | `color` |
| `text-anchor` (start/middle/end) | `align` |

## What is dropped

`TextStyle` cannot express these, so they are ignored rather than approximated:

- **`fill:url(#...)` gradient fills** — dropped to the default colour.
- **Positioning** (`x`, `y`, `transform`) — `TextStyle` carries no position;
  placement is the caller's clip offset.
- **Strokes** (`stroke`, `stroke-width`, `stroke-opacity`) and
  `fill-opacity`/`opacity`.
- **Line metrics** (`line-height`, `letter-spacing`) and text layout hints.
- **All artwork** (rects, paths, gradients, filters, defs) — the SVG is never
  rasterised; only the text slots are extracted.

The scanner finds literal `<text>` tags in document order. It does not
understand comments, CDATA, or namespaces, and it treats every `<text>` element
as a slot. Each template is kept compatible with this scanner: no self-closing
`<text>`, no `<tspan>`, no `<textPath>`, and no comment containing the substring
`<text`.
