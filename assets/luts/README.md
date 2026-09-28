# Shipped LUT data — provenance

The `.cube` files in this directory are **original Genesis-0 work**, created
in-house for Genesis-0. No third-party tables are involved and no third-party
rights attach.

- **Source of truth:** the committed files in this directory. Edit them directly
  if changes are needed.
- **Format:** Adobe `.cube` 3D LUT interchange format — a `LUT_3D_SIZE` header,
  `DOMAIN_MIN`/`DOMAIN_MAX` pinned to the default 0..1, then `size^3` RGB
  triples with red changing fastest, then green, then blue. Each table is a
  17^3 grid (4913 rows), the standard Adobe default.
- **Counts:** 8 look-up tables, each 17^3.

The tables are read through `src/render/LutLoader`.

## The eight looks

| File | Look |
| --- | --- |
| `teal_orange.cube` | Split-tone: teal shadows with orange highlights. |
| `bleach_bypass.cube` | Desaturated high-contrast bleach-bypass with bright highlights. |
| `filmic_contrast.cube` | Filmic S-curve contrast with smooth toe and shoulder. |
| `warm_daylight.cube` | Warm white balance: red gain, blue cut. |
| `cool_moonlight.cube` | Cool moonlit cast: blue gain with mild desaturation. |
| `vintage_fade.cube` | Faded vintage: lifted blacks, warm sepia, soft saturation. |
| `bw_high_contrast.cube` | Hard black-and-white from luma with steep contrast. |
| `vibrant_pop.cube` | Vibrant pop: boosted saturation and punchy contrast. |

Every look is a total function `[0,1]^3 -> [0,1]^3`, so black and white stay in
range and the grid can be sampled without surprises.
