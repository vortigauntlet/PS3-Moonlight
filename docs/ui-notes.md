# UI notes

How the 2.0 menus are put together, and how to look at them without a console.

## Files

| File | Role |
|---|---|
| `ui.c` | state, settings storage and wording, public API, the UI thread |
| `ui_layout.[ch]` | the logical canvas, safe rect, shelf maths (pure, host-tested) |
| `ui_theme.[ch]` | palette, type scale, timings, Day / Night (Dark Aero) |
| `ui_fonts.[ch]` | four faces in one texture block, advance tables, `ui_text*` |
| `ui_draw.[ch]` | rounded rects, glass, glow, spinner, pill, logo, easing |
| `ui_bg.[ch]` | sky, light shafts, two wave ribbons, bubbles |
| `ui_screens.[ch]` | one draw function per screen, plus the shared chrome |
| `ui_internal.h` | what `ui.c` shares with `ui_screens.c` |

## Canvas

Logical height is 720; width is 1280 (16:9 outputs) or 960 (4:3). Logical to
physical is `px = lx * width / LW`, `py = ly * height / 720`, which keeps
circles round on anamorphic SD because the TV stretches the pixels back.
Margins are 64 px left and right, 40 top and bottom. Nothing outside
`ui_layout.h` and `ui_layout.c` assumes 1280.

`make test` builds `tests/test_layout.c` with plain gcc and checks, for 1080,
720, 576 and 480 in both shapes, that shelf cards stay inside the safe row, the
focused card is never partial, and a 63-character name fits its tile after
ellipsizing.

## Text

libfont rasterises each glyph once into a fixed cell and scales it. Each face
is rasterised at `output height / 720 x its most-used logical size`, capped at
three quarters of its cell, and drawn with `size = cell x target / raster`.
libfont samples 95% of a cell across its quad, which `ui_fonts.c` folds in.
Advances are kept in our own tables, so `ui_text_width` and `ui_text_fit` are
exact for the sizes drawn.

## Look

Frutiger Aero (glass, aqua sky, bubbles, an XMB wave) with Dark Aero as the
night theme: near-black blue glass, a cold cyan edge on the focused item,
diagonal sheen, light shafts. The logo is the project's ring-and-spokes mark,
drawn from its SVG geometry.

## Previewing screens

Put a `preview.txt` in `/dev_hdd0/game/MNLT00001/USRDIR/` (under RPCS3's
`dev_hdd0` for the emulator) to force a screen with fake data. Nothing touches
the network in this mode.

```
state=home|discovery|settings|pairing|applist|error|streaming
running=<app id>
pin=4719
error=<host message>
apps=Name one|Name two|...
```

Delete the file to return to normal operation.
