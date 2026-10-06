#include "ui.h"
#include <tiny3d.h>
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
#include <sysutil/video.h>
#include <unistd.h>
#include <lv2/systime.h>
#include "input.h"
#include "moonlight_discovery.h"
#include <Limelight.h>
#include "video.h"
#include "audio.h"
#include "ui_internal.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "ui_draw.h"
#include "ui_bg.h"
#include "ui_screens.h"

#define CONFIG_DIR  "/dev_hdd0/game/MNLT00001/USRDIR"
#define CONFIG_PATH "/dev_hdd0/game/MNLT00001/USRDIR/config.ini"

static sys_ppu_thread_t ui_thread;
static int ui_thread_started = 0;
static volatile int ui_running = 1;
static int ui_width = 1280;
static int ui_height = 720;
static volatile int ui_state = UI_STATE_IP_ENTRY;

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
//
// 0 = AUTO: 50 on a 50 Hz output (576i/576p and other PAL modes), otherwise 60,
// so the stream always paces 1:1 against whatever the console is outputting.
static int ui_fps_options[] = {0, 30, 50, 60, 120};
#define NUM_FPS_OPTIONS (int)(sizeof(ui_fps_options) / sizeof(ui_fps_options[0]))
static int ui_fps = 0;
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
// 40, 50 and 60 exist to FIND the PS3's UDP receive ceiling, not because they
// are expected to hold: watch rx= against the step and fecfail= in [PS3-NET].
// The request is the step minus 20% (FEC headroom), and Vibepollo's x264
// with its one-frame VBV has measured at about half of THAT on Cyberpunk.
static int ui_bitrate_options[] = {2500, 5000, 10000, 12500, 15000, 20000, 25000, 30000, 40000, 50000, 60000};
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
//
// The 4:3 modes are square-pixel, so the PC renders a true 4:3 picture for a
// 4:3 set: 640x480 for 480-line outputs, 768x576 for 576-line ones, 1024x768
// as the sharper option.  All three keep width/2 a multiple of 64 for YUV420.
//
// {0, 0} is AUTO, resolved from the console's output by ui_resolve_res():
//   SD 4:3 -> 640x480 or 768x576, SD 16:9 -> 960x544, HD -> 1280x720
//   (1024x768 on an HD output set to 4:3).
static int ui_res_options[][2] = {{0, 0}, {640, 480}, {768, 576}, {960, 544}, {1024, 768},
                                  {1280, 720}, {1792, 1008}, {1920, 1080}};
static const int ui_res_legacy[][2] = {{1280, 720}, {1920, 1080}, {1792, 1008}};
#define NUM_RES_OPTIONS (int)(sizeof(ui_res_options) / sizeof(ui_res_options[0]))
#define RES_IDX_AUTO 0
static int ui_res_idx = RES_IDX_AUTO;
static int ui_res_configured = 0;    // config.ini named a resolution
static int ui_output_is_sd = 0;      // console video output is below 1280 wide

// The console's output mode, read once at startup (main.c).  Nothing here
// changes it: a bad videoConfigure() blanks the panel on this hardware.
static int ui_out_refresh_bits = 0;  // VIDEO_REFRESH_* bits of the active mode
static int ui_out_aspect = 0;        // VIDEO_ASPECT_*: 0 auto, 1 = 4:3, 2 = 16:9

// Picture shape: 0 = FIT (keep the stream's shape, black bars where it does
// not match the screen), 1 = STRETCH (fill the screen).  config.ini: aspect_mode
static int ui_aspect_mode = 0;

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
// Settings > Advanced > Intra refresh, or "intra_refresh=0" in
// /dev_hdd0/game/MNLT00001/USRDIR/config.ini.
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

static int frames_drawn_this_sec = 0;
int ui_fps_actual = 0;
static u64 last_ui_time = 0;
int show_stats = 0; // Default: Stats OFF (0)
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
void ui_set_output_mode(int refresh_bits, int aspect) {
    ui_out_refresh_bits = refresh_bits;
    ui_out_aspect = aspect;
}
int ui_output_is_50hz(void) {
    return (ui_out_refresh_bits & VIDEO_REFRESH_50HZ) &&
           !(ui_out_refresh_bits & (VIDEO_REFRESH_59_94HZ | VIDEO_REFRESH_60HZ));
}
// Shape of the screen itself.  An unreported aspect is guessed from the mode:
// SD sets are overwhelmingly 4:3, HD modes are 16:9.
int ui_output_is_4x3(void) {
    if (ui_out_aspect == VIDEO_ASPECT_4_3) return 1;
    if (ui_out_aspect == VIDEO_ASPECT_16_9) return 0;
    return ui_output_is_sd;
}
int ui_get_aspect_mode(void) { return ui_aspect_mode; }

int ui_get_fps() {
    if (ui_fps) return ui_fps;
    return ui_output_is_50hz() ? 50 : 60;
}
int ui_get_bitrate() { return ui_bitrate_options[ui_bitrate_idx]; }
int ui_get_refresh_x100(void) {
    int fps = ui_get_fps();
    if (ui_ntsc_rate && fps != 50) return (fps * 100000 + 500) / 1001;
    return fps * 100;
}

static void ui_resolve_res(int idx, int *w, int *h) {
    if (idx != RES_IDX_AUTO) { *w = ui_res_options[idx][0]; *h = ui_res_options[idx][1]; return; }
    if (ui_output_is_sd && ui_output_is_4x3()) {
        if (ui_height >= 576) { *w = 768; *h = 576; }
        else { *w = 640; *h = 480; }
    } else if (ui_output_is_sd) { *w = 960; *h = 544; }
    else if (ui_output_is_4x3()) { *w = 1024; *h = 768; }
    else { *w = 1280; *h = 720; }
}

// Macroblock rate of the selected mode, for the level hints in the menu.
static unsigned int ui_mode_mb_rate(void) {
    int sw, sh;
    ui_resolve_res(ui_res_idx, &sw, &sh);
    unsigned int w = (unsigned int)(sw + 15) / 16;
    unsigned int h = (unsigned int)(sh + 15) / 16;
    return w * h * (unsigned int)ui_get_fps();
}
int ui_get_packet_size() { return ui_packet_size_options[ui_packet_size_idx]; }
int ui_get_stream_width(void)  { int w, h; ui_resolve_res(ui_res_idx, &w, &h); return w; }
int ui_get_stream_height(void) { int w, h; ui_resolve_res(ui_res_idx, &w, &h); return h; }
// YUV420P needs 64-byte-aligned plane pitches for the RSX texture: the chroma
// pitch is width/2, so 960 wide (480) cannot use it and falls back to ARGB32.
int ui_get_pixel_format(void)  { return (ui_pixfmt && ((ui_get_stream_width() / 2) % 64) == 0) ? 1 : 0; }

// Where the stream goes on screen, in output pixels.  FIT keeps the stream's
// own shape (square pixels from the PC) against the screen's real shape, which
// for SD is 4:3 or 16:9 whatever the pixel count says (720x480 is either).
void ui_stream_rect(int screen_w, int screen_h, float *x, float *y, float *w, float *h) {
    *x = 0.0f; *y = 0.0f; *w = (float)screen_w; *h = (float)screen_h;
    if (ui_aspect_mode == 1) return;
    float screen_ar = ui_output_is_4x3() ? (4.0f / 3.0f) : (16.0f / 9.0f);
    float stream_ar = (float)ui_get_stream_width() / (float)ui_get_stream_height();
    if (stream_ar > screen_ar * 1.01f) {        // wider: bars top and bottom
        *h = (float)screen_h * screen_ar / stream_ar;
        *y = ((float)screen_h - *h) * 0.5f;
    } else if (stream_ar < screen_ar * 0.99f) { // narrower: bars left and right
        *w = (float)screen_w * stream_ar / screen_ar;
        *x = ((float)screen_w - *w) * 0.5f;
    }
}
int ui_get_rumble(void)        { return ui_rumble; }
int ui_get_trigger_mode(void)  { return ui_trigger_mode; }
int ui_get_intra_refresh(void) { return ui_intra_refresh; }
int ui_get_virtual_display(void) { return ui_virtual_display; }
int ui_get_quit_on_exit(void) { return ui_quit_on_exit; }
int ui_get_audio_channels(void) { return ui_audio_channels; }
// The AUDIO_CONFIGURATION_* the stream asks for.  The RTSP setup and the
// /launch surroundAudioInfo must both come from this, or the host can set up
// one layout and be told about another.
int ui_get_audio_configuration(void) {
    switch (ui_audio_channels) {
    case 8:  return AUDIO_CONFIGURATION_71_SURROUND;
    case 6:  return AUDIO_CONFIGURATION_51_SURROUND;
    default: return AUDIO_CONFIGURATION_STEREO;
    }
}
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

char pairing_pin_str[16] = "";

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
ps3_app_list_t current_app_list;
int active_app_idx = 0;
volatile int app_selection_confirmed = 0;

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
char ui_error_detail[192] = "";
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

void ui_remove_saved_host(int idx) {
    if (idx < 0 || idx >= saved_host_count) return;
    for (int i = idx; i < saved_host_count - 1; i++) saved_hosts[i] = saved_hosts[i + 1];
    saved_host_count--;
    memset(&saved_hosts[saved_host_count], 0, sizeof(saved_hosts[0]));
    if (selected_host_idx == idx)
        selected_host_idx = (saved_host_count > 0) ? (idx < saved_host_count ? idx : saved_host_count - 1) : -1;
    else if (selected_host_idx > idx)
        selected_host_idx--;
    if (selected_host_idx >= 0) ui_set_target_ip(saved_hosts[selected_host_idx].address);
    host_running_app = 0;
    host_running_name[0] = 0;
    ui_save_settings();
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
volatile int host_running_app = 0;      // running app id, 0 = none
char host_running_name[64] = "";
volatile int host_is_apollo = 0;
volatile int quit_request = 0;

void ui_set_host_status(int running_app, const char *app_name, int apollo_family) {
    snprintf(host_running_name, sizeof(host_running_name), "%s", app_name ? app_name : "");
    host_is_apollo = apollo_family ? 1 : 0;
    host_running_app = running_app;
}

int ui_take_quit_request(void) {
    if (!quit_request) return 0;
    quit_request = 0;
    return 1;
}

// Host Discovery State
mld_host_t discovered_hosts[MLD_MAX_HOSTS];
int discovered_host_count = 0;
int discovery_scanned = 0;
int active_host_idx = 0;
volatile int host_selection_confirmed = 0;
volatile int manual_entry_requested = 0;

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
    fprintf(f, "aspect_mode=%d\n", ui_aspect_mode);
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
    fprintf(f, "ui_theme=%d\n", ui_theme_get_mode());

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
            else if (strcmp(key, "fps") == 0) { int v = atoi(val); if (v==0||v==30||v==50||v==60||v==120) ui_fps=v; }
            else if (strcmp(key, "aspect_mode") == 0) { int v = atoi(val); if (v==0||v==1) ui_aspect_mode=v; }
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
            else if (strcmp(key, "ui_theme") == 0) { int v = atoi(val); if (v >= 0 && v <= 2) ui_theme_set_mode(v); }
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

// Native PS3 Message Dialog (Yes/No confirmation)
static volatile int msg_dialog_active = 0;
static void (*confirm_done)(int yes, void *user);
static void *confirm_user;

static void ui_msg_dialog_callback(msgButton button, void *usrData) {
    (void)usrData;
    msgDialogClose(0.0f);
    msg_dialog_active = 0;
    void (*done)(int, void *) = confirm_done;
    confirm_done = NULL;
    if (done) done(button == MSG_DIALOG_BTN_YES, confirm_user);
}

int ui_confirm(const char *text, void (*done)(int yes, void *user), void *user) {
    if (msg_dialog_active || osk_active) return 0;
    msg_dialog_active = 1;
    confirm_done = done;
    confirm_user = user;
    msgDialogOpen2(MSG_DIALOG_NORMAL | MSG_DIALOG_BTN_TYPE_YESNO | MSG_DIALOG_DEFAULT_CURSOR_NO,
                   text, ui_msg_dialog_callback, NULL, NULL);
    return 1;
}

int ui_modal_active(void) { return msg_dialog_active || osk_active; }

static void exit_confirmed(int yes, void *user) {
    (void)user;
    if (yes) {
        ui_push_log("Exit confirmed by user. Quitting to PS3 XMB...");
        ui_stop();
    } else {
        ui_push_log("Exit canceled.");
    }
}

void ui_open_exit_dialog(void) {
    ui_confirm("Do you want to quit Moonlight and return to the PS3 XMB?", exit_confirmed, NULL);
}

static void ui_loop(void *arg);

#define MAX_LOG_LINES 25
#define MAX_LOG_WIDTH UI_LOG_WIDTH

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


const unsigned char *ui_get_fallback_bitmap(void) { return &font_8x8_basic[0][0]; }

// Layout preview: USRDIR/preview.txt forces a screen with fake data so every
// screen can be captured without a host.  Absent in normal use.
//   state=home|discovery|settings|pairing|applist|error|streaming
//   running=<app id>   pin=<4 digits>   error=<text>   apps=<name>|<name>|...
static int ui_preview = 0;
int ui_preview_active(void) { return ui_preview; }

static void ui_preview_load(void) {
    FILE *f = fopen(CONFIG_DIR "/preview.txt", "r");
    if (!f) return;
    char line[256];
    int running = 0;
    char state[24] = "home";
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0';
        if (!strcmp(line, "state")) snprintf(state, sizeof(state), "%s", eq);
        else if (!strcmp(line, "running")) running = atoi(eq);
        else if (!strcmp(line, "pin")) ui_set_pairing_pin(eq);
        else if (!strcmp(line, "error")) ui_set_error_detail(eq);
        else if (!strcmp(line, "apps")) {
            memset(&current_app_list, 0, sizeof(current_app_list));
            char *p = eq;
            while (p && *p && current_app_list.count < MAX_APP_ENTRIES) {
                char *bar = strchr(p, '|');
                if (bar) *bar = '\0';
                ps3_app_entry_t *a = &current_app_list.apps[current_app_list.count];
                a->id = current_app_list.count + 1;
                snprintf(a->name, sizeof(a->name), "%s", p);
                current_app_list.count++;
                p = bar ? bar + 1 : NULL;
            }
        }
    }
    fclose(f);
    ui_preview = 1;
    if (running) {
        const char *nm = "";
        for (int i = 0; i < current_app_list.count; i++)
            if (current_app_list.apps[i].id == running) nm = current_app_list.apps[i].name;
        ui_set_host_status(running, nm, 1);
    }
    if (!strcmp(state, "discovery")) {
        snprintf(discovered_hosts[0].name, sizeof(discovered_hosts[0].name), "GAMING-PC");
        snprintf(discovered_hosts[0].address, sizeof(discovered_hosts[0].address), "192.168.0.194");
        snprintf(discovered_hosts[1].name, sizeof(discovered_hosts[1].name), "STUDY-DESKTOP-WITH-A-VERY-LONG-NAME");
        snprintf(discovered_hosts[1].address, sizeof(discovered_hosts[1].address), "192.168.0.31");
        discovered_host_count = 2;
        discovery_scanned = 1;
        ui_state = UI_STATE_DISCOVERY;
    } else if (!strcmp(state, "settings")) ui_state = UI_STATE_SETTINGS;
    else if (!strcmp(state, "pairing"))   ui_state = UI_STATE_PAIRING;
    else if (!strcmp(state, "applist"))   ui_state = UI_STATE_APPLIST;
    else if (!strcmp(state, "error"))     ui_state = UI_STATE_ERROR;
    else if (!strcmp(state, "streaming")) { ui_state = UI_STATE_STREAMING; show_stats = 1; }
}

void ui_init(int width, int height) {
    ui_width = (width > 0) ? width : 1280;
    ui_height = (height > 0) ? height : 720;

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
    ui_preview_load();
    ui_theme_update();
    ui_layout_init(ui_width, ui_height, ui_output_is_4x3());

    {
        char out_log[128];
        snprintf(out_log, sizeof(out_log), "Output: %dx%d %s %s -> AUTO stream %dx%d @ %d fps",
                 ui_width, ui_height, ui_output_is_4x3() ? "4:3" : "16:9",
                 ui_output_is_50hz() ? "50Hz" : "60Hz",
                 ui_get_stream_width(), ui_get_stream_height(), ui_get_fps());
        ui_push_log(out_log);
    }
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

int ui_log_snapshot(char (*out)[UI_LOG_WIDTH], int max_lines) {
    if (log_mutex_initialized) sysMutexLock(log_mutex, 0);
    int start = (log_count > max_lines) ? (log_count - max_lines) : 0;
    int n = 0;
    for (int i = start; i < log_count; i++) memcpy(out[n++], log_buffer[i], UI_LOG_WIDTH);
    if (log_mutex_initialized) sysMutexUnlock(log_mutex);
    return n;
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


// ---------------------------------------------------------------------------
// Settings model.  Rows are the SR_* ids from 1.4.0; categories, labels, value
// wording and help text are what the settings screen shows.
// ---------------------------------------------------------------------------
static const char *settings_labels[SETTINGS_ITEM_COUNT] = {
    [SR_FPS] = "Frame rate", [SR_RES] = "Resolution", [SR_ASPECT] = "Aspect",
    [SR_BITRATE] = "Bitrate", [SR_PIXFMT] = "Decoder output", [SR_DEBLOCK] = "Decode speed",
    [SR_PRESENT] = "Frame pacing", [SR_NTSC] = "Refresh timing", [SR_SPUS] = "Decoder SPUs",
    [SR_PACKET] = "Packet size", [SR_INTRA] = "Intra refresh", [SR_VDISPLAY] = "Virtual display",
    [SR_QUITEXIT] = "Quit game when I leave", [SR_AUDIO] = "Audio",
    [SR_MOUSE] = "Mouse", [SR_RUMBLE] = "Vibration", [SR_TRIGGERS] = "Triggers",
    [SR_VSYNC] = "VSync", [SR_OVS_X] = "Picture width", [SR_OVS_Y] = "Picture height",
    [SR_OVS_XOFF] = "Move picture left/right", [SR_OVS_YOFF] = "Move picture up/down",
    [SR_STATS] = "Performance overlay", [SR_VERBOSE] = "Verbose logging", [SR_THEME] = "Theme",
};

static const int cat_picture[]    = { SR_RES, SR_FPS, SR_BITRATE, SR_ASPECT };
static const int cat_sound[]      = { SR_AUDIO };
static const int cat_controller[] = { SR_RUMBLE, SR_TRIGGERS, SR_MOUSE };
static const int cat_screen[]     = { SR_THEME, SR_OVS_X, SR_OVS_Y, SR_OVS_XOFF, SR_OVS_YOFF, SR_STATS };
static const int cat_pc[]         = { SR_VDISPLAY, SR_QUITEXIT };
static const int cat_advanced[]   = { SR_PIXFMT, SR_DEBLOCK, SR_PRESENT, SR_NTSC, SR_SPUS,
                                      SR_PACKET, SR_INTRA, SR_VSYNC, SR_VERBOSE };

static const struct { const char *name, *note; const int *rows; int count; } settings_cats[] = {
    { "Picture",    "", cat_picture,    (int)(sizeof(cat_picture) / sizeof(int)) },
    { "Sound",      "", cat_sound,      (int)(sizeof(cat_sound) / sizeof(int)) },
    { "Controller", "", cat_controller, (int)(sizeof(cat_controller) / sizeof(int)) },
    { "Screen",     "", cat_screen,     (int)(sizeof(cat_screen) / sizeof(int)) },
    { "PC",         "", cat_pc,         (int)(sizeof(cat_pc) / sizeof(int)) },
    { "Advanced",   "For troubleshooting. The defaults are best for most people.",
                    cat_advanced, (int)(sizeof(cat_advanced) / sizeof(int)) },
};
#define SETTINGS_CATS ((int)(sizeof(settings_cats) / sizeof(settings_cats[0])))

int ui_settings_cat_count(void) { return SETTINGS_CATS; }
const char *ui_settings_cat_name(int c) { return (c >= 0 && c < SETTINGS_CATS) ? settings_cats[c].name : ""; }
const char *ui_settings_cat_note(int c) { return (c >= 0 && c < SETTINGS_CATS) ? settings_cats[c].note : ""; }
int ui_settings_cat_rows(int c, const int **rows) {
    if (c < 0 || c >= SETTINGS_CATS) { *rows = NULL; return 0; }
    *rows = settings_cats[c].rows;
    return settings_cats[c].count;
}
const char *ui_settings_label(int row) {
    return (row >= 0 && row < SETTINGS_ITEM_COUNT && settings_labels[row]) ? settings_labels[row] : "";
}

static const char *res_name(int w, int h, char *buf, size_t n) {
    if (w == 1280 && h == 720) return "720p";
    if (w == 1920 && h == 1080) return "1080p";
    snprintf(buf, n, "%dx%d", w, h);
    return buf;
}

void ui_settings_value(int row, char *out, size_t n) {
    switch (row) {
    case SR_FPS:
        if (ui_fps) snprintf(out, n, "%d", ui_fps);
        else snprintf(out, n, "Auto (%d)", ui_get_fps());
        break;
    case SR_RES: {
        char b[24];
        const char *nm = res_name(ui_get_stream_width(), ui_get_stream_height(), b, sizeof(b));
        if (ui_res_idx == RES_IDX_AUTO) snprintf(out, n, "Auto (%s)", nm);
        else snprintf(out, n, "%s", nm);
        break;
    }
    case SR_ASPECT:  snprintf(out, n, "%s", ui_aspect_mode ? "Stretch" : "Fit"); break;
    case SR_BITRATE: {
        int kbps = ui_bitrate_options[ui_bitrate_idx];
        if (kbps % 1000 == 0) snprintf(out, n, "%d Mbps", kbps / 1000);
        else snprintf(out, n, "%.1f Mbps", (float)kbps / 1000.0f);
        break;
    }
    case SR_PACKET:  snprintf(out, n, "%d", ui_packet_size_options[ui_packet_size_idx]); break;
    case SR_PIXFMT:  snprintf(out, n, "%s", ui_get_pixel_format() ? "YUV (faster)" : "ARGB (compatible)"); break;
    case SR_DEBLOCK: snprintf(out, n, "%s", (ui_no_deblock == 1) ? "Fast" : (ui_no_deblock == 0) ? "Quality" : "Auto"); break;
    case SR_PRESENT: snprintf(out, n, "%s", (ui_low_latency == 2) ? "Lowest lag" : (ui_low_latency == 0) ? "Smoothest" : "Balanced"); break;
    case SR_NTSC:    snprintf(out, n, "%s", ui_ntsc_rate ? "59.94 Hz" : "60 Hz"); break;
    case SR_SPUS:
        if (ui_vdec_spus == 0) snprintf(out, n, "Auto");
        else snprintf(out, n, "%d", ui_vdec_spus);
        break;
    case SR_INTRA:    snprintf(out, n, "%s", ui_intra_refresh ? "On" : "Off"); break;
    case SR_VDISPLAY: snprintf(out, n, "%s", ui_virtual_display ? "On" : "Off"); break;
    case SR_QUITEXIT: snprintf(out, n, "%s", ui_quit_on_exit ? "Yes" : "No"); break;
    case SR_AUDIO:
        // HQ: moonlight-common-c asks for high-quality surround at 15 Mbps and up.
        snprintf(out, n, "%s%s", (ui_audio_channels == 8) ? "7.1 Surround"
                                 : (ui_audio_channels == 6) ? "5.1 Surround" : "Stereo",
                 (ui_audio_channels > 2 && ui_get_bitrate() >= 15000) ? " - HQ" : "");
        break;
    case SR_MOUSE:    snprintf(out, n, "%s", (ui_mouse_mode == 0) ? "Game" : "Desktop"); break;
    case SR_RUMBLE:   snprintf(out, n, "%s", ui_rumble ? "On" : "Off"); break;
    case SR_TRIGGERS: snprintf(out, n, "%s", ui_trigger_mode ? "Pressure-sensitive" : "On/off"); break;
    case SR_VSYNC:    snprintf(out, n, "%s", ui_vsync ? "On" : "Off"); break;
    case SR_OVS_X:    snprintf(out, n, "%d%%", ui_ovs_x); break;
    case SR_OVS_Y:    snprintf(out, n, "%d%%", ui_ovs_y); break;
    case SR_OVS_XOFF: snprintf(out, n, "%+d", ui_ovs_xoff); break;
    case SR_OVS_YOFF: snprintf(out, n, "%+d", ui_ovs_yoff); break;
    case SR_STATS:    snprintf(out, n, "%s", show_stats ? "On" : "Off"); break;
    case SR_VERBOSE:  snprintf(out, n, "%s", ui_verbose ? "On" : "Off"); break;
    case SR_THEME: {
        int m = ui_theme_get_mode();
        snprintf(out, n, "%s", m == UI_THEME_DAY ? "Day" : m == UI_THEME_NIGHT ? "Night" : "Auto");
        break;
    }
    default:          out[0] = '\0'; break;
    }
}

void ui_settings_help(int row, char *out, size_t n) {
    const char *h = "";
    switch (row) {
    case SR_RES:
        h = "The size of the picture your PC sends. Auto picks one that suits your TV; bigger is sharper but harder work for the PS3.";
        break;
    case SR_FPS:
        h = "Pictures per second. Auto matches your TV (60, or 50 on a PAL set). 30 is gentler on the PS3 and your network.";
        break;
    case SR_BITRATE:
        h = "How much data the video uses. More looks cleaner but needs a faster network; this PS3 manages about 25 Mbps at most.";
        break;
    case SR_ASPECT:
        h = "Fit keeps the picture's shape and adds black bars if needed. Stretch fills the whole screen.";
        break;
    case SR_AUDIO:
        h = "Stereo works everywhere. 5.1 and 7.1 need a surround system, and high quality switches on at 15 Mbps and up.";
        break;
    case SR_RUMBLE:   h = "Passes the game's vibration on to your controller."; break;
    case SR_TRIGGERS: h = "Pressure-sensitive uses how far you squeeze L2 and R2. On/off treats them as plain buttons."; break;
    case SR_MOUSE:    h = "Game moves the cursor like a camera, for 3D games. Desktop points at the exact spot, for apps and menus."; break;
    case SR_THEME:    h = "Auto uses the day look from 7:00 to 19:00 by the console clock, and the night look after that."; break;
    case SR_OVS_X:    h = "Shrinks the picture sideways so your TV does not cut off the edges. Menus and games both use it."; break;
    case SR_OVS_Y:    h = "Shrinks the picture vertically so your TV does not cut off the edges. Menus and games both use it."; break;
    case SR_OVS_XOFF: h = "Moves the whole picture left or right."; break;
    case SR_OVS_YOFF: h = "Moves the whole picture up or down."; break;
    case SR_STATS:    h = "Shows frame rate, delay and network numbers over the game."; break;
    case SR_VDISPLAY: h = "Lets Apollo and Vibepollo create a screen that matches the PS3's picture exactly. Plain Sunshine ignores it."; break;
    case SR_QUITEXIT: h = "Closes the game on your PC when you stop streaming. If the connection drops, the game stays open so you can resume."; break;
    case SR_PIXFMT:   h = "YUV does the colour conversion on the graphics chip, which is faster. It needs a stream wider than 960."; break;
    case SR_DEBLOCK:  h = "Skipping the deblocking filter decodes faster at a small cost in sharpness. Auto does it only at 1080p 50 or 60."; break;
    case SR_PRESENT:  h = "Smoothest queues frames for even motion. Lowest lag shows the newest frame each refresh. Balanced sits between."; break;
    case SR_NTSC:     h = "59.94 Hz is what the PS3 really outputs as sixty, and it avoids a stutter every 17 seconds. Use 60 Hz only if your host misbehaves."; break;
    case SR_SPUS:     h = "How many of the PS3's SPU cores decode video. Auto is best unless you are testing."; break;
    case SR_PACKET:   h = "Bytes per network packet. 1392 is more efficient; 1024 is the original and safest."; break;
    case SR_INTRA:    h = "Spreads the full-picture refresh over many frames instead of one big burst, which avoids lost packets. Leave it on."; break;
    case SR_VSYNC:    h = "On waits for the TV's refresh for a smooth picture. Off has less lag but can tear."; break;
    case SR_VERBOSE:  h = "Writes extra detail to the log. Only for troubleshooting."; break;
    default: break;
    }
    if (row == SR_RES) {
        unsigned int mb = ui_mode_mb_rate();
        snprintf(out, n, "%s%s", h,
                 (mb > 522240u) ? " This is beyond what the PS3 decoder can do."
                 : (mb > 245760u) ? " It needs the decoder's Level 4.2 mode." : "");
    } else {
        snprintf(out, n, "%s", h);
    }
}

void ui_summary_parts(char parts[4][24]) {
    char b[24];
    const char *nm = res_name(ui_get_stream_width(), ui_get_stream_height(), b, sizeof(b));
    snprintf(parts[0], 24, "%s", nm);
    snprintf(parts[1], 24, "%d FPS", ui_get_fps());
    int kbps = ui_get_bitrate();
    if (kbps % 1000 == 0) snprintf(parts[2], 24, "%d Mbps", kbps / 1000);
    else snprintf(parts[2], 24, "%.1f Mbps", (float)kbps / 1000.0f);
    snprintf(parts[3], 24, "%s", ui_audio_channels == 8 ? "7.1 Surround"
                                : ui_audio_channels == 6 ? "5.1 Surround" : "Stereo");
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
    case SR_RES:     ui_res_idx = settings_step(ui_res_idx, dir, NUM_RES_OPTIONS); return 1;
    case SR_ASPECT:  ui_aspect_mode = !ui_aspect_mode; return 1;
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
    case SR_THEME:   ui_theme_set_mode(settings_step(ui_theme_get_mode(), dir, 3)); return 1;
    default: return 0;
    }
}

int ui_settings_change(int row, int dir) {
    if (!settings_change(row, dir)) return 0;
    ui_save_settings();
    return 1;
}

// Overscan: shrink the whole picture toward the centre and shift it.  Applied
// to the 2D viewport before anything is drawn, so menus and the video quad move
// together.  At 100% / 0 this is the identity (the viewport tiny3d already used).
// Offsets are in 1280x720 units.
static void ui_apply_overscan(void) {
    float sx = (float)ui_ovs_x / 100.0f, sy = (float)ui_ovs_y / 100.0f;
    float px = ((float)ui_width * (1.0f - sx)) * 0.5f + (float)ui_ovs_xoff * (float)ui_width / 1280.0f;
    float py = ((float)ui_height * (1.0f - sy)) * 0.5f + (float)ui_ovs_yoff * (float)ui_height / 720.0f;
    tiny3d_UserViewport(1, px, py, sx, sy, 1.0f, 1.0f);
}

static void ui_loop(void *arg) {
    (void)arg;
    ps3_pad_state_t pad;
    ps3_pad_state_t no_pad;
    memset(&no_pad, 0, sizeof(no_pad));

    tiny3d_Init(1024 * 1024); // 1MB vertex buffer
    ui_fonts_init();
    ui_screens_init();

    // Alpha test and blending keep the font glyphs from drawing black boxes.
    tiny3d_AlphaTest(1, 0, TINY3D_ALPHA_FUNC_GREATER);
    ui_blend_set(UI_BLEND_NORMAL);

    int frame = 0;
    while (ui_running) {
        // Pump sysutil event callbacks to service OSK and GameOS events
        sysUtilCheckCallback();
        ps3input_get_data(&pad);
        ui_draw_frame_begin();
        if ((frame++ % 600) == 0) ui_theme_update();

        // The clear colour fills whatever the overscan viewport leaves bare, so
        // it is the sky's middle stop in menus and black behind a stream (the
        // colour of the bars round a stream whose shape does not match).
        if (ui_state == UI_STATE_STREAMING) {
            tiny3d_Clear(0x000000ff, TINY3D_CLEAR_ALL);
        } else {
            const ui_theme_t *t = ui_theme();
            u32 c = ((u32)(t->bg_mid.r * 255.0f) << 24) | ((u32)(t->bg_mid.g * 255.0f) << 16) |
                    ((u32)(t->bg_mid.b * 255.0f) << 8) | 0xff;
            tiny3d_Clear(c, TINY3D_CLEAR_ALL);
        }

        // Menus take no input while the OSK or a native dialog owns the pad.
        ui_screens_input(ui_modal_active() ? &no_pad : &pad);

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

        ui_apply_overscan();
        tiny3d_UserViewportSurface(1, (float)ui_width, (float)ui_height);
        tiny3d_Project2D();
        tiny3d_AlphaTest(1, 0, TINY3D_ALPHA_FUNC_GREATER);
        ui_blend_set(UI_BLEND_NORMAL);

        if (ui_state == UI_STATE_STREAMING) {
            ps3video_draw();
            ui_blend_set(UI_BLEND_NORMAL);   // the video quad may have changed it
            ui_screens_draw_hud();
        } else {
            ui_bg_draw();
            ui_screens_draw();
        }
        ui_screens_draw_overlays();

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
    ui_fonts_shutdown();
    if (log_mutex_initialized) {
        sysMutexDestroy(log_mutex);
        log_mutex_initialized = 0;
    }
}
