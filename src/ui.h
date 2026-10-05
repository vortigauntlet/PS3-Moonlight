#ifndef UI_H
#define UI_H

#include <stdint.h>
#include "handshake.h"
#include "moonlight_discovery.h"

enum {
    UI_STATE_IP_ENTRY,
    UI_STATE_SETTINGS,
    UI_STATE_PAIRING,
    UI_STATE_APPLIST,
    UI_STATE_STREAMING,
    UI_STATE_ERROR,
    UI_STATE_DISCOVERY
};

void ui_init(int width, int height);
// The console's active output mode (VIDEO_REFRESH_* bits, VIDEO_ASPECT_*).
// Call before ui_init; AUTO frame rate and resolution are resolved from it.
void ui_set_output_mode(int refresh_bits, int aspect);
int ui_output_is_50hz(void);
int ui_output_is_4x3(void);
int ui_get_aspect_mode(void);
// Screen rectangle for the stream quad, honouring the Picture Shape setting.
void ui_stream_rect(int screen_w, int screen_h, float *x, float *y, float *w, float *h);
void ui_push_log(const char *msg);
void ui_set_state(int state);
int ui_get_state();
int ui_is_running();
int ui_get_fps();
int ui_get_bitrate();
int ui_get_refresh_x100(void); // 5994 for 59.94 fps
int ui_get_packet_size();
int ui_get_stream_width(void);
int ui_get_stream_height(void);
int ui_get_pixel_format(void);
int ui_get_intra_refresh(void);
int ui_get_no_deblock(void);
int ui_get_rumble(void);
int ui_get_trigger_mode(void); // 1 = analog (pressure), 0 = digital
int ui_get_vdec_spus(void);
const char* ui_get_target_ip();
int ui_get_width();
int ui_get_height();
void ui_stop();
void ui_shutdown();
void ui_open_osk(void);
void ui_open_exit_dialog(void);
void ui_set_target_ip(const char *ip);
int ui_get_vsync();
int ui_get_show_stats();
int ui_get_verbose();
int ui_get_mouse_mode(void);
void ui_save_settings(void);
void ui_load_settings(void);
void ui_set_pairing_pin(const char *pin);
const char* ui_get_pairing_pin(void);

// App selection state helpers
void ui_set_app_list(const ps3_app_list_t *list);
int ui_get_selected_app_id(void);
const char* ui_get_selected_app_name(void);
const char* ui_get_selected_app_uuid(void);
int ui_get_virtual_display(void);
int ui_get_quit_on_exit(void);
int ui_get_audio_channels(void);
int ui_get_audio_configuration(void); // AUDIO_CONFIGURATION_* for the stream
int ui_get_low_latency(void);
// Extra line under the error headline (e.g. the host's own rejection reason).
void ui_set_error_detail(const char *msg);
int ui_is_app_selected(void);
void ui_reset_app_selection(void);

// Multi-host saved config
#define UI_MAX_SAVED_HOSTS 8

typedef struct {
    char name[64];
    char address[16];
    char uuid[40];   /* host <uniqueid> from /serverinfo; "" until first contact */
    int  paired;
    int  last_app_id; /* -1 = nessuno */
} ui_saved_host_t;

int  ui_get_saved_host_count(void);
const ui_saved_host_t *ui_get_saved_host(int idx);
int  ui_get_selected_host_index(void);
void ui_select_host(int idx);
int  ui_upsert_saved_host(const char *name, const char *address);
void ui_set_host_paired(int idx, int paired);
void ui_set_host_last_app(int idx, int app_id);
void ui_set_host_uuid(int idx, const char *uuid);
int  ui_find_host_by_uuid(const char *uuid);
/* Re-point a saved host at a new address (it moved); keeps pairing and uuid. */
void ui_set_host_address(int idx, const char *address, const char *name);

/* What the selected host reports, refreshed by the main thread.  running_app:
 * 0 = nothing running, else an app is; name may be "". */
void ui_set_host_status(int running_app, const char *app_name, int apollo_family);
/* The user picked "Quit app on host"; returns 1 once per request. */
int  ui_take_quit_request(void);

// Host discovery state helpers
void ui_set_discovered_hosts(const mld_host_t *hosts, int count);
int  ui_is_host_selected(void);
int  ui_wants_manual_entry(void);
int  ui_get_selected_host_ip(char *out, size_t out_size);
int  ui_get_selected_host_name(char *out, size_t out_size);
void ui_reset_host_selection(void);

#endif
