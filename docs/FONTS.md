# Fonts

Every face the UI draws with, where it comes from, and under what licence.

| Role | Face | Where it lives | Licence |
|---|---|---|---|
| Body, footer, hints | SCE-PS3 Rodin LATIN | **Loaded from the console at runtime** (`/dev_flash/data/font/SCE-PS3-RD-R-LATIN.TTF`). Not in this repository or the package. | Sony system font. Loading it from flash is fine; shipping it is not, so it is never embedded. |
| Titles, card titles, wordmark | Open Sans Bold (ASCII subset, 8.9 KB) | `src/fonts/opensans_bold.h` | Apache 2.0 — [`fonts-licenses/Apache-2.0.txt`](fonts-licenses/Apache-2.0.txt) |
| Numbers: PIN digits, stat values | Michroma (U+0020–U+005A subset, 7.5 KB) | `src/fonts/michroma.h` | SIL OFL 1.1 — [`fonts-licenses/OFL-Michroma.txt`](fonts-licenses/OFL-Michroma.txt) |
| Icons | Material Icons (12-glyph subset, 2 KB) | `src/fonts/materialicons.h` | Apache 2.0 — [`fonts-licenses/Apache-2.0.txt`](fonts-licenses/Apache-2.0.txt) |
| Fallback | 8×8 bitmap font | `src/ui.c` | part of this project |

Fallback chain: if an embedded face fails to load, its text is drawn in
Rodin; if Rodin cannot be opened either, everything is drawn in the 8×8
bitmap font. Each failure is a log line, never an error screen.

## Why Open Sans Bold and not Satoshi

The design brief asked for Satoshi Bold for headings, on the condition that
its licence clearly allows redistribution inside a GPL binary. It does not:
Satoshi ships under the Indian Type Foundry Free Font License, whose terms
are only pointed at (`https://fontshare.com/terms`) and which restricts
redistributing the font data. Embedding it as a C array in a public source
tree is redistributing the font data, so it is not bundled.

Open Sans Bold is a humanist sans in the same family of shapes as Frutiger
(the face the "Frutiger Aero" look is named after), and its Apache 2.0
licence is unambiguous. Swapping it later is one header and one line in
`ui_fonts.c`.

## Never bundled

Rodin (Sony), Microgramma (Linotype/Monotype), GT America (Grilli Type),
Mata (no licence statement).

## Regenerating the subsets

`pyftsubset` (fonttools) with `--layout-features= --no-hinting`, the
codepoints above, then converted to a C array. The icon face maps the ASCII
letters `a`..`l` to the Material codepoints listed in `src/ui_fonts.c`.
