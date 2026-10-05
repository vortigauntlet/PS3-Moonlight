# Performance notes

Measurements and mechanisms behind the resolution, bitrate and telemetry settings. Moved out of the README; the README keeps the conclusions.

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

