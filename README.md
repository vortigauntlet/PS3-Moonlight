# Moonlight PS3

Moonlight PS3 is an open-source PlayStation 3 homebrew client for NVIDIA GameStream and Sunshine servers, built using the PSL1GHT SDK, Tiny3D graphics engine, FreeType 2 vector font renderer, `cellVdec` hardware decoder interface, and Opus audio decoding pipeline.

---

## Features

- **Hardware-Accelerated H.264 Video Decoding**: High-performance 720p60 H.264 video decoding utilizing the PS3 Cell Broadband Engine `cellVdec` hardware decoder, mapped directly to RSX graphics memory via Tiny3D. Features 4-slice frame negotiation (`CAPABILITY_SLICES_PER_FRAME`) with Sunshine/GameStream to eliminate UDP packet bursts and socket buffer overflow.
- **Full USB & Bluetooth Keyboard and Mouse Support**:
  - **Dual Mouse Modes**: Supports both **Game Mode** (unbounded relative delta motion for 3D camera control) and **Desktop Mode** (high-precision 1:1 absolute coordinate motion for desktop navigation), configurable in the Stream Settings menu.
  - **Complete Mouse Input**: Full support for Left, Right, Middle, Side X1, and Side X2 buttons, alongside high-precision vertical scroll wheel input.
  - **Universal Keyboard Translation**: Accurate Win32 Virtual Key (VK) mapping for standard US layout keyboards (A-Z, 0-9, F1-F12, navigation cluster, arrows, numpad, and punctuation) with discrete modifier tracking (Ctrl, Shift, Alt, Meta/Win).
- **Interactive Host Game & Application Selection Menu**: Automatically queries and parses `/applist` XML from your Sunshine/GameStream server, displaying all available host games and applications in a scrollable Material UI list with active selection highlight, index counter `[ X / Y ]`, and one-click launching.
- **Native PS3 GameOS System Dialogs**: Official `<sysutil/msg.h>` pop-up confirmation modals ("Do you want to quit Moonlight and return to the PS3 XMB?") preventing accidental exits.
- **Persistent Configuration Engine**: Automatically saves and restores Sunshine Host IP, target FPS (30/60), resolution (720p/1080p), bitrate (2.5 - 30 Mbps), RTP packet size (1024/1392), mouse mode (Game/Desktop), RSX VSync mode, HUD stats visibility, and verbose logging across console reboots in `/dev_hdd0/game/MNLT00001/USRDIR/config.ini`.
- **High-Fidelity FreeType 2 Font Rendering**: Dynamically loads official PlayStation 3 console vector fonts (`SCE-PS3-RD-R-LATIN.TTF`, `SCE-PS3-SR-R-LATIN.TTF`) from `/dev_flash/data/font/` with subpixel anti-aliasing, accompanied by custom procedural vector PlayStation button glyphs (Cross ✕, Circle ◯, Triangle △, Square ◻, D-Pad ↕).
- **Zero-Overhead RSX Hardware VSync & Telemetry HUD**: Flexible flip mode configuration (`GCM_FLIP_VSYNC` for tear-free 60Hz or `GCM_FLIP_HSYNC` for ultra-low latency) paired with an on-screen Performance Stats HUD that introduces zero CPU/GPU overhead when disabled.
- **Low-Latency Opus Multistream Audio Backend**: Custom PS3 audio backend featuring thread-safe ring buffering, low-latency 48kHz PCM playback via `sysAudio`, and automatic buffer recovery.
- **On-Screen Pairing PIN Modal**: Clean, dedicated visual badge presenting the dynamically generated 4-digit PIN for instant authorization in the Sunshine Web UI.
- **Universal NPDRM Package Build Pipeline**: Native `ppu-strip`, `fself`, and `make_self_npdrm` integration generating retail-signed PKG files compatible with both RPCS3 emulator and physical PS3 consoles (CFW / HEN).

---

## PC (host) setup for 1080p60

**720p works with any host settings. 1080p60 needs one change on the PC.**

The PS3 decodes 1080p60 comfortably — about 31 ms per frame, measured — *but
only if the stream leaves out CABAC and the H.264 deblocking filter*, the two
most expensive parts of decoding on this hardware. Hardware encoders (NVENC,
AMF, QuickSync) always use both; TEE PS3 Remoteplay measured NVENC 1080p at
147 ms per frame on a PS3, so with default host settings 1080p60 cannot play.
x264's `fastdecode` tune switches exactly those two features off, and a
stronger preset than `ultrafast` buys back picture quality at the same cost to
the PS3.

### Quick way: the setup script

On the streaming PC, double-click `tools\setup-host.cmd` (double-clicking the
.ps1 itself only opens it in Notepad). It asks for your web UI login, then
applies the settings. Or, in PowerShell:

```powershell
.\tools\setup-host.ps1                  # asks for your web UI login, then applies it
.\tools\setup-host.ps1 -DryRun          # show what it would change
.\tools\setup-host.ps1 -Pacing 60000    # Vibepollo on Wi-Fi: also pace the sender
.\tools\setup-host.ps1 -Revert          # undo
```

It detects the host and applies only what that host supports. It changes nothing
else; on Sunshine, `-Revert` sets the encoder back to auto-detect.

### Manual settings

| setting | value | Vibepollo | Apollo | Sunshine |
| :--- | :--- | :--- | :--- | :--- |
| `encoder` | `software` | per client (Client Management → overrides) | global | global |
| `sw_preset` | `veryfast` | per client | global | global |
| `sw_tune` | `zerolatency,fastdecode` | per client | global | global |
| permissions | Launch, View, List, Controller, Mouse, Keyboard | **required** — new clients may only list and view | **required** | — |
| `pacing_max_bitrate_kbps` | `60000` (Wi-Fi hosts) | global, optional | — | — (send rate is fixed) |

"Global" means every client streams with software x264, not just the PS3.
`veryfast` at 1080p60 is a real CPU load on the host; if the PS3 log shows the
host sending fewer than 60 frames, try `superfast` or `ultrafast`.

### Client settings that go with it

1080p, 60 FPS (shown as 59.94 — the PS3's real output rate), **Decoder Output:
YUV420**, bitrate **40–50 Mbps**. The bitrate setting is not what arrives: the
client asks for 80% of it (the rest is FEC headroom), and Sunshine's x264 rate
control with its one-frame buffer delivers well under that. Measured on
Cyberpunk 2077: the 12.5 setting delivered ~5 Mbps; 50 delivered ~30 Mbps at a
steady 60 fps with no lost frames.

**A wired PC matters.** Every multi-second hitch in testing was the host's Wi-Fi
stalling (throughput halves, then 30–60 frames never arrive and the stream waits
for a fresh keyframe). Nothing on the PS3 side can hide a link that stops.

### Vibepollo / Apollo extras

When the host advertises them, the client also uses: a virtual display created
at exactly the stream mode (`1920x1080x59.94`), launch by app UUID, the host's
own permission messages on the error screen, quitting the host app when you
leave a stream (PS button or the exit combo — a dropped connection leaves it
running so you can resume), and OTP pairing via
`/dev_hdd0/tmp/moonlight_otp.txt` (`<PIN> <passphrase>`). Plain Sunshine gets
the standard requests.

---

## Resolution

Selectable 960x544, 1280x720, 1792x1008 or 1920x1080, independent of the 30/50/60/120 FPS
picker (120 is for 720p and 960x544). Default is 720p, or 960x544 when the console
is outputting standard definition. The choice is saved as `stream_res=WIDTHxHEIGHT`
(the old `res_idx` is still read). 960x544 needs 57% less decode than 720p and is the
lowest-latency mode TEE PS3 Remoteplay measured; it always decodes to ARGB32, since
its chroma pitch (480) cannot be a 64-byte-aligned YUV texture. What each combination asks of the
decoder, as H.264 macroblock rate:

| mode | macroblock rate | H.264 level needed | Moonlight reference bitrate |
| :--- | ---: | :--- | ---: |
| 720p60  | 216,000/s | 4.1 | 10 Mbps |
| 1080p30 | 244,800/s | 4.1 (only just) | 10 Mbps |
| 1080p60 | 489,600/s | **4.2** | 20 Mbps |

The PS3 advertises Level 4.2, the Blu-ray maximum. 1080p60 holds on real
hardware **when the host encodes without CABAC and deblocking** — see
[PC (host) setup](#pc-host-setup-for-1080p60). With a hardware encoder on the
host it does not. The `dec=` field in the log (pictures decoded per second) is
the direct check.

Two things had to be fixed for any of this to take effect at all:

- The launch request hardcoded `mode=1280x720x60`, which is what the host uses
  to configure the game itself under `sops=1`. It now follows the picker.
- `vdecOpen()` was tried against a fixed level list `{41, 42, ...}` and stopped
  at the first success. Level 4.1 always succeeds, so 4.2 was never reached and
  the decoder was silently capped below 1080p60. The level is now chosen from
  the macroblock rate and tried as the outer loop, so the preferred level gets
  every memory size before falling back.

1080p also roughly doubles decoder-side memory (four 8 MB frame buffers instead
of four 4 MB ones, plus a larger VDEC working set), and the decoder gets 4 SPU
threads instead of 2 when the mode needs Level 4.2.

---

## Bitrate

The bitrate picker offers **2.5, 5, 10, 12.5, 15, 20, 25, 30, 40 and 50 Mbps**,
defaulting to **20 Mbps**. It is saved as `bitrate_kbps`; an older
`config.ini`'s `bitrate_idx` is still read.

20 is deliberately above Moonlight's reference for both supported modes (10 Mbps
for 720p60 and for 1080p30). The reference is a sensible default for unknown
hardware, not a ceiling, and this console's measured receive ceiling on a wired
LAN is 20-25 Mbps -- at 1080p30 that headroom is better spent on picture than
left unused.

Three things had to change before the steps above 10 Mbps were worth offering:

- **The libnet memory pool.** PSL1GHT's `netInitialize()` gives libnet a fixed
  128 KB pool shared by *every* socket on the console. A larger pool was tried
  and **measured worse** (a 4 MB pool halved receive throughput in the sister
  JellyFin-PS3 client), so the stock pool is the default; the size can be set
  in KB in `/dev_hdd0/tmp/moonlight_netpool.txt` for experiments. When the pool
  runs dry for a moment, libnet returns `ENOBUFS` (105) on *every* socket at
  once. That used to tear the stream down; it is now treated as a momentary
  shortage everywhere it can surface (poll, receive, ENet send/receive/wait).
- **The video socket receive buffer.** Sized from the largest *burst* rather
  than a time window. Sunshine sends each frame as a run of packets back-to-back
  at link rate, so the buffer has to swallow a whole frame before the receive
  thread gets a look in; for a game stream a deep buffer is latency, not safety,
  so what matters is fitting one IDR. Frame size is bitrate/fps, which means
  30 fps doubles the burst for the same bitrate: **1080p30 at 20 Mbps is a
  ~101 KB average frame and a ~406 KB IDR**, against the 64 KB this used to ask
  for. Simulated loss at that operating point with a loaded reader: 46% at
  64 KB, 5.0% at 256 KB, 0.6% at 512 KB, 0% at 1 MB.
- **RTP packet size.** Each packet costs one `recv()` syscall on the PPU, so at
  high bitrates it is the packet *rate* rather than the byte rate that saturates
  the receive thread. 1392 (Moonlight's standard MTU-safe size) moves 36% more
  payload per syscall than this port's original 1024. Both are selectable so
  they can be compared on real hardware.

### Periodic intra refresh

The client asks Sunshine for periodic intra refresh (`x-ss-video[0].intraRefresh`),
which replaces IDR frames with a band of intra-coded macroblocks that sweeps
across the frame over many frames. Without it, every IDR is a single burst about
4x the size of an average frame -- ~406 KB at 1080p30 and 20 Mbps -- and a
dropped packet triggers another IDR at exactly the moment the link is already
struggling. This is the technique Cell Stream uses for the same PC-to-PS3
workload, and Sunshine ignores the request if its encoder cannot do it.

It cuts the socket buffer needed for zero loss by 4x. Simulated at 1080p30 /
20 Mbps with a loaded reader:

| receive buffer | with IDR frames | with intra refresh |
| :--- | ---: | ---: |
| 64 KB  | 46.3% | 42.2% |
| 256 KB | 4.9% | **0%** |
| 512 KB | 0.6% | 0% |
| 1 MB   | 0% | 0% |

On by default. There is no room left on the settings page, so to turn it off set
`intra_refresh=0` in `/dev_hdd0/game/MNLT00001/USRDIR/config.ini`.

### Finding the ceiling on your console

The highest step that works is a property of your PS3 and your network, not of
this app — so measure it rather than assuming. Turn on **Stats Overlay** in
Stream Settings and watch two numbers:

```
Bitrate: 25.0 Mbps  (rx 24.7, sock 41 KB)
Dropped frames: 0
```

- `rx` tracking the selected bitrate with `Dropped frames` flat means the step
  is fine — try the next one up.
- `rx` falling short while frames drop means that step is over the ceiling.
  Come back down one.

The log is written to **`/dev_hdd0/tmp/moonlight_log.txt`** (pull it over FTP)
and also broadcast as UDP to port 18194 on the LAN, so you can watch it live.
It carries a once-per-second line with more detail:

```
[PS3-NET] rx=24.712 Mbps pkts=3061 buf=524288 rxq=8192 batch=9/16 enobufs=0 recv=182400 fecrec=3 fecfail=0 oos=0
```

| field | meaning |
| :--- | :--- |
| `rx` | throughput actually reaching the app |
| `buf` | receive buffer size the kernel accepted |
| `rxq` | bytes sitting unread in that buffer right now |
| `batch` | largest drain batch this second, out of `VIDEO_BATCH_SIZE` |
| `fecrec` / `fecfail` | packets FEC repaired / frames it could not repair |

`rxq` sitting near `buf` means the receive thread cannot drain the kernel fast
enough; `batch` pinned at its maximum means the drain loop itself is the limit.
`fecfail` climbing is the clearest sign you are past the ceiling.

Newer fields on the same line: `dec`/`shw`/`skip`/`drop` (pictures decoded,
shown, replaced before display, and discarded before decode, per second),
`dlat` (average decode latency), `rxt` (first packet to frame complete), `jit`
(arrival-gap deviation, mean/worst), `qd` (decode-unit queue depth) and `au`
(access units inside the decoder).

---

## Settings, controls and display

The Settings page scrolls and is grouped (Video, Network & Host, Controls,
Display); L1 / R1 jump a page. Options that used to be `config.ini`-only
(`low_latency`, `ntsc_rate`, `virtual_display`, `quit_on_exit`, `intra_refresh`,
`vdec_spus`) are now rows in it.

- **Rumble** (`rumble=1`): the host's rumble goes to the DS3 motors. The large motor
  follows the request; the small one is on/off (on at >= 25%). It is only
  commanded when the value changes, at most every 20 ms, and is silenced when the
  stream ends, the connection drops, the app quits, or the XMB opens.
- **Triggers** (`trigger_mode=1`): analog L2/R2 from the DS3's pressure sensors,
  enabled on every connected port. The digital L2/R2 bit decides "pressed";
  pressure only sets how hard (dead zone below 8, full at 230+). A pad that reports
  no pressure still gives a full-strength trigger. `trigger_mode=0` forces digital.
- **Host identity**: a pairing is keyed on the host's own `<uniqueid>`, not its IP,
  so a DHCP change or a rename does not need a re-pair. An existing IP-keyed pairing
  is migrated automatically on first contact. A saved host that stops answering is
  looked up on the LAN by its uniqueid and the saved address is updated.
- **Quit app on host**: the main menu shows *Quit <app> on host* while the host's
  `/serverinfo` reports something running (so it also works after a dropped
  connection or a client restart).
- **Overscan** (`overscan_x`, `overscan_y` in %, `overscan_xoff`, `overscan_yoff`):
  shrink and shift the whole picture, menus and stream together, so the edges
  survive a CRT or a TV that crops the signal. Defaults are 100% / 0, and 90% x 92%
  on an SD output.

---

## Controls & Key Bindings

| Action | Controller | Keyboard / Mouse |
| :--- | :--- | :--- |
| **Navigate Menus** | D-Pad Up / Down / Left / Right | Arrow Keys |
| **Select / Confirm / Launch** | Cross (✕) | Enter / Left Click |
| **Back / Exit to XMB (with Confirmation)** | Circle (◯) | Escape / Right Click |
| **Open On-Screen Keyboard (OSK)** | Cross (✕) on Host IP row | Physical Keyboard Direct Entry |
| **Toggle Settings Values** | Left / Right / Cross | Left / Right / Enter |
| **Emergency Stream Abort** | `Select + Start + L3 + R3` | `Ctrl + Shift + Alt + Q` |

---

## Quick Start & Building from Source

### 1. Clone the Repository
```bash
git clone https://github.com/Cruslan/PS3-Moonlight
cd PS3-Moonlight
```

### 2. Prepare the PS3 SDK Environment
Automatically downloads and extracts the pre-compiled `ps3dev` SDK and PSL1GHT toolchain for your host OS (macOS ARM64/x64 or Linux x64) into `./ps3dev`:
```bash
make prepare
```

### 3. Build the Packages
Compile the client binary and generate the signed installation packages:
```bash
make
```

### 4. Output Packages
Upon completion, the build outputs the following installable files in `build/`:
- `moonlight-ps3.pkg`: Standard package installer for physical PS3 consoles (CFW / HEN) and RPCS3 emulator.
- `moonlight-ps3.gnpdrm.pkg`: Finalized Retail NPDRM signed package.

---

## Installation & Pairing

1. Copy `moonlight-ps3.pkg` (or `moonlight-ps3.gnpdrm.pkg`) to the root of a FAT32 formatted USB drive.
2. Insert the USB drive into your jailbroken PS3 (CFW or PS3HEN).
3. On the PS3 XMB, navigate to **Game > Package Manager > Install Package Files > Standard** and install the PKG.
4. Launch **Moonlight PS3** from your XMB Game column.
5. Select the Sunshine Host IP row to enter your PC's IP address using the native PS3 On-Screen Keyboard.
6. Select **Connect / Pair to Host**. Enter the 4-digit PIN shown on screen into the Sunshine Web UI under **PIN**. The PS3 waits up to 10 minutes for it.
7. **Vibepollo / Apollo:** a newly paired device may only list apps and watch. Grant it Launch, Controller, Mouse and Keyboard in the web UI under **Client Management**, or run `tools/setup-host.ps1` (see [PC (host) setup](#pc-host-setup-for-1080p60)).
8. Once paired, select your desired game or desktop application from the interactive App List menu to begin streaming!

---

## Credits

- **[Moonlight-QT](https://github.com/moonlight-stream/moonlight-qt)**: Core client logic, GameStream/Sunshine protocol handling, icon/graphics assets, and streaming implementation are directly adapted from Moonlight-QT.
- **[Opus Interactive Audio Codec](https://opus-codec.org/)**: Audio decoding functionality is powered by the Opus codec library.
- **[Moonlight Common C](https://github.com/moonlight-stream/moonlight-common-c)**: Common GameStream client library.
- **[Tiny3D](https://github.com/ps3dev/ps3libraries)** by Hermes: a patched copy of `tiny3d.c` ships in `third_party/tiny3d_yuvfix/` to fix the green terms of its planar YUV shader (see that folder's README).
- **[TEE PS3 Remoteplay](https://github.com/TheErsysEnding/TEE-PS3-Remoteplay-PC-games-on-PS3-Streaming)** and mohasi's cell-stream: the measurements that showed 1080p60 is decodable on a PS3 when deblocking and CABAC are off, and that 59.94 Hz is the rate to stream at.
- **[Okeanos86](https://github.com/Okeanos86/PS3-Moonlight)**: the mDNS host discovery, and the ideas behind rumble, pressure triggers, the Name (IP) host row, host-keyed pairing, the quit-app menu item and SD / 960x544 overscan output, re-implemented here.
- **GarryJerry**: the Square / Triangle face-button fix.
- **[Vibepollo](https://github.com/Nonary/Vibepollo)** / **[Apollo](https://github.com/ClassicOldSong/Apollo)**: the per-client overrides, permissions, virtual display and send-pacing features this client uses when available.

## Acknowledgments & Special Thanks

- **[PS3DEV / PSL1GHT SDK](https://github.com/ps3dev/ps3dev)**: Enormous thanks to the open-source PS3 toolchain and PSL1GHT SDK developers whose foundational work, cross-compilers, hardware header definitions, and libraries made this PlayStation 3 port possible.
- **[Mohammed Asif (mohasi)](https://codeberg.org/mohasi)**: Special thanks for technical guidance, problem-solving support, and project inspiration.
- **AcidNT3.1**: Special thanks for testing and feedback during development.
- **SyrianClippy**: Special thanks for testing and feedback during development.
- **Okeanos**: Special thanks for testing and feedback during development.

---

## License

This project is released under the GNU General Public License v3.0 (GPLv3).
