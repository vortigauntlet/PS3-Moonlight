#include "ui.h"
#include <tiny3d.h>
#include <libfont.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/memory.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <sysutil/sysutil.h>
#include <sysutil/osk.h>
#include <sysutil/msg.h>
#include <unistd.h>
#include <lv2/systime.h>
#include "input.h"
#include "moonlight_discovery.h"
#include <Limelight.h>
#include "video.h"
#include "audio.h"

#define CONFIG_DIR  "/dev_hdd0/game/MNLT00001/USRDIR"
#define CONFIG_PATH "/dev_hdd0/game/MNLT00001/USRDIR/config.ini"

static sys_ppu_thread_t ui_thread;
static int ui_thread_started = 0;
static volatile int ui_running = 1;
static int ui_width = 1280;
static int ui_height = 720;
static float scale_x = 1.0f;
static float scale_y = 1.0f;
static float scale_font = 1.0f;
static volatile int ui_state = UI_STATE_IP_ENTRY;

#define SX(x) ((float)(x) * scale_x)
#define SY(y) ((float)(y) * scale_y)
#define SF(s) ((u32)(((float)(s) * scale_font < 8.0f) ? 8.0f : ((float)(s) * scale_font)))

// Host IP state
static int ip_octets[4] = {192, 168, 1, 1};
static char target_ip_str[64] = "192.168.1.1";

// Video and stream preferences
// Frame rate.  50 is here because it is the only rate that can pace 1:1 against
// a 50 Hz video output, which PAL-region consoles can be set to in the XMB.  On
// a 60 Hz output it is the WORST of the three: 50 into 60 is an uneven 6:5
// cadence, whereas 30 is a clean 2:2 and 60 is 1:1.  Check the "output:" line in
// the log for the refresh rate this console is actually running at.
//
// Decode cost at 1080p, in H.264 macroblocks/sec:
//   30 fps = 244,800  (Level 4.1, and what Yo! Player ships as its 1080p mode)
//   50 fps = 408,000  (Level 4.2)
//   60 fps = 489,600  (Level 4.2, and what Yo! Player refuses on this hardware)
//   120 fps at 720p = 432,000 (Level 4.2).  TEE PS3 Remoteplay measured this as
//   the LOWEST input delay the console reaches (16.4 ms end to end): every stage
//   hands over whole pictures, so each waits up to one frame interval, and a
//   shorter interval shortens every wait.  Above ~120 it stops paying -- the
//   output is 59.94 Hz, so the extra pictures queue at the flip.
static int ui_fps_options[] = {30, 50, 60, 120};
#define NUM_FPS_OPTIONS (int)(sizeof(ui_fps_options) / sizeof(ui_fps_options[0]))
static int ui_fps = 60;
// Bitrate ladder, in kbps.  New steps are APPENDED so a config.ini written by an
// older build keeps its meaning: index 2 is still 10 Mbps.
//
// The upper steps exist to be measured, not because they are all expected to
// work.  The limit here is the PS3's own receive path, not the LAN, so the
// honest maximum has to be found on the console: raise the step until the
// [PS3-NET] log line shows drops climbing or rxq pinned at the socket buffer
// size, then come back down one.
//
// Saved as bitrate_kbps (not an index) so steps can be inserted in order;
// an old config.ini's bitrate_idx is still read through ui_bitrate_legacy.
//
// 12.5 Mbps is there because more bits are not automatically better at 60 fps:
// TEE PS3 Remoteplay measured ~13 Mbps holding 60 fps in 98% of seconds
// against 82% at ~20.  The decoder has a per-frame budget and bits spend it.
// 40 and 50 exist to FIND the PS3's UDP receive ceiling, not because they are
// expected to hold: watch rx= against the step and fecfail= in [PS3-NET].
// The request is the step minus 20% (FEC headroom), and Vibepollo's x264
// with its one-frame VBV has measured at about half of THAT on Cyberpunk.
static int ui_bitrate_options[] = {2500, 5000, 10000, 12500, 15000, 20000, 25000, 30000, 40000, 50000};
static const int ui_bitrate_legacy[] = {2500, 5000, 10000, 15000, 20000, 25000, 30000};
#define NUM_BITRATE_OPTIONS (int)(sizeof(ui_bitrate_options) / sizeof(ui_bitrate_options[0]))
// Default 20 Mbps (index 4).  Higher than Moonlight's reference for either
// supported mode, deliberately: the PS3's measured receive ceiling on this LAN
// is 20-25 Mbps, and at 1080p30 that headroom is better spent on picture than
// left unused.  An existing config.ini keeps whatever index it already had.
static int ui_bitrate_idx = 5; // 20000

static int ui_bitrate_index_for(int kbps) {
    int best = 0;
    for (int i = 1; i < NUM_BITRATE_OPTIONS; i++) {
        int d = ui_bitrate_options[i] - kbps, b = ui_bitrate_options[best] - kbps;
        if ((d < 0 ? -d : d) < (b < 0 ? -b : b)) best = i;
    }
    return best;
}

// Ask the host for the console's real output rate, 59.94 Hz, instead of a
// whole 60.  Anything else beats against the display: 60 on a 59.94 output
// drops a picture every ~17 s.  Sent as x-nv-video[0].clientRefreshRateX100,
// which Sunshine/Vibepollo use to run the encoder (and a virtual display) at
// the fractional rate.  1 = NTSC rates (59.94/29.97/119.88), 0 = whole numbers.
// config.ini: ntsc_rate
static int ui_ntsc_rate = 1;

// RTP payload bytes per packet.  1024 was this port's original value; 1392 is
// Moonlight's standard MTU-safe size and costs 36% fewer recv() syscalls for the
// same bitrate.
static int ui_packet_size_options[] = {1024, 1392};
#define NUM_PACKET_SIZE_OPTIONS (int)(sizeof(ui_packet_size_options) / sizeof(ui_packet_size_options[0]))
static int ui_packet_size_idx = 0; // Default: 1024, matching earlier builds

// Stream resolution, independent of the FPS selector above, so all four
// combinations can be tried.  Default stays 720p, which is what every earlier
// build sent.
//
// What each combination asks of the decoder, as H.264 macroblock rate:
//   720p60  = 3600 MB/frame x 60 = 216000 MB/s  -> inside Level 4.1 (245760)
//   1080p30 = 8160 MB/frame x 30 = 244800 MB/s  -> just inside Level 4.1
//   1080p60 = 8160 MB/frame x 60 = 489600 MB/s  -> needs Level 4.2
// The PS3's decoder advertises Level 4.2 (the Blu-ray maximum), but Blu-ray
// 1080p is 24 fps, so 1080p60 asks for 2.5x the macroblock rate this hardware
// has ever been shown to sustain.  It is offered so it can be measured; watch
// "Decoded FPS" in the stats overlay to see whether it actually keeps up.
//
// 1792x1008 (appended, so saved indices keep their meaning) is 16:9 at 7056
// MB/frame, 13.5% less decode than 1080p for a picture that scales to the TV
// almost untouched.  TEE PS3 Remoteplay measured it at 60 fps with zero drops.
//
// 960x544 is 2040 MB/frame, 57% less decode than 720p.  TEE PS3 Remoteplay
// measured it at 120+ fps with the lowest latency the console reaches.  It is
// also the sensible stream for a standard-definition output.
//
// Saved as stream_res=WIDTHxHEIGHT, not an index, so entries can be added in
// display order.  The old res_idx key is still read through ui_res_legacy.
static int ui_res_options[][2] = {{960, 544}, {1280, 720}, {1792, 1008}, {1920, 1080}};
static const int ui_res_legacy[][2] = {{1280, 720}, {1920, 1080}, {1792, 1008}};
#define NUM_RES_OPTIONS (int)(sizeof(ui_res_options) / sizeof(ui_res_options[0]))
#define RES_IDX_960 0
#define RES_IDX_720 1
static int ui_res_idx = RES_IDX_720; // Default: 1280x720
static int ui_res_configured = 0;    // config.ini named a resolution
static int ui_output_is_sd = 0;      // console video output is below 1280 wide

static int ui_res_index_for(int w, int h) {
    for (int i = 0; i < NUM_RES_OPTIONS; i++)
        if (ui_res_options[i][0] == w && ui_res_options[i][1] == h) return i;
    return -1;
}

// Controls.
// Rumble: forward the host's rumble requests to the DS3 motors.
// config.ini: rumble (1 = on, 0 = off)
static int ui_rumble = 1;
// Trigger mode: 1 = analog (DS3 pressure, with a digital fallback for pads that
// report none), 0 = always digital (full 255 when the L2/R2 button is down).
// config.ini: trigger_mode
static int ui_trigger_mode = 1;

// Overscan correction, applied to the whole picture (menus and the stream) so
// the edges survive a CRT or a TV that crops the signal.  Scale is the percent
// of the screen used; offset is in 1280x720 units and moves the picture.
// The defaults are 100% / 0 on HDMI and a modest shrink on SD outputs, where
// cropping is the norm.  config.ini: overscan_x, overscan_y, overscan_xoff,
// overscan_yoff
#define OVS_SCALE_MIN 70
#define OVS_OFF_LIMIT 60
static int ui_ovs_x = 100, ui_ovs_y = 100, ui_ovs_xoff = 0, ui_ovs_yoff = 0;
static int ui_ovs_configured = 0;

// VDEC output pixel format.
//   0 = ARGB32   - VDEC does the YUV->RGB conversion itself and writes 4 bytes
//                  per pixel.  This is what every earlier build used.
//   1 = YUV420P  - VDEC writes 1.5 bytes per pixel and the RSX does the colour
//                  conversion in tiny3d's built-in YUV shader.  2.67x less data
//                  crossing main memory per frame, and the conversion moves off
//                  the decoder's SPUs onto the GPU, where it is close to free.
//                  This is how JellyFin-PS3 decodes 1080p on the same console.
// Default stays ARGB because the YUV path has not been run on hardware yet; if
// it works, it is strictly the better one.
static int ui_pixfmt = 0;

// Ask Sunshine for periodic intra refresh instead of IDR frames.  On by
// default: it removes the single largest burst on the wire, which is the
// dominant packet-loss mechanism at these bitrates.  Sunshine silently ignores
// the request if its encoder cannot do it.
//
// There is no room left on the settings page, so this one lives in config.ini
// only -- set "intra_refresh=0" in
// /dev_hdd0/game/MNLT00001/USRDIR/config.ini to turn it off.
static int ui_intra_refresh = 1;

// Skip the in-loop deblocking filter in the decoder.
//   -1 = auto (DEFAULT): skip it only on modes that need Level 4.2, i.e.
//        1080p50/60.  Those ask for ~2.2x the ~220,000 MB/s this console
//        decodes in practice, so there is no quality A/B to run there --
//        without every available speedup they cannot play at all.
//    0 = always keep deblocking (full picture quality)
//    1 = always skip it (fastest decode, slightly blockier picture)
//
// 1080p30 and 720p60 stay at full quality under auto on purpose.  Run 1080p30
// as-is first; if Decoded FPS sits short of 30, pick FAST and compare -- a
// clean A/B with one variable, and the quality verdict is the user's to make.
//
// Deblocking is the single most expensive stage of H.264 decode -- it is what
// the IBM Cell research specifically vectorised because it dominated the
// profile.  cellVdec exposes a pseudo-level (1042, CELL_VDEC_AVC_LEVEL_UNK)
// that turns it off, typically worth 20-35% of decode time.
//
// The cost is drift: the encoder filtered its reference frames and we did not,
// so our reconstruction slowly diverges. Two things already in this build bound
// that -- periodic intra refresh continuously re-seeds every macroblock, and
// maxNumReferenceFrames=1 stops errors propagating down a long chain. At the
// bitrates this client now targets there is also very little blocking to filter
// in the first place, which is exactly why trading it for throughput is the
// right way round: we have bitrate headroom and no decode headroom.
//
// config.ini key: no_deblock
static int ui_no_deblock = -1;

// SPU threads for the decoder. 0 = auto (2 for 720p, 3 for HD as JellyFin-PS3
// uses, 4 for Level 4.2 modes as Movian uses). Nothing else in this app uses
// SPUs, and GameOS leaves six available, so 5 is worth a measurement at
// 1080p60 -- compare dec= in the [PS3-NET] log line. config.ini key: vdec_spus
static int ui_vdec_spus = 0;
static int ui_vsync = 1; // Default: VSync ON (1)

// Ask an Apollo/Vibepollo host for a virtual display at the stream mode.
// Ignored by plain Sunshine (the parameter is never sent to it).  On by
// default: it is the feature those hosts exist for, and it lets the game run
// at the PS3's exact resolution and frame rate.  config.ini: virtual_display
static int ui_virtual_display = 1;

// When the user leaves a stream (exit combo, or quitting to the XMB), also
// quit the app on the host so it does not keep running with nobody watching.
// A dropped connection does NOT trigger it, so a network blip can still be
// resumed.  config.ini: quit_on_exit (1 = quit, 0 = leave it running)
static int ui_quit_on_exit = 1;

// Audio channels requested from the host: 2 (stereo), 6 (5.1) or 8 (7.1).
// Surround at 15 Mbps and up gets the host's high-quality mode (uncoupled
// streams at a much higher Opus bitrate).  config.ini: audio_channels
static int ui_audio_channels = 2;

// Presentation policy (config.ini: low_latency)
//   0 = smooth:   FIFO up to two deep, trimmed only after ~0.5 s stuck deep
//   1 = balanced (default): FIFO absorbs a pair that lands in one refresh,
//       but a queue that stays two deep for 6 vblanks loses its oldest
//   2 = newest:   every vblank shows the newest picture and drops the rest.
//       Lowest lag, but over Wi-Fi it threw away ~1 in 3 decoded pictures.
static int ui_low_latency = 1;

// Navigation item counts for Main Menu and Settings Submenu
// Host / Settings / Connect, plus "Quit app on host" while the host reports one running.
#define MAIN_MENU_ITEM_COUNT (host_running_app ? 4 : 3)
static int active_main_item = 0; // 0: Sunshine Host IP, 1: Configure Settings, 2: Connect/Pair

// Settings rows, in display order (grouped).
enum {
    // Video
    SR_FPS, SR_RES, SR_BITRATE, SR_PIXFMT, SR_DEBLOCK, SR_PRESENT, SR_NTSC, SR_SPUS,
    // Network & host
    SR_PACKET, SR_INTRA, SR_VDISPLAY, SR_QUITEXIT, SR_AUDIO,
    // Controls
    SR_MOUSE, SR_RUMBLE, SR_TRIGGERS,
    // Display
    SR_VSYNC, SR_OVS_X, SR_OVS_Y, SR_OVS_XOFF, SR_OVS_YOFF, SR_STATS, SR_VERBOSE,
    SR_BACK,
    SETTINGS_ITEM_COUNT
};
#define SETTINGS_VISIBLE 10
static int active_settings_item = 0;
static int settings_scroll = 0;

static int frames_drawn_this_sec = 0;
static int ui_fps_actual = 0;
static u64 last_ui_time = 0;
static int show_stats = 0; // Default: Stats OFF (0)
static int ui_verbose = 0; // Default: Verbose Logging OFF (0)
static int ui_mouse_mode = 0; // Default: 0 = Game Mode (Relative), 1 = Desktop Mode (Absolute)

// OSK management state
static sys_mem_container_t osk_container;
static int osk_container_created = 0;
static volatile int osk_active = 0;
static u16 osk_title[64];
static u16 osk_initial[64];
static u16 osk_output[64];

void ui_set_state(int state) { ui_state = state; }
int ui_get_state() { return ui_state; }
int ui_is_running() { return ui_running; }
int ui_get_fps() { return ui_fps; }
int ui_get_bitrate() { return ui_bitrate_options[ui_bitrate_idx]; }
int ui_get_refresh_x100(void) {
    if (ui_ntsc_rate && ui_fps != 50) return (ui_fps * 100000 + 500) / 1001;
    return ui_fps * 100;
}

// Macroblock rate of the selected mode, for the level hints in the menu.
static unsigned int ui_mode_mb_rate(void) {
    unsigned int w = (unsigned int)(ui_res_options[ui_res_idx][0] + 15) / 16;
    unsigned int h = (unsigned int)(ui_res_options[ui_res_idx][1] + 15) / 16;
    return w * h * (unsigned int)ui_fps;
}
int ui_get_packet_size() { return ui_packet_size_options[ui_packet_size_idx]; }
int ui_get_stream_width(void)  { return ui_res_options[ui_res_idx][0]; }
int ui_get_stream_height(void) { return ui_res_options[ui_res_idx][1]; }
// YUV420P needs 64-byte-aligned plane pitches for the RSX texture: the chroma
// pitch is width/2, so 960 wide (480) cannot use it and falls back to ARGB32.
int ui_get_pixel_format(void)  { return (ui_pixfmt && ((ui_res_options[ui_res_idx][0] / 2) % 64) == 0) ? 1 : 0; }
int ui_get_rumble(void)        { return ui_rumble; }
int ui_get_trigger_mode(void)  { return ui_trigger_mode; }
int ui_get_intra_refresh(void) { return ui_intra_refresh; }
int ui_get_virtual_display(void) { return ui_virtual_display; }
int ui_get_quit_on_exit(void) { return ui_quit_on_exit; }
int ui_get_audio_channels(void) { return ui_audio_channels; }
int ui_get_low_latency(void) { return ui_low_latency; }
int ui_get_no_deblock(void)    { return ui_no_deblock; }
int ui_get_vdec_spus(void)     { return ui_vdec_spus; }
int ui_get_width() { return ui_width; }
int ui_get_height() { return ui_height; }
void ui_stop() { ui_running = 0; }
int ui_get_vsync() { return ui_vsync; }
int ui_get_show_stats() { return show_stats; }
int ui_get_verbose() { return ui_verbose; }
int ui_get_mouse_mode(void) { return ui_mouse_mode; }

static char pairing_pin_str[16] = "";

void ui_set_pairing_pin(const char *pin) {
    if (pin) {
        strncpy(pairing_pin_str, pin, sizeof(pairing_pin_str) - 1);
        pairing_pin_str[sizeof(pairing_pin_str) - 1] = '\0';
    } else {
        pairing_pin_str[0] = '\0';
    }
}

const char* ui_get_pairing_pin(void) {
    return pairing_pin_str;
}

// App Selection State
static ps3_app_list_t current_app_list;
static int active_app_idx = 0;
static volatile int app_selection_confirmed = 0;

void ui_set_app_list(const ps3_app_list_t *list) {
    if (list) {
        memcpy(&current_app_list, list, sizeof(current_app_list));
    } else {
        memset(&current_app_list, 0, sizeof(current_app_list));
    }
    active_app_idx = 0;
    app_selection_confirmed = 0;
}

int ui_get_selected_app_id(void) {
    if (current_app_list.count > 0 && active_app_idx >= 0 && active_app_idx < current_app_list.count) {
        return current_app_list.apps[active_app_idx].id;
    }
    return -1;
}

const char* ui_get_selected_app_uuid(void) {
    if (current_app_list.count > 0 && active_app_idx >= 0 && active_app_idx < current_app_list.count) {
        return current_app_list.apps[active_app_idx].uuid;
    }
    return "";
}

// Error-screen detail line, written by the connect thread, read by the UI.
static char ui_error_detail[192] = "";
void ui_set_error_detail(const char *msg) {
    if (!msg) msg = "";
    strncpy(ui_error_detail, msg, sizeof(ui_error_detail) - 1);
    ui_error_detail[sizeof(ui_error_detail) - 1] = '\0';
}

const char* ui_get_selected_app_name(void) {
    if (current_app_list.count > 0 && active_app_idx >= 0 && active_app_idx < current_app_list.count) {
        return current_app_list.apps[active_app_idx].name;
    }
    return "";
}

int ui_is_app_selected(void) {
    return app_selection_confirmed;
}

void ui_reset_app_selection(void) {
    app_selection_confirmed = 0;
}

// Multi-host saved config
static ui_saved_host_t saved_hosts[UI_MAX_SAVED_HOSTS];
static int saved_host_count = 0;
static int selected_host_idx = -1;

int ui_get_saved_host_count(void) { return saved_host_count; }

const ui_saved_host_t *ui_get_saved_host(int idx) {
    if (idx < 0 || idx >= saved_host_count) return NULL;
    return &saved_hosts[idx];
}

int ui_get_selected_host_index(void) { return selected_host_idx; }

void ui_select_host(int idx) {
    if (idx < 0 || idx >= saved_host_count) return;
    selected_host_idx = idx;
    ui_set_target_ip(saved_hosts[idx].address);
}

int ui_upsert_saved_host(const char *name, const char *address) {
    if (!address || !*address) return -1;
    for (int i = 0; i < saved_host_count; i++) {
        if (strcmp(saved_hosts[i].address, address) == 0) {
            if (name && *name) {
                strncpy(saved_hosts[i].name, name, sizeof(saved_hosts[i].name) - 1);
                saved_hosts[i].name[sizeof(saved_hosts[i].name) - 1] = '\0';
            }
            return i;
        }
    }
    int idx;
    if (saved_host_count < UI_MAX_SAVED_HOSTS) {
        idx = saved_host_count++;
    } else {
        idx = (selected_host_idx >= 0) ? selected_host_idx : 0;
    }
    memset(&saved_hosts[idx], 0, sizeof(saved_hosts[idx]));
    strncpy(saved_hosts[idx].name, (name && *name) ? name : "Unnamed Host",
            sizeof(saved_hosts[idx].name) - 1);
    strncpy(saved_hosts[idx].address, address, sizeof(saved_hosts[idx].address) - 1);
    saved_hosts[idx].paired = 0;
    saved_hosts[idx].last_app_id = -1;
    return idx;
}

void ui_set_host_paired(int idx, int paired) {
    if (idx < 0 || idx >= saved_host_count) return;
    saved_hosts[idx].paired = paired ? 1 : 0;
}

void ui_set_host_last_app(int idx, int app_id) {
    if (idx < 0 || idx >= saved_host_count) return;
    saved_hosts[idx].last_app_id = app_id;
}

void ui_set_host_uuid(int idx, const char *uuid) {
    if (idx < 0 || idx >= saved_host_count || !uuid) return;
    snprintf(saved_hosts[idx].uuid, sizeof(saved_hosts[idx].uuid), "%s", uuid);
}

int ui_find_host_by_uuid(const char *uuid) {
    if (!uuid || !*uuid) return -1;
    for (int i = 0; i < saved_host_count; i++)
        if (saved_hosts[i].uuid[0] && strcmp(saved_hosts[i].uuid, uuid) == 0) return i;
    return -1;
}

void ui_set_host_address(int idx, const char *address, const char *name) {
    if (idx < 0 || idx >= saved_host_count || !address || !*address) return;
    snprintf(saved_hosts[idx].address, sizeof(saved_hosts[idx].address), "%s", address);
    if (name && *name) snprintf(saved_hosts[idx].name, sizeof(saved_hosts[idx].name), "%s", name);
    if (idx == selected_host_idx) ui_set_target_ip(address);
}

// Host status, written by the main thread and read by the UI thread.  Small
// scalars and one string, written name-first: a torn read shows at worst a
// stale label for one frame.
static volatile int host_running_app = 0;
static char host_running_name[64] = "";
static volatile int host_is_apollo = 0;
static volatile int quit_request = 0;

void ui_set_host_status(int running_app, const char *app_name, int apollo_family) {
    snprintf(host_running_name, sizeof(host_running_name), "%s", app_name ? app_name : "");
    host_is_apollo = apollo_family ? 1 : 0;
    host_running_app = running_app ? 1 : 0;
}

int ui_take_quit_request(void) {
    if (!quit_request) return 0;
    quit_request = 0;
    return 1;
}

// Host Discovery State
static mld_host_t discovered_hosts[MLD_MAX_HOSTS];
static int discovered_host_count = 0;
static int discovery_scanned = 0;
static int active_host_idx = 0;
static volatile int host_selection_confirmed = 0;
static volatile int manual_entry_requested = 0;

void ui_set_discovered_hosts(const mld_host_t *hosts, int count) {
    if (count < 0) count = 0;
    if (count > MLD_MAX_HOSTS) count = MLD_MAX_HOSTS;
    if (hosts && count > 0) memcpy(discovered_hosts, hosts, sizeof(mld_host_t) * count);
    discovered_host_count = count;
    discovery_scanned = 1;
    active_host_idx = 0;
}

int ui_is_host_selected(void) { return host_selection_confirmed || manual_entry_requested; }
int ui_wants_manual_entry(void) { return manual_entry_requested; }

int ui_get_selected_host_ip(char *out, size_t out_size) {
    if (!host_selection_confirmed) return 0;
    if (active_host_idx < 0 || active_host_idx >= discovered_host_count) return 0;
    snprintf(out, out_size, "%s", discovered_hosts[active_host_idx].address);
    return 1;
}

int ui_get_selected_host_name(char *out, size_t out_size) {
    if (!host_selection_confirmed) return 0;
    if (active_host_idx < 0 || active_host_idx >= discovered_host_count) return 0;
    snprintf(out, out_size, "%s", discovered_hosts[active_host_idx].name);
    return 1;
}

void ui_reset_host_selection(void) {
    host_selection_confirmed = 0;
    manual_entry_requested = 0;
    discovery_scanned = 0;
    discovered_host_count = 0;
    active_host_idx = 0;
}

void ui_set_target_ip(const char *str) {
    if (!str || !*str) return;
    int o[4];
    if (sscanf(str, "%d.%d.%d.%d", &o[0], &o[1], &o[2], &o[3]) == 4) {
        for (int i = 0; i < 4; i++) {
            if (o[i] >= 0 && o[i] <= 255) {
                ip_octets[i] = o[i];
            }
        }
        snprintf(target_ip_str, sizeof(target_ip_str), "%d.%d.%d.%d", 
                 ip_octets[0], ip_octets[1], ip_octets[2], ip_octets[3]);
    } else {
        strncpy(target_ip_str, str, sizeof(target_ip_str) - 1);
        target_ip_str[sizeof(target_ip_str) - 1] = '\0';
    }
}

const char* ui_get_target_ip() {
    if (target_ip_str[0] == '\0') {
        snprintf(target_ip_str, sizeof(target_ip_str), "%d.%d.%d.%d", 
                 ip_octets[0], ip_octets[1], ip_octets[2], ip_octets[3]);
    }
    return target_ip_str;
}

void ui_save_settings(void) {
    mkdir("/dev_hdd0/game/MNLT00001", 0700);
    mkdir(CONFIG_DIR, 0700);

    const char *final_path = CONFIG_PATH;
    char tmp_path[144];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        final_path = "config.ini";
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
        f = fopen(tmp_path, "w");
    }
    if (!f) return;

    fprintf(f, "# PS3-Moonlight Configuration File\n\n[global]\n");
    fprintf(f, "selected_host=%d\n", selected_host_idx);
    fprintf(f, "fps=%d\n", ui_fps);
    fprintf(f, "bitrate_kbps=%d\n", ui_bitrate_options[ui_bitrate_idx]);
    fprintf(f, "ntsc_rate=%d\n", ui_ntsc_rate);
    fprintf(f, "packet_size_idx=%d\n", ui_packet_size_idx);
    fprintf(f, "stream_res=%dx%d\n", ui_res_options[ui_res_idx][0], ui_res_options[ui_res_idx][1]);
    fprintf(f, "rumble=%d\n", ui_rumble);
    fprintf(f, "trigger_mode=%d\n", ui_trigger_mode);
    fprintf(f, "overscan_x=%d\n", ui_ovs_x);
    fprintf(f, "overscan_y=%d\n", ui_ovs_y);
    fprintf(f, "overscan_xoff=%d\n", ui_ovs_xoff);
    fprintf(f, "overscan_yoff=%d\n", ui_ovs_yoff);
    fprintf(f, "pixfmt=%d\n", ui_pixfmt);
    fprintf(f, "intra_refresh=%d\n", ui_intra_refresh);
    fprintf(f, "virtual_display=%d\n", ui_virtual_display);
    fprintf(f, "quit_on_exit=%d\n", ui_quit_on_exit);
    fprintf(f, "audio_channels=%d\n", ui_audio_channels);
    fprintf(f, "low_latency=%d\n", ui_low_latency);
    fprintf(f, "no_deblock=%d\n", ui_no_deblock);
    fprintf(f, "vdec_spus=%d\n", ui_vdec_spus);
    fprintf(f, "mouse_mode=%d\n", ui_mouse_mode);
    fprintf(f, "vsync=%d\n", ui_vsync ? 1 : 0);
    fprintf(f, "stats=%d\n", show_stats ? 1 : 0);
    fprintf(f, "verbose=%d\n", ui_verbose ? 1 : 0);

    for (int i = 0; i < saved_host_count; i++) {
        fprintf(f, "\n[host.%d]\n", i);
        fprintf(f, "name=%s\n", saved_hosts[i].name);
        fprintf(f, "address=%s\n", saved_hosts[i].address);
        if (saved_hosts[i].uuid[0]) fprintf(f, "uuid=%s\n", saved_hosts[i].uuid);
        fprintf(f, "paired=%d\n", saved_hosts[i].paired);
        fprintf(f, "last_app=%d\n", saved_hosts[i].last_app_id);
    }

    fflush(f);
    fclose(f);

    if (rename(tmp_path, final_path) != 0) {
        FILE *direct = fopen(final_path, "w");
        FILE *src = fopen(tmp_path, "r");
        if (direct && src) {
            char buf[256]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, direct);
        }
        if (direct) fclose(direct);
        if (src) fclose(src);
        remove(tmp_path);
    }
}

void ui_load_settings(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) f = fopen("config.ini", "r");
    if (!f) return;

    saved_host_count = 0;
    selected_host_idx = -1;
    char section[32] = "";
    int current_host_idx = -1;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\r' || *p == '\n') continue;
        char *end = p + strlen(p) - 1;
        while (end >= p && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t'))
            *end-- = '\0';
        if (*p == '\0') continue;

        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close) {
                *close = '\0';
                snprintf(section, sizeof(section), "%s", p + 1);
                if (strncmp(section, "host.", 5) == 0) {
                    current_host_idx = saved_host_count;
                    if (current_host_idx < UI_MAX_SAVED_HOSTS) {
                        memset(&saved_hosts[current_host_idx], 0, sizeof(saved_hosts[current_host_idx]));
                        saved_hosts[current_host_idx].last_app_id = -1;
                        saved_host_count++;
                    }
                } else { current_host_idx = -1; }
            }
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = p;
        const char *val = eq + 1;

        if (section[0] == '\0' || strcmp(section, "global") == 0) {
            if (strcmp(key, "selected_host") == 0) selected_host_idx = atoi(val);
            else if (strcmp(key, "fps") == 0) { int v = atoi(val); if (v==30||v==50||v==60||v==120) ui_fps=v; }
            else if (strcmp(key, "bitrate_kbps") == 0) { int v = atoi(val); if (v > 0) ui_bitrate_idx = ui_bitrate_index_for(v); }
            else if (strcmp(key, "bitrate_idx") == 0) {
                // Pre-kbps config: index into the old ladder.
                int v = atoi(val);
                if (v >= 0 && v < (int)(sizeof(ui_bitrate_legacy) / sizeof(ui_bitrate_legacy[0])))
                    ui_bitrate_idx = ui_bitrate_index_for(ui_bitrate_legacy[v]);
            }
            else if (strcmp(key, "ntsc_rate") == 0) ui_ntsc_rate = (atoi(val) != 0);
            else if (strcmp(key, "packet_size_idx") == 0) { int v = atoi(val); if (v>=0&&v<NUM_PACKET_SIZE_OPTIONS) ui_packet_size_idx=v; }
            else if (strcmp(key, "stream_res") == 0) {
                int w = 0, h = 0;
                if (sscanf(val, "%dx%d", &w, &h) == 2) {
                    int i = ui_res_index_for(w, h);
                    if (i >= 0) { ui_res_idx = i; ui_res_configured = 1; }
                }
            }
            else if (strcmp(key, "res_idx") == 0) {
                // Pre-stream_res config: index into the old ladder.  A stream_res
                // line, if present, wins whichever order they appear in.
                int v = atoi(val);
                if (!ui_res_configured && v >= 0 && v < (int)(sizeof(ui_res_legacy) / sizeof(ui_res_legacy[0]))) {
                    int i = ui_res_index_for(ui_res_legacy[v][0], ui_res_legacy[v][1]);
                    if (i >= 0) { ui_res_idx = i; ui_res_configured = 1; }
                }
            }
            else if (strcmp(key, "rumble") == 0) ui_rumble = (atoi(val) != 0);
            else if (strcmp(key, "trigger_mode") == 0) ui_trigger_mode = (atoi(val) != 0);
            else if (strcmp(key, "overscan_x") == 0) { int v = atoi(val); if (v>=OVS_SCALE_MIN&&v<=100) { ui_ovs_x=v; ui_ovs_configured=1; } }
            else if (strcmp(key, "overscan_y") == 0) { int v = atoi(val); if (v>=OVS_SCALE_MIN&&v<=100) { ui_ovs_y=v; ui_ovs_configured=1; } }
            else if (strcmp(key, "overscan_xoff") == 0) { int v = atoi(val); if (v>=-OVS_OFF_LIMIT&&v<=OVS_OFF_LIMIT) { ui_ovs_xoff=v; ui_ovs_configured=1; } }
            else if (strcmp(key, "overscan_yoff") == 0) { int v = atoi(val); if (v>=-OVS_OFF_LIMIT&&v<=OVS_OFF_LIMIT) { ui_ovs_yoff=v; ui_ovs_configured=1; } }
            else if (strcmp(key, "pixfmt") == 0) { int v = atoi(val); if (v==0||v==1) ui_pixfmt=v; }
            else if (strcmp(key, "intra_refresh") == 0) ui_intra_refresh = (atoi(val) != 0);
            else if (strcmp(key, "virtual_display") == 0) ui_virtual_display = (atoi(val) != 0);
            else if (strcmp(key, "quit_on_exit") == 0) ui_quit_on_exit = (atoi(val) != 0);
            else if (strcmp(key, "audio_channels") == 0) { int v = atoi(val); if (v==2||v==6||v==8) ui_audio_channels=v; }
            else if (strcmp(key, "low_latency") == 0) { int v = atoi(val); if (v >= 0 && v <= 2) ui_low_latency = v; }
            else if (strcmp(key, "no_deblock") == 0) { int v = atoi(val); if (v>=-1&&v<=1) ui_no_deblock=v; }
            else if (strcmp(key, "vdec_spus") == 0) { int v = atoi(val); if (v>=0&&v<=6) ui_vdec_spus=v; }
            else if (strcmp(key, "mouse_mode") == 0) { int v = atoi(val); if (v==0||v==1) ui_mouse_mode=v; }
            else if (strcmp(key, "vsync") == 0) ui_vsync = (atoi(val) != 0);
            else if (strcmp(key, "stats") == 0) show_stats = (atoi(val) != 0);
            else if (strcmp(key, "verbose") == 0) ui_verbose = (atoi(val) != 0);
            else if ((strcmp(key, "host_ip") == 0 || strcmp(key, "ip") == 0) && val[0]) {
                if (saved_host_count == 0) {
                    int i = ui_upsert_saved_host("Saved Host", val);
                    selected_host_idx = i;
                }
            }
        } else if (current_host_idx >= 0 && current_host_idx < UI_MAX_SAVED_HOSTS) {
            if (strcmp(key, "name") == 0)
                strncpy(saved_hosts[current_host_idx].name, val, sizeof(saved_hosts[current_host_idx].name) - 1);
            else if (strcmp(key, "address") == 0)
                strncpy(saved_hosts[current_host_idx].address, val, sizeof(saved_hosts[current_host_idx].address) - 1);
            else if (strcmp(key, "uuid") == 0)
                snprintf(saved_hosts[current_host_idx].uuid, sizeof(saved_hosts[current_host_idx].uuid), "%s", val);
            else if (strcmp(key, "paired") == 0) saved_hosts[current_host_idx].paired = atoi(val);
            else if (strcmp(key, "last_app") == 0) saved_hosts[current_host_idx].last_app_id = atoi(val);
        }
    }
    fclose(f);

    if (selected_host_idx < 0 || selected_host_idx >= saved_host_count)
        selected_host_idx = (saved_host_count > 0) ? 0 : -1;
    if (selected_host_idx >= 0)
        ui_set_target_ip(saved_hosts[selected_host_idx].address);
}

static void ascii_to_utf16(u16 *dst, const char *src, int max_len) {
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = (u16)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

static void utf16_to_ascii(char *dst, const u16 *src, int max_len) {
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = (char)(src[i] & 0xFF);
        i++;
    }
    dst[i] = 0;
}

static void ui_osk_callback(u64 status, u64 param, void *usrdata) {
    (void)param;
    (void)usrdata;
    if (status == SYSUTIL_OSK_LOADED) {
        ui_push_log("OSK: Virtual keyboard opened");
    } else if (status == SYSUTIL_OSK_DONE) {
        oskCallbackReturnParam ret_param;
        memset(&ret_param, 0, sizeof(ret_param));
        ret_param.str = osk_output;
        ret_param.len = 64;
        oskUnloadAsync(&ret_param);
        
        if (ret_param.res == OSK_OK) {
            char entered_text[64];
            utf16_to_ascii(entered_text, osk_output, sizeof(entered_text));
            int idx = ui_upsert_saved_host("Manual Entry", entered_text);
            ui_select_host(idx);
            char log_msg[96];
            snprintf(log_msg, sizeof(log_msg), "Host saved: %s", entered_text);
            ui_save_settings();
            ui_push_log(log_msg);
        } else {
            ui_push_log("OSK: Finished");
        }
    } else if (status == SYSUTIL_OSK_INPUT_CANCELED) {
        oskCallbackReturnParam ret_param;
        memset(&ret_param, 0, sizeof(ret_param));
        oskUnloadAsync(&ret_param);
        ui_push_log("OSK: Virtual keyboard canceled");
    } else if (status == SYSUTIL_OSK_UNLOADED) {
        if (osk_container_created) {
            sysMemContainerDestroy(osk_container);
            osk_container_created = 0;
        }
        osk_active = 0;
        ui_push_log("OSK: Virtual keyboard closed");
    }
}

void ui_open_osk(void) {
    if (osk_active) return;
    
    // Allocate 4MB memory container required by GameOS OSK service
    if (sysMemContainerCreate(&osk_container, 4 * 1024 * 1024) != 0) {
        ui_push_log("OSK Error: Memory container allocation failed");
        return;
    }
    osk_container_created = 1;
    osk_active = 1;
    
    oskParam param;
    memset(&param, 0, sizeof(oskParam));
    param.allowedPanels = OSK_PANEL_TYPE_DEFAULT | OSK_PANEL_TYPE_ALPHABET | 
                          OSK_PANEL_TYPE_NUMERAL | OSK_PANEL_TYPE_URL | OSK_PANEL_TYPE_LATIN;
    param.firstViewPanel = OSK_PANEL_TYPE_URL;
    param.controlPoint.x = 0.0f;
    param.controlPoint.y = 0.0f;
    param.prohibitFlags = 0;

    ui_get_target_ip();
    ascii_to_utf16(osk_title, "Enter Sunshine Host IP / Address", 64);
    ascii_to_utf16(osk_initial, target_ip_str, 64);

    oskInputFieldInfo input_info;
    memset(&input_info, 0, sizeof(oskInputFieldInfo));
    input_info.message = osk_title;
    input_info.startText = osk_initial;
    input_info.maxLength = 63;

    oskSetKeyLayoutOption(OSK_10KEY_PANEL | OSK_FULLKEY_PANEL);
    oskSetInitialInputDevice(OSK_DEVICE_PAD);

    if (oskLoadAsync(osk_container, &param, &input_info) != 0) {
        ui_push_log("OSK Error: oskLoadAsync failed");
        sysMemContainerDestroy(osk_container);
        osk_container_created = 0;
        osk_active = 0;
    }
}

// Native PS3 Message Dialog (Confirmation Pop-up)
static volatile int msg_dialog_active = 0;

static void ui_msg_dialog_callback(msgButton button, void *usrData) {
    (void)usrData;
    msgDialogClose(0.0f);
    msg_dialog_active = 0;
    if (button == MSG_DIALOG_BTN_YES) {
        ui_push_log("Exit confirmed by user. Quitting to PS3 XMB...");
        ui_stop();
    } else {
        ui_push_log("Exit canceled.");
    }
}

void ui_open_exit_dialog(void) {
    if (msg_dialog_active || osk_active) return;
    msg_dialog_active = 1;
    msgDialogOpen2(MSG_DIALOG_NORMAL | MSG_DIALOG_BTN_TYPE_YESNO | MSG_DIALOG_DEFAULT_CURSOR_NO,
                   "Do you want to quit Moonlight and return to the PS3 XMB?",
                   ui_msg_dialog_callback, NULL, NULL);
}

static void ui_loop(void *arg);

#define MAX_LOG_LINES 25
#define MAX_LOG_WIDTH 100

static char log_buffer[MAX_LOG_LINES][MAX_LOG_WIDTH];
static int log_count = 0;
static sys_mutex_t log_mutex;
static int log_mutex_initialized = 0;

// Simple 8x8 font bitmap (MSX style) - subset (32-127)
// This is a minimal fallback to avoid external dependencies
static const unsigned char font_8x8_basic[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
    {0x18,0x3c,0x3c,0x18,0x18,0x00,0x18,0x00}, // !
    {0x6c,0x6c,0x6c,0x00,0x00,0x00,0x00,0x00}, // "
    {0x6c,0x6c,0xfe,0x6c,0xfe,0x6c,0x6c,0x00}, // #
    {0x18,0x3e,0x60,0x3c,0x06,0x7c,0x18,0x00}, // $
    {0x00,0xc6,0xcc,0x18,0x30,0x66,0xc6,0x00}, // %
    {0x38,0x6c,0x38,0x76,0xdc,0xcc,0x76,0x00}, // &
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // '
    {0x0c,0x18,0x30,0x30,0x30,0x18,0x0c,0x00}, // (
    {0x30,0x18,0x0c,0x0c,0x0c,0x18,0x30,0x00}, // )
    {0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00}, // *
    {0x00,0x18,0x18,0x7e,0x18,0x18,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ,
    {0x00,0x00,0x00,0xfe,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // .
    {0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x00}, // /
    {0x3c,0x66,0x6e,0x7e,0x76,0x66,0x3c,0x00}, // 0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x3c,0x00}, // 1
    {0x3c,0x66,0x06,0x0c,0x18,0x30,0x7e,0x00}, // 2
    {0x3c,0x66,0x06,0x1c,0x06,0x66,0x3c,0x00}, // 3
    {0x0c,0x1c,0x3c,0x6c,0xfe,0x0c,0x0c,0x00}, // 4
    {0x7e,0x60,0x7c,0x06,0x06,0x66,0x3c,0x00}, // 5
    {0x1c,0x30,0x60,0x7c,0x66,0x66,0x3c,0x00}, // 6
    {0x7e,0x06,0x0c,0x18,0x30,0x30,0x30,0x00}, // 7
    {0x3c,0x66,0x66,0x3c,0x66,0x66,0x3c,0x00}, // 8
    {0x3c,0x66,0x66,0x3e,0x06,0x0c,0x38,0x00}, // 9
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // :
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // ;
    {0x0c,0x18,0x30,0x60,0x30,0x18,0x0c,0x00}, // <
    {0x00,0x00,0x7e,0x00,0x7e,0x00,0x00,0x00}, // =
    {0x30,0x18,0x0c,0x06,0x0c,0x18,0x30,0x00}, // >
    {0x3c,0x66,0x06,0x0c,0x18,0x00,0x18,0x00}, // ?
    {0x3c,0x66,0x6e,0x6e,0x60,0x3e,0x00,0x00}, // @
    {0x18,0x3c,0x66,0x66,0x7e,0x66,0x66,0x00}, // A
    {0x7c,0x66,0x66,0x7c,0x66,0x66,0x7c,0x00}, // B
    {0x3c,0x66,0x60,0x60,0x60,0x66,0x3c,0x00}, // C
    {0x78,0x6c,0x66,0x66,0x66,0x6c,0x78,0x00}, // D
    {0x7e,0x60,0x60,0x7c,0x60,0x60,0x7e,0x00}, // E
    {0x7e,0x60,0x60,0x7c,0x60,0x60,0x60,0x00}, // F
    {0x3c,0x66,0x60,0x6e,0x66,0x66,0x3c,0x00}, // G
    {0x66,0x66,0x66,0x7e,0x66,0x66,0x66,0x00}, // H
    {0x3c,0x18,0x18,0x18,0x18,0x18,0x3c,0x00}, // I
    {0x1e,0x0c,0x0c,0x0c,0x0c,0x6c,0x38,0x00}, // J
    {0x66,0x6c,0x78,0x70,0x78,0x6c,0x66,0x00}, // K
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7e,0x00}, // L
    {0x63,0x77,0x7f,0x6b,0x63,0x63,0x63,0x00}, // M
    {0x66,0x66,0x76,0x7e,0x6e,0x66,0x66,0x00}, // N
    {0x3c,0x66,0x66,0x66,0x66,0x66,0x3c,0x00}, // O
    {0x7c,0x66,0x66,0x7c,0x60,0x60,0x60,0x00}, // P
    {0x3c,0x66,0x66,0x66,0x66,0x3c,0x0e,0x02}, // Q
    {0x7c,0x66,0x66,0x7c,0x78,0x6c,0x66,0x00}, // R
    {0x3c,0x66,0x30,0x18,0x0c,0x66,0x3c,0x00}, // S
    {0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // T
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x00}, // U
    {0x66,0x66,0x66,0x66,0x66,0x3c,0x18,0x00}, // V
    {0x63,0x63,0x63,0x6b,0x7f,0x77,0x63,0x00}, // W
    {0x66,0x66,0x3c,0x18,0x3c,0x66,0x66,0x00}, // X
    {0x66,0x66,0x66,0x3c,0x18,0x18,0x18,0x00}, // Y
    {0x7e,0x06,0x0c,0x18,0x30,0x60,0x7e,0x00}, // Z
    {0x3c,0x30,0x30,0x30,0x30,0x30,0x3c,0x00}, // [
    {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x00}, // backslash
    {0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00}, // ]
    {0x10,0x38,0x6c,0xc6,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x00}, // _
    {0x30,0x18,0x0c,0x00,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x3c,0x06,0x3e,0x66,0x3e,0x00}, // a
    {0x60,0x60,0x7c,0x66,0x66,0x66,0x7c,0x00}, // b
    {0x00,0x00,0x3c,0x60,0x60,0x66,0x3c,0x00}, // c
    {0x06,0x06,0x3e,0x66,0x66,0x66,0x3e,0x00}, // d
    {0x00,0x00,0x3c,0x66,0x7e,0x60,0x3c,0x00}, // e
    {0x1c,0x30,0x7c,0x30,0x30,0x30,0x30,0x00}, // f
    {0x00,0x00,0x3e,0x66,0x66,0x3e,0x06,0x3c}, // g
    {0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x00}, // h
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3c,0x00}, // i
    {0x06,0x00,0x1e,0x06,0x06,0x66,0x3c,0x00}, // j
    {0x60,0x60,0x66,0x6c,0x78,0x6c,0x66,0x00}, // k
    {0x30,0x30,0x30,0x30,0x30,0x30,0x1c,0x00}, // l
    {0x00,0x00,0x66,0x7f,0x7f,0x6b,0x63,0x00}, // m
    {0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x00}, // n
    {0x00,0x00,0x3c,0x66,0x66,0x66,0x3c,0x00}, // o
    {0x00,0x00,0x7c,0x66,0x66,0x7c,0x60,0x60}, // p
    {0x00,0x00,0x3e,0x66,0x66,0x3e,0x06,0x06}, // q
    {0x00,0x00,0x7c,0x66,0x60,0x60,0x60,0x00}, // r
    {0x00,0x00,0x3e,0x60,0x3c,0x06,0x7c,0x00}, // s
    {0x30,0x30,0x7c,0x30,0x30,0x30,0x1c,0x00}, // t
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3e,0x00}, // u
    {0x00,0x00,0x66,0x66,0x66,0x3c,0x18,0x00}, // v
    {0x00,0x00,0x63,0x6b,0x7f,0x7f,0x36,0x00}, // w
    {0x00,0x00,0x66,0x3c,0x18,0x3c,0x66,0x00}, // x
    {0x00,0x00,0x66,0x66,0x66,0x3e,0x06,0x3c}, // y
    {0x00,0x00,0x7e,0x0c,0x18,0x30,0x7e,0x00}, // z
    {0x0c,0x18,0x18,0x70,0x18,0x18,0x0c,0x00}, // {
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // |
    {0x30,0x18,0x18,0x0e,0x18,0x18,0x30,0x00}, // }
    {0x00,0x00,0x4c,0xb2,0x00,0x00,0x00,0x00}, // ~
};

static void * texture_mem = NULL;

void ui_init(int width, int height) {
    ui_width = (width > 0) ? width : 1280;
    ui_height = (height > 0) ? height : 720;
    scale_x = (float)ui_width / 1280.0f;
    scale_y = (float)ui_height / 720.0f;
    scale_font = (scale_x < scale_y) ? scale_x : scale_y;

    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);
    if (sysMutexCreate(&log_mutex, &attr) == 0) {
        log_mutex_initialized = 1;
    }

    // Standard-definition output (480i/p, 576i/p): crops the picture and has
    // no use for a 720p stream.  Defaults only; config.ini still wins.
    ui_output_is_sd = (ui_width < 1280);
    if (ui_output_is_sd) {
        ui_ovs_x = 90;
        ui_ovs_y = 92;
    }

    // Load persisted Host IP and stream settings from HDD
    ui_load_settings();

    if (ui_output_is_sd && !ui_res_configured) ui_res_idx = RES_IDX_960;
    char cfg_log[96];
    snprintf(cfg_log, sizeof(cfg_log), "Config: Target Host [%s]", target_ip_str);
    ui_push_log(cfg_log);

    // Register sysutil callback on slot 1 for OSK keyboard lifecycle
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT1, ui_osk_callback, NULL);

    if (sysThreadCreate(&ui_thread, ui_loop, 0, 500, 0x10000,
                        THREAD_JOINABLE, "UI Thread") == 0) {
        ui_thread_started = 1;
    } else {
        ui_running = 0;
    }
}

void ui_push_log(const char *msg) {
    if (!msg) return;
    if (log_mutex_initialized) sysMutexLock(log_mutex, 0);
    if (log_count < MAX_LOG_LINES) {
        strncpy(log_buffer[log_count], msg, MAX_LOG_WIDTH - 1);
        log_buffer[log_count][MAX_LOG_WIDTH - 1] = '\0';
        log_count++;
    } else {
        // Shift buffer
        for (int i = 0; i < MAX_LOG_LINES - 1; i++) {
            memcpy(log_buffer[i], log_buffer[i + 1], MAX_LOG_WIDTH);
        }
        strncpy(log_buffer[MAX_LOG_LINES - 1], msg, MAX_LOG_WIDTH - 1);
        log_buffer[MAX_LOG_LINES - 1][MAX_LOG_WIDTH - 1] = '\0';
    }
    if (log_mutex_initialized) sysMutexUnlock(log_mutex);
}

static void draw_background_gradient() {
    // 1. Base Dark Gray Background (#303030 to #242424)
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    tiny3d_VertexPos(0, 0, 65535);
    tiny3d_VertexFcolor(0.188f, 0.188f, 0.188f, 1.0f); // #303030
    tiny3d_VertexPos(ui_width, 0, 65535);
    tiny3d_VertexFcolor(0.188f, 0.188f, 0.188f, 1.0f);
    tiny3d_VertexPos(0, ui_height * 0.72f, 65535);
    tiny3d_VertexFcolor(0.141f, 0.141f, 0.141f, 1.0f); // #242424
    tiny3d_VertexPos(ui_width, ui_height * 0.72f, 65535);
    tiny3d_VertexFcolor(0.141f, 0.141f, 0.141f, 1.0f);
    tiny3d_End();

    // 2. Titlebar Header Bar (#3F51B5 Material Indigo Blue)
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    tiny3d_VertexPos(0, 0, 65535);
    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f); // #3F51B5
    tiny3d_VertexPos(ui_width, 0, 65535);
    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
    tiny3d_VertexPos(0, SY(64), 65535);
    tiny3d_VertexFcolor(0.200f, 0.260f, 0.620f, 1.0f);
    tiny3d_VertexPos(ui_width, SY(64), 65535);
    tiny3d_VertexFcolor(0.200f, 0.260f, 0.620f, 1.0f);
    tiny3d_End();

    // 3. Titlebar Bottom Accent Line (#1A237E Deep Indigo)
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    tiny3d_VertexPos(0, SY(64), 65535);
    tiny3d_VertexFcolor(0.102f, 0.137f, 0.494f, 1.0f);
    tiny3d_VertexPos(ui_width, SY(64), 65535);
    tiny3d_VertexFcolor(0.102f, 0.137f, 0.494f, 1.0f);
    tiny3d_VertexPos(0, SY(67), 65535);
    tiny3d_VertexFcolor(0.102f, 0.137f, 0.494f, 1.0f);
    tiny3d_VertexPos(ui_width, SY(67), 65535);
    tiny3d_VertexFcolor(0.102f, 0.137f, 0.494f, 1.0f);
    tiny3d_End();
}

// FreeType font engine state
static FT_Library ft_library = NULL;
static FT_Face ft_face = NULL;
static int font_is_ttf = 0;

static void render_ps_button_glyph(u8 chr, u8 *bitmap, short *w, short *h, short *y_correction) {
    *w = 26;
    *h = 28;
    *y_correction = 2; // Aligned with baseline
    
    float cx = 13.0f;
    float cy = 14.0f;
    float r_outer = 11.5f;
    float r_inner = 9.5f;
    
    for (int y = 0; y < 28; y++) {
        for (int x = 0; x < 26; x++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float alpha = 0.0f;
            
            if (chr == 1) {
                // Cross (✕) Button Badge
                if (d <= r_outer && d >= r_inner) {
                    float edge = (d > r_outer - 0.75f) ? (r_outer - d) / 0.75f : ((d < r_inner + 0.75f) ? (d - r_inner) / 0.75f : 1.0f);
                    if (edge > 0.0f) alpha = fmaxf(alpha, edge * 220.0f);
                }
                if (d < r_inner - 0.8f) {
                    float dist_d1 = fabsf(dx - dy) / 1.4142f;
                    float dist_d2 = fabsf(dx + dy) / 1.4142f;
                    float line_dist = fminf(dist_d1, dist_d2);
                    if (line_dist < 1.6f && d < 6.5f) {
                        float cross_alpha = (line_dist < 0.9f) ? 1.0f : (1.6f - line_dist) / 0.7f;
                        alpha = fmaxf(alpha, cross_alpha * 255.0f);
                    }
                }
            } else if (chr == 2) {
                // Circle (◯) Button Badge
                if (d <= r_outer && d >= r_inner) {
                    float edge = (d > r_outer - 0.75f) ? (r_outer - d) / 0.75f : ((d < r_inner + 0.75f) ? (d - r_inner) / 0.75f : 1.0f);
                    if (edge > 0.0f) alpha = fmaxf(alpha, edge * 220.0f);
                }
                float ir = 5.2f;
                float dist_ir = fabsf(d - ir);
                if (dist_ir < 1.6f) {
                    float ring_alpha = (dist_ir < 0.9f) ? 1.0f : (1.6f - dist_ir) / 0.7f;
                    alpha = fmaxf(alpha, ring_alpha * 255.0f);
                }
            } else if (chr == 3) {
                // Triangle (△) Button Badge
                if (d <= r_outer && d >= r_inner) {
                    float edge = (d > r_outer - 0.75f) ? (r_outer - d) / 0.75f : ((d < r_inner + 0.75f) ? (d - r_inner) / 0.75f : 1.0f);
                    if (edge > 0.0f) alpha = fmaxf(alpha, edge * 220.0f);
                }
                float ty = dy + 1.0f;
                float dist_bottom = fabsf(ty - 4.0f);
                float dist_left = fabsf(dx * 0.866f + ty * 0.5f + 1.5f);
                float dist_right = fabsf(-dx * 0.866f + ty * 0.5f + 1.5f);
                if (ty <= 4.2f && ty >= -5.5f && fabsf(dx) <= (ty + 5.5f) * 0.65f + 1.2f) {
                    float tri_dist = fminf(dist_bottom, fminf(dist_left, dist_right));
                    if (tri_dist < 1.5f) {
                        float tri_alpha = (tri_dist < 0.8f) ? 1.0f : (1.5f - tri_dist) / 0.7f;
                        alpha = fmaxf(alpha, tri_alpha * 255.0f);
                    }
                }
            } else if (chr == 4) {
                // Square (◻) Button Badge
                if (d <= r_outer && d >= r_inner) {
                    float edge = (d > r_outer - 0.75f) ? (r_outer - d) / 0.75f : ((d < r_inner + 0.75f) ? (d - r_inner) / 0.75f : 1.0f);
                    if (edge > 0.0f) alpha = fmaxf(alpha, edge * 220.0f);
                }
                float max_d = fmaxf(fabsf(dx), fabsf(dy));
                float sq_dist = fabsf(max_d - 4.8f);
                if (sq_dist < 1.5f && max_d <= 5.5f) {
                    float sq_alpha = (sq_dist < 0.8f) ? 1.0f : (1.5f - sq_dist) / 0.7f;
                    alpha = fmaxf(alpha, sq_alpha * 255.0f);
                }
            } else if (chr == 5) {
                // D-Pad Up/Down (↕) Icon
                if (fabsf(dx) <= 2.2f && fabsf(dy) <= 8.5f) {
                    alpha = 240.0f;
                }
                if (dy < -2.0f && dy >= -9.5f) {
                    float arrow_w = (dy + 9.5f) * 0.9f;
                    if (fabsf(dx) <= arrow_w + 0.8f) {
                        alpha = 255.0f;
                    }
                }
                if (dy > 2.0f && dy <= 9.5f) {
                    float arrow_w = (9.5f - dy) * 0.9f;
                    if (fabsf(dx) <= arrow_w + 0.8f) {
                        alpha = 255.0f;
                    }
                }
            } else if (chr == 6) {
                // Heart (♥) Symbol
                float d_left = sqrtf((dx + 4.0f) * (dx + 4.0f) + (dy + 2.5f) * (dy + 2.5f));
                float d_right = sqrtf((dx - 4.0f) * (dx - 4.0f) + (dy + 2.5f) * (dy + 2.5f));
                if (d_left <= 4.8f) {
                    float edge = (d_left > 4.0f) ? (4.8f - d_left) / 0.8f : 1.0f;
                    alpha = fmaxf(alpha, edge * 255.0f);
                }
                if (d_right <= 4.8f) {
                    float edge = (d_right > 4.0f) ? (4.8f - d_right) / 0.8f : 1.0f;
                    alpha = fmaxf(alpha, edge * 255.0f);
                }
                if (dy >= -2.5f && dy <= 8.5f) {
                    float max_w = (8.5f - dy) * 0.77f;
                    if (fabsf(dx) <= max_w + 0.8f) {
                        float edge = (fabsf(dx) > max_w) ? (max_w + 0.8f - fabsf(dx)) / 0.8f : 1.0f;
                        alpha = fmaxf(alpha, edge * 255.0f);
                    }
                }
            }
            
            if (alpha > 255.0f) alpha = 255.0f;
            bitmap[y * 32 + x] = (u8)alpha;
        }
    }
}

static void ttf_render_callback(u8 chr, u8 *bitmap, short *w, short *h, short *y_correction) {
    memset(bitmap, 0, 32 * 32);
    *w = 0;
    *h = 0;
    *y_correction = 0;
    
    // Check for Custom Glyph slots (1: Cross, 2: Circle, 3: Triangle, 4: Square, 5: D-Pad, 6: Heart)
    if (chr >= 1 && chr <= 6) {
        render_ps_button_glyph(chr, bitmap, w, h, y_correction);
        return;
    }
    
    if (!ft_face) return;
    
    // Custom spacing for space character
    if (chr == ' ') {
        *w = 10;
        *h = 1;
        *y_correction = 0;
        return;
    }
    
    FT_UInt glyph_index = FT_Get_Char_Index(ft_face, (FT_ULong)chr);
    if (glyph_index == 0) return;
    
    if (FT_Load_Glyph(ft_face, glyph_index, FT_LOAD_DEFAULT)) return;
    if (FT_Render_Glyph(ft_face->glyph, FT_RENDER_MODE_NORMAL)) return;
    
    FT_GlyphSlot slot = ft_face->glyph;
    int bw = slot->bitmap.width;
    int bh = slot->bitmap.rows;
    if (bw > 32) bw = 32;
    if (bh > 32) bh = 32;
    
    *w = (short)(slot->advance.x >> 6);
    if (*w <= 0) *w = (short)(bw + 2);
    *h = (short)bh;
    *y_correction = (short)(26 - slot->bitmap_top);
    if (*y_correction < 0) *y_correction = 0;
    
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            u8 val = slot->bitmap.buffer[y * slot->bitmap.pitch + x];
            bitmap[y * 32 + x] = val;
        }
    }
}

static void ui_init_fonts() {
    ResetFont();
    font_is_ttf = 0;
    
    // PlayStation 3 internal system fonts (ordered by preference)
    static const char *font_candidates[] = {
        "/dev_flash/data/font/SCE-PS3-RD-R-LATIN.TTF",
        "/dev_flash/data/font/SCE-PS3-SR-R-LATIN.TTF",
        "/dev_flash/data/font/SCE-PS3-VR-R-LATIN.TTF",
        "/dev_flash/data/font/SCE-PS3-DH-R-CGB.TTF",
        NULL
    };
    
    if (FT_Init_FreeType(&ft_library) == 0) {
        for (int i = 0; font_candidates[i] != NULL; i++) {
            if (FT_New_Face(ft_library, font_candidates[i], 0, &ft_face) == 0) {
                FT_Set_Pixel_Sizes(ft_face, 0, 30);
                
                texture_mem = tiny3d_AllocTexture(1024 * 1024);
                if (texture_mem) {
                    AddFontFromTTF((u8 *)texture_mem, 1, 127, 32, 32, ttf_render_callback);
                    font_is_ttf = 1;
                    char log_buf[96];
                    snprintf(log_buf, sizeof(log_buf), "Font: Loaded FreeType TTF (%s)", font_candidates[i]);
                    ui_push_log(log_buf);
                    break;
                }
            }
        }
    }
    
    // Fallback to built-in bitmap font if TTF failed or files unavailable
    if (!font_is_ttf) {
        if (!texture_mem) {
            texture_mem = tiny3d_AllocTexture(64 * 1024);
        }
        if (texture_mem) {
            AddFontFromBitmapArray((u8 *)font_8x8_basic, (u8 *)texture_mem, 32, 127, 8, 8, 1, BIT7_FIRST_PIXEL);
            ui_push_log("Font: Loaded fallback 8x8 bitmap font");
        }
    }
    
    SetCurrentFont(0);
    SetFontSize(SF(16), SF(16));
    SetFontColor(0xffffffff, 0x00000000);
}

// ---------------------------------------------------------------------------
// Settings page: a scrolling list, one row per SR_* id, grouped.
// ---------------------------------------------------------------------------
static const char *settings_labels[SETTINGS_ITEM_COUNT] = {
    "Target FPS:", "Resolution:", "Target Bitrate:", "Decoder Output:", "Decode Speed:",
    "Presentation:", "Refresh Rate:", "Decoder SPUs:",
    "Packet Size:", "Intra Refresh:", "Virtual Display:", "Quit App On Exit:", "Audio:",
    "Mouse Mode:", "Rumble:", "Triggers:",
    "VSync Mode:", "Picture Width:", "Picture Height:", "Shift Horizontal:", "Shift Vertical:",
    "Stats Overlay:", "Verbose Logging:",
    ""
};
static const char *settings_group_names[] = {"Video Settings", "Network & Host", "Controls", "Display", "Settings"};
static const unsigned char settings_group[SETTINGS_ITEM_COUNT] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1,
    2, 2, 2,
    3, 3, 3, 3, 3, 3, 3,
    4
};

static void settings_clamp_scroll(void) {
    if (active_settings_item < settings_scroll) settings_scroll = active_settings_item;
    if (active_settings_item >= settings_scroll + SETTINGS_VISIBLE)
        settings_scroll = active_settings_item - SETTINGS_VISIBLE + 1;
    if (settings_scroll > SETTINGS_ITEM_COUNT - SETTINGS_VISIBLE) settings_scroll = SETTINGS_ITEM_COUNT - SETTINGS_VISIBLE;
    if (settings_scroll < 0) settings_scroll = 0;
}

static void settings_value_text(int row, char *out, size_t n) {
    switch (row) {
    case SR_FPS: {
        int x100 = ui_get_refresh_x100();
        if (x100 % 100) snprintf(out, n, "%d.%02d FPS", x100 / 100, x100 % 100);
        else snprintf(out, n, "%d FPS", ui_fps);
        break;
    }
    case SR_RES:
        snprintf(out, n, "%dx%d%s", ui_get_stream_width(), ui_get_stream_height(),
                 (ui_mode_mb_rate() > 522240u) ? "  (OVER Level 4.2)"
                 : (ui_mode_mb_rate() > 245760u) ? "  (needs Level 4.2)"
                 : (ui_res_idx == RES_IDX_960) ? "  (fastest)" : "");
        break;
    case SR_BITRATE: {
        int kbps = ui_bitrate_options[ui_bitrate_idx];
        if (kbps % 1000 == 0) snprintf(out, n, "%d Mbps", kbps / 1000);
        else snprintf(out, n, "%.1f Mbps", (float)kbps / 1000.0f);
        break;
    }
    case SR_PACKET:
        snprintf(out, n, "%d bytes - %s", ui_packet_size_options[ui_packet_size_idx],
                 (ui_packet_size_options[ui_packet_size_idx] == 1024) ? "original" : "fewer syscalls");
        break;
    case SR_PIXFMT:
        snprintf(out, n, "%s", ui_get_pixel_format() ? "YUV420 (GPU convert, faster)"
                               : ui_pixfmt ? "ARGB32 (960 wide cannot use YUV)" : "ARGB32 (original)");
        break;
    case SR_DEBLOCK:
        snprintf(out, n, "%s", (ui_no_deblock == 1) ? "FAST (no deblocking)"
                               : (ui_no_deblock == 0) ? "QUALITY (normal)" : "AUTO (fast at 1080p50/60)");
        break;
    case SR_PRESENT:
        snprintf(out, n, "%s", (ui_low_latency == 2) ? "NEWEST (lowest lag)"
                               : (ui_low_latency == 0) ? "SMOOTH" : "BALANCED");
        break;
    case SR_NTSC:
        snprintf(out, n, "%s", ui_ntsc_rate ? "59.94 Hz (NTSC)" : "Whole numbers (60 Hz)");
        break;
    case SR_SPUS:
        if (ui_vdec_spus == 0) snprintf(out, n, "AUTO");
        else snprintf(out, n, "%d", ui_vdec_spus);
        break;
    case SR_INTRA:      snprintf(out, n, "%s", ui_intra_refresh ? "ON" : "OFF"); break;
    case SR_VDISPLAY:   snprintf(out, n, "%s", ui_virtual_display ? "ON (Apollo / Vibepollo)" : "OFF"); break;
    case SR_QUITEXIT:   snprintf(out, n, "%s", ui_quit_on_exit ? "YES (close it on the host)" : "NO (leave running)"); break;
    case SR_AUDIO:
        // HQ: moonlight-common-c asks for high-quality surround at 15 Mbps and up.
        snprintf(out, n, "%s%s", (ui_audio_channels == 8) ? "7.1 SURROUND"
                                 : (ui_audio_channels == 6) ? "5.1 SURROUND" : "STEREO",
                 (ui_audio_channels > 2 && ui_get_bitrate() >= 15000) ? " (high quality)" : "");
        break;
    case SR_MOUSE:      snprintf(out, n, "%s", (ui_mouse_mode == 0) ? "GAME (Relative / 3D)" : "DESKTOP (Absolute / 1:1)"); break;
    case SR_RUMBLE:     snprintf(out, n, "%s", ui_rumble ? "ON" : "OFF"); break;
    case SR_TRIGGERS:   snprintf(out, n, "%s", ui_trigger_mode ? "ANALOG (pressure)" : "DIGITAL (on / off)"); break;
    case SR_VSYNC:      snprintf(out, n, "%s", ui_vsync ? "ON (Smooth 60Hz)" : "OFF (Low Latency)"); break;
    case SR_OVS_X:      snprintf(out, n, "%d%%", ui_ovs_x); break;
    case SR_OVS_Y:      snprintf(out, n, "%d%%", ui_ovs_y); break;
    case SR_OVS_XOFF:   snprintf(out, n, "%+d", ui_ovs_xoff); break;
    case SR_OVS_YOFF:   snprintf(out, n, "%+d", ui_ovs_yoff); break;
    case SR_STATS:      snprintf(out, n, "%s", show_stats ? "ON" : "OFF"); break;
    case SR_VERBOSE:    snprintf(out, n, "%s", ui_verbose ? "ON" : "OFF"); break;
    default:            out[0] = '\0'; break;
    }
}

static int settings_step(int v, int dir, int count) { return (v + dir + count) % count; }
static int settings_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Apply one press (dir = +1 / -1) to a row.  Returns 1 if a value changed.
static int settings_change(int row, int dir) {
    switch (row) {
    case SR_FPS: {
        int i = 0;
        for (int k = 0; k < NUM_FPS_OPTIONS; k++) if (ui_fps_options[k] == ui_fps) i = k;
        ui_fps = ui_fps_options[settings_step(i, dir, NUM_FPS_OPTIONS)];
        return 1;
    }
    case SR_RES:     ui_res_idx = settings_step(ui_res_idx, dir, NUM_RES_OPTIONS); ui_res_configured = 1; return 1;
    case SR_BITRATE: ui_bitrate_idx = settings_step(ui_bitrate_idx, dir, NUM_BITRATE_OPTIONS); return 1;
    case SR_PACKET:  ui_packet_size_idx = settings_step(ui_packet_size_idx, dir, NUM_PACKET_SIZE_OPTIONS); return 1;
    case SR_PIXFMT:  ui_pixfmt = !ui_pixfmt; return 1;
    case SR_DEBLOCK: // AUTO -> QUALITY -> FAST -> AUTO
        ui_no_deblock = (dir > 0) ? ((ui_no_deblock == 1) ? -1 : ui_no_deblock + 1)
                                  : ((ui_no_deblock == -1) ? 1 : ui_no_deblock - 1);
        return 1;
    case SR_PRESENT: ui_low_latency = settings_step(ui_low_latency, dir, 3); return 1;
    case SR_NTSC:    ui_ntsc_rate = !ui_ntsc_rate; return 1;
    case SR_SPUS:    ui_vdec_spus = settings_step(ui_vdec_spus, dir, 7); return 1; // 0 = auto, 1..6
    case SR_INTRA:   ui_intra_refresh = !ui_intra_refresh; return 1;
    case SR_VDISPLAY: ui_virtual_display = !ui_virtual_display; return 1;
    case SR_QUITEXIT: ui_quit_on_exit = !ui_quit_on_exit; return 1;
    case SR_AUDIO: {
        static const int ch[] = {2, 6, 8};
        int i = 0;
        for (int k = 0; k < 3; k++) if (ch[k] == ui_audio_channels) i = k;
        ui_audio_channels = ch[settings_step(i, dir, 3)];
        return 1;
    }
    case SR_MOUSE:   ui_mouse_mode = !ui_mouse_mode; return 1;
    case SR_RUMBLE:  ui_rumble = !ui_rumble; return 1;
    case SR_TRIGGERS: ui_trigger_mode = !ui_trigger_mode; return 1;
    case SR_VSYNC:
        ui_vsync = !ui_vsync;
        gcmSetFlipMode(ui_vsync ? GCM_FLIP_VSYNC : GCM_FLIP_HSYNC);
        return 1;
    case SR_OVS_X:    ui_ovs_x = settings_clamp(ui_ovs_x + 2 * dir, OVS_SCALE_MIN, 100); ui_ovs_configured = 1; return 1;
    case SR_OVS_Y:    ui_ovs_y = settings_clamp(ui_ovs_y + 2 * dir, OVS_SCALE_MIN, 100); ui_ovs_configured = 1; return 1;
    case SR_OVS_XOFF: ui_ovs_xoff = settings_clamp(ui_ovs_xoff + 2 * dir, -OVS_OFF_LIMIT, OVS_OFF_LIMIT); ui_ovs_configured = 1; return 1;
    case SR_OVS_YOFF: ui_ovs_yoff = settings_clamp(ui_ovs_yoff + 2 * dir, -OVS_OFF_LIMIT, OVS_OFF_LIMIT); ui_ovs_configured = 1; return 1;
    case SR_STATS:   show_stats = !show_stats; return 1;
    case SR_VERBOSE: ui_verbose = !ui_verbose; return 1;
    default: return 0;
    }
}

// Overscan: shrink the whole picture toward the centre and shift it.  Applied
// to the 2D viewport before anything is drawn, so menus and the video quad move
// together.  At 100% / 0 this is the identity (the viewport tiny3d already used).
static void ui_apply_overscan(void) {
    float sx = (float)ui_ovs_x / 100.0f, sy = (float)ui_ovs_y / 100.0f;
    float px = ((float)ui_width * (1.0f - sx)) * 0.5f + SX(ui_ovs_xoff);
    float py = ((float)ui_height * (1.0f - sy)) * 0.5f + SY(ui_ovs_yoff);
    tiny3d_UserViewport(1, px, py, sx, sy, 1.0f, 1.0f);
}

static void ui_loop(void *arg) {
    (void)arg;
    ps3_pad_state_t pad;
    
    tiny3d_Init(1024 * 1024); // 1MB vertex buffer
    ui_init_fonts();
    
    // Enable Alpha Test and Blending to eliminate solid black texture boxes around font glyphs
    tiny3d_AlphaTest(1, 0, TINY3D_ALPHA_FUNC_GREATER);
    tiny3d_BlendFunc(1, 
        TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA | TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA,
        TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA | TINY3D_BLEND_FUNC_DST_ALPHA_ONE_MINUS_SRC_ALPHA,
        TINY3D_BLEND_RGB_FUNC_ADD | TINY3D_BLEND_ALPHA_FUNC_ADD);
    
    while (ui_running) {
        // Pump sysutil event callbacks to service OSK and GameOS events
        sysUtilCheckCallback();
        ps3input_get_data(&pad);
        
        tiny3d_Clear(0x303030ff, TINY3D_CLEAR_ALL);
        
        // Handle input for UI menu states when OSK dialog is not actively capturing input
        if (ui_state == UI_STATE_IP_ENTRY) {
            if (!osk_active && !msg_dialog_active) {
                // The quit row vanishes when the host stops reporting an app.
                if (active_main_item >= MAIN_MENU_ITEM_COUNT) active_main_item = MAIN_MENU_ITEM_COUNT - 1;
                // Vertical navigation across main menu rows
                if (pad.buttons_pressed & UP_FLAG) {
                    active_main_item = (active_main_item + MAIN_MENU_ITEM_COUNT - 1) % MAIN_MENU_ITEM_COUNT;
                }
                if (pad.buttons_pressed & DOWN_FLAG) {
                    active_main_item = (active_main_item + 1) % MAIN_MENU_ITEM_COUNT;
                }
                
                // Action handling per main menu item
                if (active_main_item == 0) {
                    // Host IP row: Open native OSK keyboard on Cross, Left, or Right
                    if ((pad.buttons_pressed & A_FLAG) || (pad.buttons_pressed & LEFT_FLAG) || (pad.buttons_pressed & RIGHT_FLAG)) {
                        ui_reset_host_selection();
                        ui_state = UI_STATE_DISCOVERY;
                    }
                } else if (active_main_item == 1) {
                    // Settings Submenu: Enter stream configuration menu
                    if ((pad.buttons_pressed & A_FLAG) || (pad.buttons_pressed & RIGHT_FLAG)) {
                        ui_state = UI_STATE_SETTINGS;
                        active_settings_item = 0; settings_scroll = 0;
                    }
                } else if (active_main_item == 2) {
                    // Connect / Pair action button
                    if (pad.buttons_pressed & A_FLAG) {
                        ui_state = UI_STATE_PAIRING;
                    }
                } else if (active_main_item == 3) {
                    // Quit the app the host says is running (main thread sends /cancel)
                    if (pad.buttons_pressed & A_FLAG) quit_request = 1;
                }

                // START button initiates connection immediately from anywhere in main menu
                if (pad.buttons_pressed & PLAY_FLAG) {
                    ui_state = UI_STATE_PAIRING;
                }

                // Circle button opens native PS3 confirmation dialog to exit to XMB
                if (pad.buttons_pressed & B_FLAG) {
                    ui_open_exit_dialog();
                }
            }
        } else if (ui_state == UI_STATE_SETTINGS) {
            // Vertical navigation across the scrolling settings list
            if (pad.buttons_pressed & UP_FLAG) {
                active_settings_item = (active_settings_item + SETTINGS_ITEM_COUNT - 1) % SETTINGS_ITEM_COUNT;
            }
            if (pad.buttons_pressed & DOWN_FLAG) {
                active_settings_item = (active_settings_item + 1) % SETTINGS_ITEM_COUNT;
            }
            // L1 / R1 jump a page
            if (pad.buttons_pressed & LB_FLAG) {
                active_settings_item -= SETTINGS_VISIBLE - 1;
                if (active_settings_item < 0) active_settings_item = 0;
            }
            if (pad.buttons_pressed & RB_FLAG) {
                active_settings_item += SETTINGS_VISIBLE - 1;
                if (active_settings_item >= SETTINGS_ITEM_COUNT) active_settings_item = SETTINGS_ITEM_COUNT - 1;
            }
            settings_clamp_scroll();

            if (active_settings_item == SR_BACK) {
                if (pad.buttons_pressed & A_FLAG) {
                    ui_save_settings();
                    ui_state = UI_STATE_IP_ENTRY;
                }
            } else {
                int dir = 0;
                if ((pad.buttons_pressed & A_FLAG) || (pad.buttons_pressed & RIGHT_FLAG)) dir = 1;
                else if (pad.buttons_pressed & LEFT_FLAG) dir = -1;
                if (dir && settings_change(active_settings_item, dir)) ui_save_settings();
            }

            // Circle button returns to main menu from anywhere in settings
            if (pad.buttons_pressed & B_FLAG) {
                ui_save_settings();
                ui_state = UI_STATE_IP_ENTRY;
            }
        } else if (ui_state == UI_STATE_PAIRING) {
            // Circle button to cancel pairing attempt
            if (pad.buttons_pressed & B_FLAG) {
                ui_state = UI_STATE_IP_ENTRY;
            }
        } else if (ui_state == UI_STATE_APPLIST) {
            if (current_app_list.count > 0) {
                if (pad.buttons_pressed & UP_FLAG) {
                    active_app_idx = (active_app_idx + current_app_list.count - 1) % current_app_list.count;
                }
                if (pad.buttons_pressed & DOWN_FLAG) {
                    active_app_idx = (active_app_idx + 1) % current_app_list.count;
                }
                if (pad.buttons_pressed & A_FLAG) {
                    app_selection_confirmed = 1;
                }
            }
            // Circle button returns to main menu
            if (pad.buttons_pressed & B_FLAG) {
				app_selection_confirmed = 0;
				ui_state = UI_STATE_IP_ENTRY;
			}
        } else if (ui_state == UI_STATE_DISCOVERY) {
            if (discovery_scanned) {
                int total_rows = discovered_host_count + 1;
                if (pad.buttons_pressed & UP_FLAG)
                    active_host_idx = (active_host_idx + total_rows - 1) % total_rows;
                if (pad.buttons_pressed & DOWN_FLAG)
                    active_host_idx = (active_host_idx + 1) % total_rows;
                if (pad.buttons_pressed & A_FLAG) {
                    if (active_host_idx == discovered_host_count)
                        manual_entry_requested = 1;
                    else
                        host_selection_confirmed = 1;
                }
            }
            if (pad.buttons_pressed & B_FLAG) {
                ui_reset_host_selection();
                ui_state = UI_STATE_IP_ENTRY;
            }
        }

        if (show_stats) {
            u64 now = sysGetSystemTime();
            if (last_ui_time == 0) last_ui_time = now;
            if (now - last_ui_time >= 1000000) {
                ui_fps_actual = frames_drawn_this_sec;
                frames_drawn_this_sec = 0;
                last_ui_time = now;
            }
            frames_drawn_this_sec++;
        }

        // 1. Draw UI / Video (Top 70%)
        ui_apply_overscan();
        tiny3d_UserViewportSurface(1, (float)ui_width, (float)ui_height);
        tiny3d_Project2D();
        
        // Ensure transparent alpha blending is active for all 2D text and menu overlays
        tiny3d_AlphaTest(1, 0, TINY3D_ALPHA_FUNC_GREATER);
        tiny3d_BlendFunc(1, 
            TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA | TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA,
            TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA | TINY3D_BLEND_FUNC_DST_ALPHA_ONE_MINUS_SRC_ALPHA,
            TINY3D_BLEND_RGB_FUNC_ADD | TINY3D_BLEND_ALPHA_FUNC_ADD);
        
        if (ui_state == UI_STATE_STREAMING) {
            ps3video_draw();
            
            // Draw Video Performance Stats HUD (Top Left) if enabled
            if (show_stats) {
                float sx = SX(30);
                float sy = SY(30);
                float line_h = SY(20);
                float hud_w = SX(430);
                float hud_h = 13.5f * line_h;

                // Semi-transparent dark HUD container background (#121212 with 85% alpha)
                tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                tiny3d_VertexPos(sx - SX(10), sy - SY(10), 65535);
                tiny3d_VertexFcolor(0.07f, 0.07f, 0.07f, 0.85f);
                tiny3d_VertexPos(sx + hud_w, sy - SY(10), 65535);
                tiny3d_VertexFcolor(0.07f, 0.07f, 0.07f, 0.85f);
                tiny3d_VertexPos(sx - SX(10), sy + hud_h, 65535);
                tiny3d_VertexFcolor(0.07f, 0.07f, 0.07f, 0.85f);
                tiny3d_VertexPos(sx + hud_w, sy + hud_h, 65535);
                tiny3d_VertexFcolor(0.07f, 0.07f, 0.07f, 0.85f);
                tiny3d_End();

                // Top Accent Line (#3F51B5)
                tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                tiny3d_VertexPos(sx - SX(10), sy - SY(10), 65535);
                tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                tiny3d_VertexPos(sx + hud_w, sy - SY(10), 65535);
                tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                tiny3d_VertexPos(sx - SX(10), sy - SY(8), 65535);
                tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                tiny3d_VertexPos(sx + hud_w, sy - SY(8), 65535);
                tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                tiny3d_End();

                SetCurrentFont(0);
                SetFontSize(SF(16), SF(16));
                SetFontColor(0x00ff00ff, 0); // Pure Matrix Green (RGBA: RR=0, GG=255, BB=0, AA=255)

                DrawFormatString(sx, sy + 0 * line_h, "Rendered FPS: %d", ps3video_get_current_fps());
                DrawFormatString(sx, sy + 1 * line_h, "Decoded FPS: %d", ps3video_get_decoded_fps());
                DrawFormatString(sx, sy + 2 * line_h, "UI Loop FPS: %d", ui_fps_actual);
                DrawFormatString(sx, sy + 3 * line_h, "Decode Latency: %d ms", ps3video_get_decode_latency());
                DrawFormatString(sx, sy + 4 * line_h, "Render Latency: %d ms", ps3video_get_render_latency());
                DrawFormatString(sx, sy + 5 * line_h, "Network Latency: %d ms", ps3video_get_net_latency() / 2);
                DrawFormatString(sx, sy + 6 * line_h, "Total Latency: %d ms", (ps3video_get_net_latency() / 2) + ps3video_get_decode_latency() + ps3video_get_render_latency());
                DrawFormatString(sx, sy + 7 * line_h, "Resolution: %dx%d stream -> %dx%d screen",
                                 ui_get_stream_width(), ui_get_stream_height(), ui_width, ui_height);
                DrawFormatString(sx, sy + 8 * line_h, "Target FPS: %d FPS", ui_get_fps());
                // Requested vs measured.  These two disagreeing is the whole
                // signal: if "Received" sits below "Bitrate" while frames are
                // being dropped, the selected step is above what this console
                // can actually pull.
                {
                    extern volatile int ps3_video_rx_kbps;   // VideoStream.c
                    extern volatile int ps3_video_rxq_bytes; // VideoStream.c
                    int want = ui_get_bitrate();
                    int got = ps3_video_rx_kbps;

                    DrawFormatString(sx, sy + 9 * line_h, "Bitrate: %.1f Mbps  (rx %.1f, sock %d KB)",
                                     (float)want / 1000.0f, (float)got / 1000.0f,
                                     ps3_video_rxq_bytes / 1024);
                    DrawFormatString(sx, sy + 10 * line_h, "Dropped frames: %u",
                                     (unsigned)ps3video_get_dropped_frames());
                }
                
                /* Real-time Hardware Telemetry Stream Link Activity Monitor */
                u32 total_frames = ps3video_get_total_decoded_frames();
                int pulse_phase = (int)((total_frames / 4) % 4);
                const char* spinner = "";
                switch (pulse_phase) {
                    case 0: spinner = "[ - ]"; break;
                    case 1: spinner = "[ \\ ]"; break;
                    case 2: spinner = "[ | ]"; break;
                    case 3: spinner = "[ / ]"; break;
                }
                DrawFormatString(sx, sy + 11 * line_h, "Stream Link: ACTIVE %s", spinner);
                {
                    int ach, ahq;
                    unsigned adec, amax, aund;
                    ps3audio_get_hud(&ach, &ahq, &adec, &amax, &aund);
                    if (ach > 0)
                        DrawFormatString(sx, sy + 12 * line_h,
                                         "Audio: %s%s  decode %.2f / %.2f ms  underruns %u",
                                         ach == 8 ? "7.1" : ach == 6 ? "5.1" : "Stereo",
                                         ahq ? " HQ" : "", adec / 1000.0f, amax / 1000.0f, aund);
                    else
                        DrawString(sx, sy + 12 * line_h, "Audio: not running");
                }
            }
        } else {
            draw_background_gradient();
            
            if (ui_state == UI_STATE_IP_ENTRY) {
                // Title inside #3F51B5 header bar
                SetFontSize(SF(26), SF(26));
                SetFontColor(0xffffffff, 0);
                DrawString(SX(40), SY(18), "Moonlight PS3");
                
                // Row 0: Sunshine Host
                SetFontSize(SF(24), SF(24));
                SetFontColor((active_main_item == 0) ? 0xff82b1ff : 0xffb0bec5, 0);
                float next_x = DrawString(SX(60), SY(125), "Sunshine Host:");
                
                SetFontColor((active_main_item == 0) ? 0xff82b1ff : 0xffffffff, 0);
                {
                    const ui_saved_host_t *sh = ui_get_saved_host(selected_host_idx);
                    if (sh && sh->name[0] && strcmp(sh->name, "Manual Entry") != 0 &&
                        strcmp(sh->name, sh->address) != 0)
                        DrawFormatString(next_x + SX(20), SY(125), "[ %s (%s) ]", sh->name, target_ip_str);
                    else
                        DrawFormatString(next_x + SX(20), SY(125), "[ %s ]", target_ip_str);
                    if (host_is_apollo) {
                        SetFontSize(SF(16), SF(16));
                        SetFontColor(0xff9e9e9e, 0);
                        DrawString(SX(60), SY(152), "Vibepollo / Apollo host (virtual display available)");
                    }
                }

                // Row 1: Settings Sub-menu Link
                SetFontSize(SF(24), SF(24));
                SetFontColor((active_main_item == 1) ? 0xff82b1ff : 0xffffffff, 0);
                DrawString(SX(60), SY(185), "[ CONFIGURE STREAM SETTINGS ]");
                
                // Active settings summary preview
                SetFontSize(SF(18), SF(18));
                SetFontColor(0xff9e9e9e, 0);
                int kbps = ui_bitrate_options[ui_bitrate_idx];
                if (kbps % 1000 == 0) {
                    DrawFormatString(SX(60), SY(225), "Current: %dx%d  |  %d FPS  |  %d Mbps  |  Mouse: %s  |  VSync: %s", 
                                     ui_get_stream_width(), ui_get_stream_height(),
                                     ui_fps, kbps / 1000, (ui_mouse_mode == 0) ? "GAME" : "DESKTOP", ui_vsync ? "ON" : "OFF");
                } else {
                    DrawFormatString(SX(60), SY(225), "Current: %dx%d  |  %d FPS  |  %.1f Mbps  |  Mouse: %s  |  VSync: %s", 
                                     ui_get_stream_width(), ui_get_stream_height(),
                                     ui_fps, (float)kbps / 1000.0f, (ui_mouse_mode == 0) ? "GAME" : "DESKTOP", ui_vsync ? "ON" : "OFF");
                }

                // Row 2: Connect / Pair Action Button
                SetFontSize(SF(24), SF(24));
                SetFontColor((active_main_item == 2) ? 0xff82b1ff : 0xffffffff, 0);
                DrawString(SX(60), SY(280), "[ CONNECT / PAIR TO HOST ]");

                // Row 3: only while the host reports an app running
                if (host_running_app) {
                    SetFontColor((active_main_item == 3) ? 0xff82b1ff : 0xffffffff, 0);
                    if (host_running_name[0])
                        DrawFormatString(SX(60), SY(335), "[ QUIT %s ON HOST ]", host_running_name);
                    else
                        DrawString(SX(60), SY(335), "[ QUIT RUNNING APP ON HOST ]");
                }

                // Clean controls legend
                SetFontSize(SF(18), SF(18));
                SetFontColor(0xff9e9e9e, 0);
                DrawString(SX(60), SY(445), "\x05 Navigate   |   \x01 Select   |   \x02 Exit to XMB");
            } else if (ui_state == UI_STATE_SETTINGS) {
                // Title inside #3F51B5 header bar
                SetFontSize(SF(26), SF(26));
                SetFontColor(0xffffffff, 0);
                DrawFormatString(SX(40), SY(18), "Moonlight PS3  -  %s",
                                 settings_group_names[settings_group[active_settings_item]]);

                // Scrolling list: SETTINGS_VISIBLE rows starting at settings_scroll.
                SetFontSize(SF(20), SF(20));
                for (int v = 0; v < SETTINGS_VISIBLE; v++) {
                    int r = settings_scroll + v;
                    if (r >= SETTINGS_ITEM_COUNT) break;
                    float y = SY(100 + 35 * v);
                    int active = (r == active_settings_item);
                    if (r == SR_BACK) {
                        SetFontColor(active ? 0xff82b1ff : 0xffffffff, 0);
                        DrawString(SX(60), y, "[ BACK TO MAIN MENU ]");
                        continue;
                    }
                    char val[80];
                    settings_value_text(r, val, sizeof(val));
                    SetFontColor(active ? 0xff82b1ff : 0xffb0bec5, 0);
                    DrawString(SX(60), y, (char *)settings_labels[r]);
                    SetFontColor(active ? 0xff82b1ff : 0xffffffff, 0);
                    DrawFormatString(SX(430), y, "[ %s ]", val);
                }

                // Position and scroll hints
                SetFontSize(SF(16), SF(16));
                SetFontColor(0xff9e9e9e, 0);
                DrawFormatString(SX(1090), SY(70), "%d / %d", active_settings_item + 1, SETTINGS_ITEM_COUNT);
                if (settings_scroll > 0)
                    DrawString(SX(1090), SY(88), "more above");
                if (settings_scroll + SETTINGS_VISIBLE < SETTINGS_ITEM_COUNT)
                    DrawString(SX(1090), SY(100 + 35 * SETTINGS_VISIBLE - 18), "more below");

                // Clean controls legend
                SetFontSize(SF(18), SF(18));
                SetFontColor(0xff9e9e9e, 0);
                DrawString(SX(60), SY(490), "\x05 Navigate   |   \x01 Select / Change   |   \x02 Back");
            } else if (ui_state == UI_STATE_PAIRING) {
                // Title inside #3F51B5 header bar
                SetFontSize(SF(26), SF(26));
                SetFontColor(0xffffffff, 0);
                DrawString(SX(40), SY(18), "Moonlight PS3  -  Device Pairing");

                if (pairing_pin_str[0] != '\0') {
                    // Heading
                    SetFontSize(SF(24), SF(24));
                    SetFontColor(0xffffffff, 0);
                    DrawString(SX(60), SY(110), "Sunshine Pairing Required");

                    // Subheading instructions
                    SetFontSize(SF(18), SF(18));
                    SetFontColor(0xffb0bec5, 0);
                    DrawString(SX(60), SY(150), "Open Sunshine Web UI (PIN Tab) and enter this PIN:");

                    // PIN Badge Card Container (#1E1E1E background with #3F51B5 top accent line)
                    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                    tiny3d_VertexPos(SX(60), SY(190), 65535);
                    tiny3d_VertexFcolor(0.12f, 0.12f, 0.12f, 0.95f);
                    tiny3d_VertexPos(SX(400), SY(190), 65535);
                    tiny3d_VertexFcolor(0.12f, 0.12f, 0.12f, 0.95f);
                    tiny3d_VertexPos(SX(60), SY(275), 65535);
                    tiny3d_VertexFcolor(0.12f, 0.12f, 0.12f, 0.95f);
                    tiny3d_VertexPos(SX(400), SY(275), 65535);
                    tiny3d_VertexFcolor(0.12f, 0.12f, 0.12f, 0.95f);
                    tiny3d_End();

                    // Top Accent Line (#3F51B5)
                    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                    tiny3d_VertexPos(SX(60), SY(190), 65535);
                    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                    tiny3d_VertexPos(SX(400), SY(190), 65535);
                    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                    tiny3d_VertexPos(SX(60), SY(193), 65535);
                    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                    tiny3d_VertexPos(SX(400), SY(193), 65535);
                    tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 1.0f);
                    tiny3d_End();

                    // PIN Text in large bold highlight
                    SetFontSize(SF(34), SF(34));
                    SetFontColor(0xff82b1ff, 0); // Rose / Pink highlight
                    DrawFormatString(SX(85), SY(218), "PIN:  %s", pairing_pin_str);

                    // Waiting status
                    SetFontSize(SF(18), SF(18));
                    SetFontColor(0xff9e9e9e, 0);
                    DrawString(SX(60), SY(305), "Waiting for confirmation from host...");
                } else {
                    SetFontSize(SF(24), SF(24));
                    SetFontColor(0xff82b1ff, 0);
                    DrawString(SX(60), SY(180), "Connecting to Sunshine Host...");

                    SetFontSize(SF(18), SF(18));
                    SetFontColor(0xffb0bec5, 0);
                    DrawString(SX(60), SY(220), "Initializing handshake session...");
                }

                // Bottom cancel button legend
                SetFontSize(SF(18), SF(18));
                SetFontColor(0xff9e9e9e, 0);
                DrawString(SX(60), SY(445), "\x02 Cancel Pairing");
            } else if (ui_state == UI_STATE_APPLIST) {
                // Title inside #3F51B5 header bar
                SetFontSize(SF(26), SF(26));
                SetFontColor(0xffffffff, 0);
                DrawString(SX(40), SY(18), "Moonlight PS3  -  Host Applications");

                // Subtitle
                SetFontSize(SF(22), SF(22));
                SetFontColor(0xffffffff, 0);
                DrawString(SX(60), SY(95), "Select Game or Application to Stream:");

                if (current_app_list.count > 0) {
                    SetFontSize(SF(18), SF(18));
                    SetFontColor(0xffb0bec5, 0);
                    DrawFormatString(SX(480), SY(95), "[ %d / %d ]", active_app_idx + 1, current_app_list.count);

                    // Render visible applications list
                    #define MAX_VISIBLE_APPS 6
                    int start_idx = 0;
                    if (active_app_idx >= MAX_VISIBLE_APPS) {
                        start_idx = active_app_idx - MAX_VISIBLE_APPS + 1;
                    }
                    int visible_count = current_app_list.count - start_idx;
                    if (visible_count > MAX_VISIBLE_APPS) visible_count = MAX_VISIBLE_APPS;

                    for (int i = 0; i < visible_count; i++) {
                        int idx = start_idx + i;
                        float row_y = SY(135) + (i * SY(48));

                        if (idx == active_app_idx) {
                            // Active selection card background (#242424 with pink accent border)
                            tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                            tiny3d_VertexPos(SX(55), row_y, 65535);
                            tiny3d_VertexFcolor(0.16f, 0.16f, 0.16f, 0.95f);
                            tiny3d_VertexPos(SX(850), row_y, 65535);
                            tiny3d_VertexFcolor(0.16f, 0.16f, 0.16f, 0.95f);
                            tiny3d_VertexPos(SX(55), row_y + SY(40), 65535);
                            tiny3d_VertexFcolor(0.16f, 0.16f, 0.16f, 0.95f);
                            tiny3d_VertexPos(SX(850), row_y + SY(40), 65535);
                            tiny3d_VertexFcolor(0.16f, 0.16f, 0.16f, 0.95f);
                            tiny3d_End();

                            // Left Accent Line (#FF82B1)
                            tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
                            tiny3d_VertexPos(SX(55), row_y, 65535);
                            tiny3d_VertexFcolor(1.0f, 0.51f, 0.69f, 1.0f);
                            tiny3d_VertexPos(SX(59), row_y, 65535);
                            tiny3d_VertexFcolor(1.0f, 0.51f, 0.69f, 1.0f);
                            tiny3d_VertexPos(SX(55), row_y + SY(40), 65535);
                            tiny3d_VertexFcolor(1.0f, 0.51f, 0.69f, 1.0f);
                            tiny3d_VertexPos(SX(59), row_y + SY(40), 65535);
                            tiny3d_VertexFcolor(1.0f, 0.51f, 0.69f, 1.0f);
                            tiny3d_End();

                            SetFontSize(SF(22), SF(22));
                            SetFontColor(0xff82b1ff, 0); // Pink highlight
                            DrawFormatString(SX(70), row_y + SY(8), "[ > ]  %s", current_app_list.apps[idx].name);
                        } else {
                            SetFontSize(SF(20), SF(20));
                            SetFontColor(0xffb0bec5, 0);
                            DrawFormatString(SX(70), row_y + SY(8), "       %s", current_app_list.apps[idx].name);
                        }
                    }
                } else {
                    SetFontSize(SF(22), SF(22));
                    SetFontColor(0xffff82b1, 0);
                    DrawString(SX(60), SY(180), "No applications found on host.");
                }

                // Clean controls legend
                SetFontSize(SF(18), SF(18));
                SetFontColor(0xff9e9e9e, 0);
                DrawString(SX(60), SY(445), "\x05 Navigate   |   \x01 Launch Game   |   \x02 Cancel");
                } else if (ui_state == UI_STATE_DISCOVERY) {
                SetFontSize(SF(26), SF(26));
                SetFontColor(0xffffffff, 0);
                DrawString(SX(40), SY(18), "Moonlight PS3  -  Find Host");

                if (!discovery_scanned) {
                    SetFontSize(SF(24), SF(24));
                    SetFontColor(0xff82b1ff, 0);
                    DrawString(SX(60), SY(180), "Scanning for Sunshine hosts on the LAN...");
                } else {
                    SetFontSize(SF(20), SF(20));
                    SetFontColor(0xffb0bec5, 0);
                    if (discovered_host_count == 0)
                        DrawString(SX(60), SY(110), "No hosts found automatically.");
                    else
                        DrawFormatString(SX(60), SY(95), "Found %d host(s):", discovered_host_count);

                    int total_rows = discovered_host_count + 1;
                    for (int i = 0; i < total_rows; i++) {
                        float row_y = SY(140) + (i * SY(40));
                        char label_buf[96];
                        if (i == discovered_host_count)
                            snprintf(label_buf, sizeof(label_buf), "[ Enter IP manually... ]");
                        else
                            snprintf(label_buf, sizeof(label_buf), "%s  (%s)",
                                     discovered_hosts[i].name, discovered_hosts[i].address);
                        SetFontSize(SF(22), SF(22));
                        SetFontColor((i == active_host_idx) ? 0xff82b1ff : 0xffffffff, 0);
                        DrawFormatString(SX(70), row_y, "%s %s",
                                        (i == active_host_idx) ? "[ > ]" : "     ", label_buf);
                    }
                }
                SetFontSize(SF(18), SF(18));
                SetFontColor(0xff9e9e9e, 0);
                DrawString(SX(60), SY(445), "\x05 Navigate   |   \x01 Select   |   \x02 Back");
			} else if (ui_state == UI_STATE_ERROR) {
                SetFontSize(SF(26), SF(26));
                SetFontColor(0xffff5252, 0);
                DrawString(SX(60), SY(200), "ERROR: Target unreachable or Pairing failed.");
                if (ui_error_detail[0]) {
                    // The host's own reason (Vibepollo explains permission
                    // refusals and what to change).  Wrapped by hand: the font
                    // layer does not wrap.
                    SetFontSize(SF(18), SF(18));
                    SetFontColor(0xffffffff, 0);
                    const int wrap = 70;
                    const char *p = ui_error_detail;
                    int line = 0;
                    while (*p && line < 4) {
                        char buf[80];
                        int len = (int)strlen(p);
                        int take = len > wrap ? wrap : len;
                        if (take < len) {
                            int sp = take;
                            while (sp > 0 && p[sp] != ' ') sp--;
                            if (sp > 0) take = sp;
                        }
                        memcpy(buf, p, (size_t)take);
                        buf[take] = '\0';
                        DrawString(SX(60), SY(235 + line * 24), buf);
                        p += take;
                        while (*p == ' ') p++;
                        line++;
                    }
                    SetFontSize(SF(26), SF(26));
                    SetFontColor(0xffff5252, 0);
                }
                DrawString(SX(60), SY(340), "Press \x01 to return.");
                if (pad.buttons_pressed & A_FLAG) {
                    ui_state = UI_STATE_IP_ENTRY;
                    ui_error_detail[0] = '\0';
                }
            }
        }

        // 2. Draw TTY Logs (Bottom 28%) - Only in Menus
        if (ui_state != UI_STATE_STREAMING) {
            char visible_logs[8][MAX_LOG_WIDTH];
            int visible_log_count = 0;

            if (log_mutex_initialized) sysMutexLock(log_mutex, 0);
            int start_line = (log_count > 8) ? (log_count - 8) : 0;
            for (int i = start_line; i < log_count; i++) {
                memcpy(visible_logs[visible_log_count++], log_buffer[i], MAX_LOG_WIDTH);
            }
            if (log_mutex_initialized) sysMutexUnlock(log_mutex);

            // Log container background (#1E1E1E)
            tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
            tiny3d_VertexPos(0, ui_height * 0.72f, 65535);
            tiny3d_VertexFcolor(0.118f, 0.118f, 0.118f, 0.95f);
            tiny3d_VertexPos(ui_width, ui_height * 0.72f, 65535);
            tiny3d_VertexFcolor(0.118f, 0.118f, 0.118f, 0.95f);
            tiny3d_VertexPos(0, ui_height, 65535);
            tiny3d_VertexFcolor(0.090f, 0.090f, 0.090f, 0.95f);
            tiny3d_VertexPos(ui_width, ui_height, 65535);
            tiny3d_VertexFcolor(0.090f, 0.090f, 0.090f, 0.95f);
            tiny3d_End();

            // Log header accent line (#3F51B5)
            tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
            tiny3d_VertexPos(0, ui_height * 0.72f, 65535);
            tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 0.8f);
            tiny3d_VertexPos(ui_width, ui_height * 0.72f, 65535);
            tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 0.8f);
            tiny3d_VertexPos(0, (ui_height * 0.72f) + SY(2), 65535);
            tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 0.8f);
            tiny3d_VertexPos(ui_width, (ui_height * 0.72f) + SY(2), 65535);
            tiny3d_VertexFcolor(0.247f, 0.318f, 0.710f, 0.8f);
            tiny3d_End();

            SetFontSize(SF(16), SF(16));
            SetFontColor(0xff80d8ff, 0); // Light Material Cyan/Blue for log readability
            for (int i = 0; i < visible_log_count; i++) {
                DrawString(SX(20), (ui_height * 0.73f) + (i * SY(19)), visible_logs[i]);
            }
        }

        tiny3d_Flip();
        ps3video_after_flip(); // YUV self-test screen capture (no-op unless armed)
        // tiny3d_Flip() waits for VBlank, so usleep is unnecessary and causes frame drops.
    }
    sysThreadExit(0);
}

void ui_shutdown() {
    ui_save_settings();
    ui_stop();
    if (msg_dialog_active) {
        msgDialogAbort();
        msg_dialog_active = 0;
    }
    if (osk_active) {
        oskAbort();
    }
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT1);
    if (osk_container_created) {
        sysMemContainerDestroy(osk_container);
        osk_container_created = 0;
    }
    if (ui_thread_started) {
        u64 retval;
        sysThreadJoin(ui_thread, &retval);
        ui_thread_started = 0;
    }
    if (ft_face) {
        FT_Done_Face(ft_face);
        ft_face = NULL;
    }
    if (ft_library) {
        FT_Done_FreeType(ft_library);
        ft_library = NULL;
    }
    if (log_mutex_initialized) {
        sysMutexDestroy(log_mutex);
        log_mutex_initialized = 0;
    }
}

