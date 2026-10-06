#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

// What ui.c shares with ui_screens.c.  Not for main.c: main.c talks to ui.h.

#include "ui.h"
#include "input.h"

// Settings rows.  The ids and their config.ini keys are unchanged from 1.4.0;
// only labels, grouping and wording are new.
enum {
    SR_FPS, SR_RES, SR_ASPECT, SR_BITRATE, SR_PIXFMT, SR_DEBLOCK, SR_PRESENT, SR_NTSC, SR_SPUS,
    SR_PACKET, SR_INTRA, SR_VDISPLAY, SR_QUITEXIT, SR_AUDIO,
    SR_MOUSE, SR_RUMBLE, SR_TRIGGERS,
    SR_VSYNC, SR_OVS_X, SR_OVS_Y, SR_OVS_XOFF, SR_OVS_YOFF, SR_STATS, SR_VERBOSE,
    SR_THEME,
    SETTINGS_ITEM_COUNT
};

int         ui_settings_cat_count(void);
const char *ui_settings_cat_name(int cat);
const char *ui_settings_cat_note(int cat);               // "" when none
int         ui_settings_cat_rows(int cat, const int **rows);
const char *ui_settings_label(int row);
void        ui_settings_help(int row, char *out, size_t n);
void        ui_settings_value(int row, char *out, size_t n);
// Apply one press (dir = +1 / -1).  Saves config.ini when a value changed.
int         ui_settings_change(int row, int dir);

// Log.
#define UI_LOG_WIDTH 100
int  ui_log_snapshot(char (*out)[UI_LOG_WIDTH], int max_lines);

// A native Yes/No dialog.  Returns 0 if one is already open.
int  ui_confirm(const char *text, void (*done)(int yes, void *user), void *user);
// True while the OSK or a native dialog owns the pad.
int  ui_modal_active(void);

// Shared state, owned by ui.c.
extern ps3_app_list_t   current_app_list;
extern int              active_app_idx;
extern volatile int     app_selection_confirmed;
extern char             pairing_pin_str[16];
extern char             ui_error_detail[192];
extern volatile int     host_running_app;       // running app id, 0 = none
extern char             host_running_name[64];
extern volatile int     host_is_apollo;
extern volatile int     quit_request;
extern volatile int     switch_quit_request;    // quit the running app, then launch
extern mld_host_t       discovered_hosts[MLD_MAX_HOSTS];
extern int              discovered_host_count;
extern int              discovery_scanned;
extern int              active_host_idx;
extern volatile int     host_selection_confirmed;
extern volatile int     manual_entry_requested;
extern int              show_stats;
extern int              ui_fps_actual;

// Stream summary, in plain words, for the home screen.
void ui_summary_parts(char parts[4][24]);

#endif
