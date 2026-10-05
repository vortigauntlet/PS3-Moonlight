# Handoff: take Okeanos86's unreleased features and do them properly

**Goal:** bring the seven features from
[Okeanos86/PS3-Moonlight](https://github.com/Okeanos86/PS3-Moonlight)
(commits `8a64c4f`..`b7870cf`, 19–25 Sep 2026, never released) into this branch.
Re-implement them rather than merge them, fixing the problems listed below,
and make them fit what this branch already does (Vibepollo support, 59.94,
quit-on-exit, the new log fields).

Both projects are GPLv3, so reusing code is fine. Credit **Okeanos86**, and
**GarryJerry** for the controller fix, in the README credits and in the commit
messages.

---

## Where things stand

| | |
| :--- | :--- |
| Repo | `PS3-Moonlight` (OneDrive tree), branch `feature/1080p60-vibepollo` |
| Base | Cruslan's v1.3.1 + Okeanos86's mDNS commit `7bb78c9` + `18bcb13` (client) + `c790503` (docs, `tools/setup-host.ps1`) |
| Not merged | Okeanos86 `8a64c4f bc16203 9e9e23b 5e0372e f974e43 1816078 b7870cf`. Read them with `git show <sha>`; they are in the local object store via the `fork`/`origin` remotes. |
| Remotes | `origin` = Okeanos86, `cruslan` = the original, `fork` = the user's GitHub fork (being re-created from Cruslan's repo) |
| Build | WSL as root: `make PS3DEV=/root/ps3dev PSL1GHT=/root/ps3dev pkg` in a copy of the tree. `wsl -u root` is required because `/root` is unreadable otherwise. Output: `build/pkg/USRDIR/EBOOT.BIN`. |
| Deploy | `JellyFin-PS3/tools/ps3push/ftp.ps1 -Local <EBOOT> -Remote /dev_hdd0/game/MNLT00001/USRDIR/EBOOT.BIN -OutDir <scratch>`. It reads the file back and compares SHA-256. Check its "before:" line: other sessions deploy to the same console (192.168.0.202), though usually to the Jellyfin app. |
| Logs | `/dev_hdd0/tmp/moonlight_log.txt` over FTP; one `[PS3-NET]` line per second (fields in the README). Host side: `C:\Program Files\Sunshine\config\logs\` (the install is Vibepollo 2.0.0). |
| Launch | Cannot be automated: the user starts the app on the PS3. Plan every change so one launch gives a complete test. |
| Clean-build proof | `git archive HEAD` → extract in WSL → build. Builds have reported success while building nothing before; trust only this. |

---

## The seven features: what theirs does, what is wrong with it, what to build

### 1. Square/Triangle mapping — take it as is

**Theirs (`bc16203`, by GarryJerry):** Square → `X_FLAG`, Triangle →
`Y_FLAG`.

**Problem:** none. This branch is still **wrong**: `src/input.c:230-231` maps
Square → Y and Triangle → X, under a comment about "the user's layout". The
Xbox layout the host emulates puts X on the left (Square) and Y on top
(Triangle).

**Do:** swap the two lines and drop the comment. One-line change, test any
game's face buttons.

### 2. Pressure-sensitive L2/R2 — take the idea, add a fallback

**Theirs (`bc16203`):** `ioPadSetPortSetting(0, PAD_SETTINGS_PRESS_ON)`, then
triggers = `PRE_L2`/`PRE_R2` (0–255).

**Problems:**
- **Only port 0** is put into pressure mode.
- **No fallback:** a pad that does not report pressure (the Sixaxis, many
  third-party and Bluetooth pads, the PS Move navigation controller) returns
  `PRE_*` = 0. With their change its triggers stop working entirely.
- **Raw values:** DS3 pressure is noisy near zero and rarely reaches 255, so
  triggers drift slightly and never reach full.

**Build:**
- Enable pressure mode on every connected port, and again when a pad connects
  (`ioPadGetInfo` → `port_status`).
- Trigger value = `BTN_L2 ? max(PRE_L2_mapped, 1) : 0`. The digital bit is the
  truth of "pressed"; pressure only says how hard.
- Remap pressure through a small dead zone and a top boost: below ~8 → 0, and
  above ~230 → 255, scaled linearly in between.
- Add a `config.ini` key to force digital triggers (`trigger_mode=0`).

**Test:** a racing game's throttle, with a DS3 and if possible a non-pressure pad.

### 3. Rumble — the idea is good, the code has real bugs

**Theirs (`f974e43`):** `cb_rumble` stores the motor levels and the input loop
calls `ioPadSetActDirect(0, …)`.

**Problems:**
- **Bug — duplicate controller packets.** It **duplicates
  `LiSendControllerEvent`**: their diff adds a second, identical call, so every
  controller state goes to the host twice, at 250 Hz.
- **The motor command is unconditional:** it is sent on every 4 ms loop, 250
  times a second, even when nothing changed. That is wasted pad traffic,
  especially over Bluetooth.
- **Only port 0**, and it ignores the controller number `c` the host sends.
- **Motors are never stopped on teardown.** If the stream drops while a motor
  is on, the pad keeps buzzing in the menu.
- **No user setting.**

**Build:**
- Send the actuator only when the requested values change, and at most every
  ~20 ms.
- Map controller number → pad port. Large motor = `low >> 8`; the small motor
  is on/off, so switch it on at `high >= 0x4000`, not at any non-zero value.
- Stop both motors in `ps3input_stop()`, on connection-terminated, on quit, and
  when the stream window loses focus (XMB opened).
- `cb_rumble_triggers` (`src/connection.c:56`) can stay a no-op: the DS3 has
  no trigger motors.
- Settings toggle or `config.ini` key `rumble=1`.

**Test:** any game with rumble, plus pull the network mid-rumble — the pad must
go quiet.

### 4. A stable host identity — theirs uses the display name; use the host's UUID

**Theirs (`8a64c4f`):** keys the pinned server-certificate file on the saved
host *name* instead of the IP, so a DHCP address change does not break
pairing.

**Problems:**
- **Fragile key.** The name is a display string: mDNS names, manual entries
  ("Manual Entry" is a real value in this codebase) and renamed PCs all change
  it, and two hosts can share one.
- **Old pairings lost.** Existing pairings saved under the IP key are orphaned
  and the user has to re-pair.

**Build:**
- Key on the host's **`<uniqueid>`** from `/serverinfo`. It is plain HTTP, no
  pairing needed, and stable across IP changes and renames (Vibepollo here:
  `67B42504-…`).
- On first contact with a host that only has an IP-keyed file, **migrate** it
  rather than ask for a re-pair.
- Combine with mDNS: when a saved host's uniqueid is rediscovered at a new IP,
  update the saved address automatically.

**Test:** pair, change the PC's IP (or fake it in `config.ini`), and reconnect
without a PIN.

### 5. Host row shows "Name (IP)" — take it

**Theirs (`9e9e23b`):** the main menu shows `[ Name (IP) ]`.

**Problem:** none, beyond hiding the literal "Manual Entry" name, which theirs
already does.

**Do:** port it. Consider showing whether that host is the Vibepollo family,
from `<VirtualDisplayCapable>`.

### 6. The "Disconnect" item — replace it with a "Quit app on host" item

**Theirs (`b7870cf`):** a fourth main-menu item, shown after any stream in
this app run, that sends `/cancel`.

**Overlap:** this branch **already** sends `/cancel` when the user leaves a
stream (PS button or the exit combo — sent from inside the exit callback,
because the system kills the app moments later). A dropped connection
deliberately leaves the host app running so it can be resumed. So the only gap
left is "the connection dropped and I don't want to resume".

**Problems with theirs:**
- **Local flag.** It keys off a session flag, so it is shown when nothing is
  running and not shown after an app restart when something is.
- **Stale credentials.** It keeps `hinfo` at function scope across loop
  iterations, so it reuses stale handshake state.

**Build:**
- Show **"Quit <app> on host"** only when the selected host's `/serverinfo`
  (over HTTPS, as the paired client) reports `<currentgame>` ≠ 0. That is the
  host's own truth, and it survives app restarts.
- Reuse `hv_quit_app()`, which already parses `<cancel>1</cancel>` and records
  the host's status message.

**Test:** stream, kill the network, reconnect to the menu: the item appears;
quit; it disappears.

### 7. SD / analog CRT output: overscan correction + a 960x544 stream

**Theirs (`5e0372e`, `1816078`):**
- `videoGetDeviceInfo` detects non-HDMI output below 1280 wide.
- SD H.Offset/H.Shrink settings apply through `tiny3d_UserViewport`.
- A stream-resolution picker offering 1280x720 / 960x544, shown only on SD
  analog outputs.

**Problems:**
- **`res_idx` collides with this branch.** Here `res_idx` 1 = 1920x1080 and 2 =
  1792x1008; there 1 = 960x544. A shared `config.ini` would pick a 1080p
  stream on an SD TV.
- **Horizontal only.** There is no vertical overscan, and the base values are 0,
  so every user has to tune from scratch.
- **Hidden picker.** The resolution picker only exists on SD analog, so 960x544
  over HDMI is unreachable, though TEE measured it as useful there: 120+ fps,
  lowest latency.
- **Menu overflow.** It adds three more Settings rows to a page that is
  already full: this branch has 11 rows, the last at `SY(455)`.

**Build:**
- Save resolution as `stream_res=WIDTHxHEIGHT`, not an index, and read the old
  `res_idx` through a legacy table (the same pattern as `bitrate_kbps`).
- Offer 960x544 everywhere. On SD outputs, **default** to it.
- Overscan: X and Y, offset and scale, with sensible SD defaults. Apply them to
  the video quad and the UI alike.
- Do the Settings paging/scroll first (see below).

**Test:** the PS3 set to 480p/576p (composite or component), and 960x544 over
HDMI at 120 fps.

---

## Do this first: make the Settings page scroll

Every feature above needs a row (rumble, trigger mode, the resolution
changes, overscan ×4), and the page has no room. Turn `SETTINGS_ITEM_COUNT`
rows into a scrolling list, or group them: **Video** (resolution, FPS, bitrate,
decoder output, decode speed, presentation), **Network**, **Controls** (mouse,
rumble, triggers), **Display** (VSync, overscan, stats). Several existing
options are `config.ini`-only for this reason: `low_latency`, `ntsc_rate`,
`virtual_display`, `quit_on_exit`, `intra_refresh`, `vdec_spus`. Bring them
into the menu once there is space.

## Suggested order

1. Square/Triangle (1 line) and Name (IP) row (tiny) — ship-safe now.
2. Settings scrolling.
3. Rumble (fixed) and triggers (with fallback).
4. Host identity by uniqueid + migration.
5. "Quit app on host" from `currentgame`.
6. SD output + `stream_res` key + 960x544 everywhere.

One feature per commit, each with a hardware test before the next.

## Further ideas (TEE PS3 Remoteplay, for later)

- An on-screen stats panel bound to a button combo (theirs: SELECT+R3), showing
  the `[PS3-NET]` fields.
- A raw-HID keyboard path: send key positions rather than characters, so the
  PC's own layout applies.
- Recording the stream to `/dev_hdd0` with the host timestamps.
