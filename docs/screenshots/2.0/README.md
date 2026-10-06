# 2.0 screenshots

Captured from the RPCS3 emulator with `preview.txt` forcing each screen (see
[docs/ui-notes.md](../../ui-notes.md)). Files are `<mode>-<screen>.png`.

| Mode | Emulator setting |
|---|---|
| `1080p-16x9` | 1920x1080, 16:9 |
| `720p-16x9`, `720p-4x3` | 1280x720, 16:9 / 4:3 |
| `576-16x9`, `576-4x3` | 720x576, 16:9 / 4:3 |
| `480-16x9`, `480-4x3` | 720x480, 16:9 / 4:3 |

Not covered: 1080i, 576i and 480i (the emulator has no interlaced modes; they
use the same canvas as their progressive twins). RPCS3 shows the 16:9 SD modes
at their stored shape instead of stretching them to 16:9, so circle roundness on
anamorphic output is not something these captures can show. The 4:3 SD captures
sit inside the default 90% x 92% overscan, which is why they have a border. The
`stream` captures are the stats overlay with no video behind it.
Everything here still needs a look on a real TV.
