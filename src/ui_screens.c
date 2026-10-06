#include "ui_screens.h"
#include "ui_internal.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "ui_draw.h"
#include <tiny3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "video.h"
#include "audio.h"

#define PI_F 3.14159265f

static const ui_col_t BLACK_0  = { 0.0f, 0.0f, 0.0f, 0.0f };
static const ui_col_t SHADE    = { 0.0f, 0.02f, 0.06f, 0.30f };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ===========================================================================
// Toasts
// ===========================================================================
static char toast_msg[96];
static volatile unsigned toast_seq;
static unsigned toast_seen;
static float toast_age = 1e9f;
static float toast_hold = UI_TOAST_HOLD_S;

void ui_toast_for(const char *msg, float hold_seconds) {
    if (!msg) return;
    snprintf(toast_msg, sizeof(toast_msg), "%s", msg);
    toast_hold = hold_seconds;
    toast_seq++;
}
void ui_toast(const char *msg) { ui_toast_for(msg, UI_TOAST_HOLD_S); }

static void draw_toast(void) {
    if (toast_seq != toast_seen) { toast_seen = toast_seq; toast_age = 0.0f; }
    toast_age += ui_dt();
    float total = UI_TOAST_IN_S + toast_hold + UI_TOAST_OUT_S;
    if (toast_age >= total) return;
    float a = 1.0f;
    if (toast_age < UI_TOAST_IN_S) a = toast_age / UI_TOAST_IN_S;
    else if (toast_age > UI_TOAST_IN_S + toast_hold) a = 1.0f - (toast_age - UI_TOAST_IN_S - toast_hold) / UI_TOAST_OUT_S;

    char buf[96];
    float max_w = ui_lay.lw - 2.0f * UI_SAFE_X - 64.0f;
    ui_fit_string(UI_FACE_BODY, UI_T_SECOND, max_w, toast_msg, buf, sizeof(buf));
    float tw = ui_text_width(UI_FACE_BODY, UI_T_SECOND, buf);
    float w = tw + 56.0f, h = 46.0f;
    float x = ui_centre() - w * 0.5f;
    float y = ui_bottom() - 28.0f - 20.0f - h - 6.0f;   // above the footer

    float sa = ui_get_alpha(), sx = ui_get_offset_x(), sy = ui_get_offset_y();
    ui_set_alpha(a);
    ui_set_offset(0.0f, (1.0f - a) * 10.0f);
    ui_glass(x, y, w, h, h * 0.5f, 0.0f);
    ui_text(UI_FACE_BODY, UI_T_SECOND, ui_centre(), y + 10.0f, UI_ALIGN_CENTER, UI_TEXT, buf);
    ui_set_alpha(sa);
    ui_set_offset(sx, sy);
}

// ===========================================================================
// Chrome: top bar, footer, log drawer
// ===========================================================================
static void clock_string(char *out, size_t n) {
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm) { out[0] = '\0'; return; }
    snprintf(out, n, "%s %d %s   %02d:%02d", days[tm->tm_wday % 7], tm->tm_mday,
             months[tm->tm_mon % 12], tm->tm_hour, tm->tm_min);
}

static void draw_topbar(const char *title) {
    // A soft shade behind the bar keeps the clock readable over the wave.
    ui_rect(0.0f, 0.0f, ui_lay.lw, 120.0f, SHADE, BLACK_0);
    ui_moon(ui_left() + 14.0f, 68.0f, 17.0f);
    ui_text_fit(UI_FACE_HEAD, UI_T_CARD, ui_left() + 44.0f, 50.0f, UI_ALIGN_LEFT, UI_TEXT,
                ui_lay.lw * 0.5f, title ? title : "Moonlight");
    char clk[48];
    clock_string(clk, sizeof(clk));
    ui_text(UI_FACE_BODY, UI_T_SECOND, ui_right(), 54.0f, UI_ALIGN_RIGHT, UI_TEXT_2, clk);
}

static void draw_title(const char *s) {
    ui_text_shadow(UI_FACE_HEAD, UI_T_TITLE, ui_left(), 108.0f, UI_ALIGN_LEFT, UI_TEXT, s);
}

typedef struct { const char *key; ui_col_t col; const char *label; } foot_t;

static float foot_item(float x, float y, const foot_t *f) {
    float w;
    if (f->key[0] >= 1 && f->key[0] <= 5) {
        w = ui_text(UI_FACE_BODY, 28.0f, x, y - 5.0f, UI_ALIGN_LEFT, f->col, f->key) + 8.0f;
    } else {
        float tw = ui_text_width(UI_FACE_BODY, UI_T_MIN, f->key);
        float bw = tw + 22.0f;
        ui_rrect(x, y - 1.0f, bw, 30.0f, 15.0f, SHADE, SHADE);
        ui_ring(x, y - 1.0f, bw, 30.0f, 15.0f, UI_STROKE, ui_col_a(UI_TEXT_2, 0.55f), ui_col_a(UI_TEXT_2, 0.25f));
        ui_text(UI_FACE_BODY, UI_T_MIN, x + 11.0f, y + 3.0f, UI_ALIGN_LEFT, UI_TEXT_2, f->key);
        w = bw + 8.0f;
    }
    return w + ui_text(UI_FACE_BODY, UI_T_HINT, x + w, y, UI_ALIGN_LEFT, UI_TEXT_2, f->label);
}

static void draw_footer(const foot_t *items, int n) {
    float fy = ui_bottom() - 28.0f;
    ui_rect(0.0f, fy - 40.0f, ui_lay.lw, UI_LH - (fy - 40.0f), BLACK_0, SHADE);
    float x = ui_left();
    for (int i = 0; i < n; i++) x += foot_item(x, fy, &items[i]) + 32.0f;
}

#define F_CROSS(l)    { UI_G_CROSS, UI_PS_CROSS, l }
#define F_CIRCLE(l)   { UI_G_CIRCLE, UI_PS_CIRCLE, l }
#define F_TRIANGLE(l) { UI_G_TRIANGLE, UI_PS_TRIANGLE, l }
#define F_SQUARE(l)   { UI_G_SQUARE, UI_PS_SQUARE, l }
#define F_KEY(k, l)   { k, UI_TEXT_2, l }

static int   log_open;
static float log_t;

static void draw_log_drawer(void) {
    log_t = ui_approach(log_t, log_open ? 1.0f : 0.0f, 16.0f);
    if (log_t < 0.01f) return;
    float e = log_t;
    float sa = ui_get_alpha();
    const int lines = 12;
    float h = 40.0f + lines * 24.0f + 30.0f;
    float x = 40.0f, w = ui_lay.lw - 80.0f;
    float y = UI_LH + 12.0f - h * e;
    ui_set_alpha(sa);
    ui_glass(x, y, w, h, UI_R_CARD, 0.0f);
    ui_text(UI_FACE_HEAD, UI_T_MIN, x + 24.0f, y + 12.0f, UI_ALIGN_LEFT, UI_TEXT, "Log");
    ui_text(UI_FACE_BODY, UI_T_MIN, x + w - 24.0f, y + 12.0f, UI_ALIGN_RIGHT, UI_TEXT_3, "SELECT to close");
    char buf[12][UI_LOG_WIDTH];
    int n = ui_log_snapshot(buf, lines);
    for (int i = 0; i < n; i++)
        ui_text_fit(UI_FACE_BODY, UI_T_MIN, x + 24.0f, y + 44.0f + i * 24.0f, UI_ALIGN_LEFT,
                    UI_TEXT_2, w - 48.0f, buf[i]);
}

// ===========================================================================
// Shared widgets
// ===========================================================================
static unsigned hash_str(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

// HSV to RGB, all 0..1.
static ui_col_t hsv(float h, float s, float v, float a) {
    float r = v, g = v, b = v;
    float hh = (h - floorf(h)) * 6.0f;
    int i = (int)hh;
    float f = hh - (float)i;
    float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    ui_col_t c = { r, g, b, a };
    return c;
}

static void centre_message(const char *title, const char *body) {
    ui_text_shadow(UI_FACE_HEAD, UI_T_CARD, ui_centre(), 356.0f, UI_ALIGN_CENTER, UI_TEXT, title);
    if (body && *body)
        ui_text_wrap(UI_FACE_BODY, UI_T_SECOND, ui_centre(), 400.0f, UI_ALIGN_CENTER, UI_TEXT_2,
                     ui_lay.lw - 2.0f * UI_SAFE_X - 120.0f, 30.0f, 3, body);
}

// ===========================================================================
// Screen transition
// ===========================================================================
static int   shown_state = -1;
static float trans_t = 1.0f;
static int   trans_dir = 1;

static int state_depth(int s) {
    switch (s) {
    case UI_STATE_IP_ENTRY: return 0;
    case UI_STATE_DISCOVERY:
    case UI_STATE_SETTINGS:
    case UI_STATE_PAIRING:  return 1;
    case UI_STATE_ERROR:
    case UI_STATE_APPLIST:  return 2;
    default:                return 3;
    }
}

// ===========================================================================
// Home: Choose a PC
// ===========================================================================
#define CARD_W   240.0f
#define CARD_H   280.0f
#define CARD_GAP 32.0f
#define CARD_Y   190.0f
#define MAX_ITEMS (UI_MAX_SAVED_HOSTS + 2)

enum { IT_HOST, IT_ADD, IT_SETTINGS, IT_EMPTY };

static int   home_focus;
static float home_first_f;
static int   home_first_i;
static float card_focus[MAX_ITEMS];
static int   rest_focus = -1;
static float rest_time;
static int   opt_open;
static float opt_t;
static int   opt_item;
static int   opt_info;

static int home_items(int *kinds) {
    int nh = ui_get_saved_host_count();
    int n = 0;
    if (nh == 0) {
        kinds[n++] = IT_EMPTY;
    } else {
        for (int i = 0; i < nh; i++) kinds[n++] = IT_HOST;
        kinds[n++] = IT_ADD;
    }
    kinds[n++] = IT_SETTINGS;
    return n;
}

static void home_enter(void) {
    int sel = ui_get_selected_host_index();
    home_focus = (sel >= 0) ? sel : 0;
    opt_open = 0; opt_t = 0.0f; opt_info = 0;
    rest_focus = home_focus; rest_time = 0.0f;
    float avail = ui_right() - ui_left();
    int fit = ui_shelf_fit(avail, CARD_W, CARD_GAP);
    int kinds[MAX_ITEMS];
    home_first_i = ui_shelf_first(home_focus, 0, fit, home_items(kinds));
    home_first_f = (float)home_first_i;
    memset(card_focus, 0, sizeof(card_focus));
}

static void host_display_name(const ui_saved_host_t *h, char *out, size_t n) {
    if (!strcmp(h->name, "Manual Entry") || !h->name[0] || !strcmp(h->name, h->address))
        snprintf(out, n, "%s", h->address);
    else
        snprintf(out, n, "%s", h->name);
}

static void connect_to(int idx) {
    ui_select_host(idx);
    ui_set_state(UI_STATE_PAIRING);
}

static void remove_confirmed(int yes, void *user) {
    int idx = (int)(intptr_t)user;
    if (!yes) return;
    const ui_saved_host_t *h = ui_get_saved_host(idx);
    char name[64] = "PC";
    if (h) host_display_name(h, name, sizeof(name));
    ui_remove_saved_host(idx);
    char m[96];
    snprintf(m, sizeof(m), "Removed %s", name);
    ui_toast(m);
    int kinds[MAX_ITEMS];
    int n = home_items(kinds);
    home_focus = clampi(home_focus, 0, n - 1);
}

enum { OP_CONNECT, OP_QUIT, OP_INFO, OP_REMOVE };

static int options_items(int *out) {
    int n = 0;
    out[n++] = OP_CONNECT;
    if (host_running_app && home_focus == ui_get_selected_host_index()) out[n++] = OP_QUIT;
    out[n++] = OP_INFO;
    out[n++] = OP_REMOVE;
    return n;
}

static void options_input(const ps3_pad_state_t *p) {
    int items[4];
    int n = options_items(items);
    if (opt_info) {
        if (p->buttons_pressed & (B_FLAG | Y_FLAG | A_FLAG)) opt_info = 0;
        return;
    }
    opt_item = clampi(opt_item, 0, n - 1);
    if (p->buttons_pressed & UP_FLAG)   opt_item = (opt_item + n - 1) % n;
    if (p->buttons_pressed & DOWN_FLAG) opt_item = (opt_item + 1) % n;
    if (p->buttons_pressed & (B_FLAG | Y_FLAG)) { opt_open = 0; return; }
    if (p->buttons_pressed & A_FLAG) {
        switch (items[opt_item]) {
        case OP_CONNECT: opt_open = 0; connect_to(home_focus); break;
        case OP_QUIT:    opt_open = 0; quit_request = 1; break;
        case OP_INFO:    opt_info = 1; break;
        case OP_REMOVE: {
            const ui_saved_host_t *h = ui_get_saved_host(home_focus);
            char name[64] = "this PC", q[160];
            if (h) host_display_name(h, name, sizeof(name));
            snprintf(q, sizeof(q), "Remove %s from your PCs? You will need to pair again to use it.", name);
            opt_open = 0;
            ui_confirm(q, remove_confirmed, (void *)(intptr_t)home_focus);
            break;
        }
        }
    }
}

static void home_input(const ps3_pad_state_t *p) {
    int kinds[MAX_ITEMS];
    int n = home_items(kinds);
    home_focus = clampi(home_focus, 0, n - 1);

    if (opt_open) { options_input(p); return; }

    if (p->buttons_pressed & LEFT_FLAG)  home_focus = clampi(home_focus - 1, 0, n - 1);
    if (p->buttons_pressed & RIGHT_FLAG) home_focus = clampi(home_focus + 1, 0, n - 1);

    int kind = kinds[home_focus];
    if (p->buttons_pressed & A_FLAG) {
        if (kind == IT_HOST) connect_to(home_focus);
        else if (kind == IT_ADD || kind == IT_EMPTY) { ui_reset_host_selection(); ui_set_state(UI_STATE_DISCOVERY); }
        else ui_set_state(UI_STATE_SETTINGS);
    }
    if ((p->buttons_pressed & Y_FLAG) && kind == IT_HOST) {
        opt_open = 1; opt_item = 0; opt_info = 0;
        ui_select_host(home_focus);
    }
    // START still connects to the focused PC, as it always has.
    if ((p->buttons_pressed & PLAY_FLAG) && kind == IT_HOST) connect_to(home_focus);
    if (p->buttons_pressed & B_FLAG) ui_open_exit_dialog();
}

static void draw_card_host(float x, float y, float f, float alpha, int idx) {
    const ui_saved_host_t *h = ui_get_saved_host(idx);
    if (!h) return;
    float s = 1.0f + (UI_FOCUS_SCALE - 1.0f) * f;
    float w = CARD_W * s, hh = CARD_H * s;
    float cx = x + CARD_W * 0.5f;
    float cy = y + CARD_H * 0.5f - UI_FOCUS_LIFT * f;
    float x0 = cx - w * 0.5f, y0 = cy - hh * 0.5f;

    float sa = ui_get_alpha();
    ui_set_alpha(sa * alpha);
    ui_glass(x0, y0, w, hh, UI_R_CARD, f);
    if (f > 0.02f) ui_glow(x0, y0, w, hh, UI_R_CARD, ui_glow_breath() * f);

    ui_col_t ic = ui_col_mix(UI_TEXT_2, UI_ACCENT, f);
    ui_icon(UI_ICON_COMPUTER, cx, y0 + 92.0f * s, 104.0f * s, ic);
    if (!h->paired) {
        float lx0 = cx + 46.0f * s, ly0 = y0 + 128.0f * s;
        ui_circle(lx0, ly0, 21.0f, ui_col_a(SHADE, 0.85f), ui_col_a(SHADE, 0.85f));
        ui_icon(UI_ICON_LOCK, lx0, ly0, 30.0f, UI_WARN);
    }

    char name[64];
    host_display_name(h, name, sizeof(name));
    ui_text_fit(UI_FACE_HEAD, UI_T_CARD, cx, y0 + 168.0f * s, UI_ALIGN_CENTER, UI_TEXT, w - 28.0f, name);

    char lab[96];
    ui_col_t col;
    if (idx == ui_get_selected_host_index() && host_running_app && h->paired) {
        char app[64];
        snprintf(app, sizeof(app), "%s", host_running_name[0] ? host_running_name : "a game");
        char pre[96];
        snprintf(pre, sizeof(pre), "Playing %s", app);
        ui_fit_string(UI_FACE_BODY, UI_T_MIN, w - 28.0f - 46.0f, pre, lab, sizeof(lab));
        col = UI_OK;
    } else if (h->paired) { snprintf(lab, sizeof(lab), "Ready"); col = UI_OK; }
    else { snprintf(lab, sizeof(lab), "Not paired"); col = UI_WARN; }
    float pw = ui_pill_width(lab);
    ui_pill(cx - pw * 0.5f, y0 + 222.0f * s, lab, col);
    ui_set_alpha(sa);
}

static void draw_card_simple(float x, float y, float f, float alpha, int icon, const char *label, int ghost) {
    float s = 1.0f + (UI_FOCUS_SCALE - 1.0f) * f;
    float w = CARD_W * s, hh = CARD_H * s;
    float cx = x + CARD_W * 0.5f;
    float cy = y + CARD_H * 0.5f - UI_FOCUS_LIFT * f;
    float x0 = cx - w * 0.5f, y0 = cy - hh * 0.5f;
    float sa = ui_get_alpha();
    ui_set_alpha(sa * alpha);
    if (ghost) {
        // The "Add" card: an outline of glass rather than glass.
        ui_col_t fill = { 1.0f, 1.0f, 1.0f, 0.04f + 0.06f * f };
        ui_rrect(x0, y0, w, hh, UI_R_CARD, fill, fill);
        ui_ring(x0, y0, w, hh, UI_R_CARD, UI_STROKE, ui_col_a(UI_ACCENT, 0.35f + 0.6f * f),
                ui_col_a(UI_ACCENT, 0.15f + 0.4f * f));
    } else {
        ui_glass(x0, y0, w, hh, UI_R_CARD, f);
    }
    if (f > 0.02f) ui_glow(x0, y0, w, hh, UI_R_CARD, ui_glow_breath() * f);
    ui_icon(icon, cx, y0 + 110.0f * s, 88.0f * s, ui_col_mix(UI_TEXT_2, UI_ACCENT, f));
    ui_text_fit(UI_FACE_HEAD, UI_T_CARD, cx, y0 + 196.0f * s, UI_ALIGN_CENTER, UI_TEXT, w - 28.0f, label);
    ui_set_alpha(sa);
}

static void draw_empty_card(float x, float y, float f, float alpha) {
    float w = 520.0f;
    float s = 1.0f + (UI_FOCUS_SCALE - 1.0f) * f;
    float cx = x + w * 0.5f, cy = y + CARD_H * 0.5f - UI_FOCUS_LIFT * f;
    float ww = w * s, hh = CARD_H * s;
    float x0 = cx - ww * 0.5f, y0 = cy - hh * 0.5f;
    float sa = ui_get_alpha();
    ui_set_alpha(sa * alpha);
    ui_glass(x0, y0, ww, hh, UI_R_CARD, f);
    if (f > 0.02f) ui_glow(x0, y0, ww, hh, UI_R_CARD, ui_glow_breath() * f);
    ui_icon(UI_ICON_COMPUTER, x0 + 70.0f, y0 + 78.0f, 80.0f, UI_ACCENT);
    ui_text_shadow(UI_FACE_HEAD, 34.0f, x0 + 128.0f, y0 + 48.0f, UI_ALIGN_LEFT, UI_TEXT, "Let's find your PC");
    ui_text_wrap(UI_FACE_BODY, UI_T_SECOND, x0 + 36.0f, y0 + 120.0f, UI_ALIGN_LEFT, UI_TEXT_2,
                 ww - 72.0f, 30.0f, 4,
                 "Make sure Sunshine or Apollo is running on your PC and that it's on the same network as your PS3.");
    foot_t search = F_CROSS("Search for PCs");
    foot_item(x0 + 36.0f, y0 + hh - 56.0f, &search);
    ui_set_alpha(sa);
}

static void draw_summary(float y) {
    char parts[4][24];
    ui_summary_parts(parts);
    const float size = UI_T_SECOND, gap = 36.0f;
    float total = 0.0f;
    for (int i = 0; i < 4; i++) total += ui_text_width(UI_FACE_BODY, size, parts[i]);
    total += gap * 3.0f;
    float x = ui_centre() - total * 0.5f;
    for (int i = 0; i < 4; i++) {
        x += ui_text(UI_FACE_BODY, size, x, y, UI_ALIGN_LEFT, UI_TEXT_2, parts[i]);
        if (i < 3) {
            ui_circle(x + gap * 0.5f, y + 12.0f, 3.5f, ui_col_a(UI_ACCENT, 0.9f), ui_col_a(UI_ACCENT, 0.6f));
            x += gap;
        }
    }
}

static void draw_options_panel(void) {
    opt_t = ui_approach(opt_t, opt_open ? 1.0f : 0.0f, 18.0f);
    if (opt_t < 0.01f) return;
    float e = opt_t;
    float sa = ui_get_alpha();

    // Dim the shelf behind it.
    ui_set_alpha(sa * e);
    ui_rect(0.0f, 0.0f, ui_lay.lw, UI_LH, ui_col_a(BLACK_0, 0.35f), ui_col_a(BLACK_0, 0.35f));
    ui_set_alpha(sa);

    const ui_saved_host_t *h = ui_get_saved_host(home_focus);
    if (!h) return;
    int items[4];
    int n = options_items(items);
    float pw = 380.0f, row_h = 58.0f;
    float ph = 96.0f + (opt_info ? 4 : n) * row_h + 24.0f;
    float px = ui_right() - pw + (1.0f - e) * (pw + UI_SAFE_X + 20.0f);
    float py = 128.0f;
    ui_glass(px, py, pw, ph, UI_R_CARD, 0.0f);

    char name[64];
    host_display_name(h, name, sizeof(name));
    ui_text_fit(UI_FACE_HEAD, UI_T_CARD, px + 28.0f, py + 24.0f, UI_ALIGN_LEFT, UI_TEXT, pw - 56.0f, name);
    ui_rect(px + 24.0f, py + 70.0f, pw - 48.0f, UI_STROKE, ui_col_a(UI_TEXT, 0.25f), ui_col_a(UI_TEXT, 0.10f));

    if (opt_info) {
        char l[96];
        float y = py + 90.0f;
        snprintf(l, sizeof(l), "Address   %s", h->address);
        ui_text_fit(UI_FACE_BODY, UI_T_BODY, px + 28.0f, y, UI_ALIGN_LEFT, UI_TEXT, pw - 56.0f, l);
        snprintf(l, sizeof(l), "Pairing   %s", h->paired ? "Paired" : "Not paired");
        ui_text(UI_FACE_BODY, UI_T_BODY, px + 28.0f, y + 44.0f, UI_ALIGN_LEFT, h->paired ? UI_OK : UI_WARN, l);
        if (host_is_apollo && home_focus == ui_get_selected_host_index())
            ui_text_fit(UI_FACE_BODY, UI_T_SECOND, px + 28.0f, y + 88.0f, UI_ALIGN_LEFT, UI_TEXT_2,
                        pw - 56.0f, "Vibepollo / Apollo host");
        else
            ui_text(UI_FACE_BODY, UI_T_SECOND, px + 28.0f, y + 88.0f, UI_ALIGN_LEFT, UI_TEXT_3,
                    h->uuid[0] ? "Remembered by its own ID" : "Not contacted yet");
        return;
    }

    for (int i = 0; i < n; i++) {
        float ry = py + 86.0f + i * row_h;
        int focus = (i == opt_item);
        char label[96];
        int icon = UI_ICON_INFO;
        switch (items[i]) {
        case OP_CONNECT: snprintf(label, sizeof(label), "Connect"); icon = UI_ICON_PLAY; break;
        case OP_QUIT:
            snprintf(label, sizeof(label), "Quit %s on PC", host_running_name[0] ? host_running_name : "game");
            icon = UI_ICON_STOP; break;
        case OP_INFO:    snprintf(label, sizeof(label), "PC info"); icon = UI_ICON_INFO; break;
        default:         snprintf(label, sizeof(label), "Remove PC"); icon = UI_ICON_DELETE; break;
        }
        if (focus) {
            ui_glass(px + 14.0f, ry, pw - 28.0f, row_h - 8.0f, UI_R_ROW, 1.0f);
            ui_glow(px + 14.0f, ry, pw - 28.0f, row_h - 8.0f, UI_R_ROW, ui_glow_breath());
        }
        ui_icon(icon, px + 48.0f, ry + (row_h - 8.0f) * 0.5f, 30.0f,
                items[i] == OP_REMOVE ? UI_BAD : (focus ? UI_ACCENT : UI_TEXT_2));
        ui_text_fit(UI_FACE_BODY, UI_T_BODY, px + 76.0f, ry + 11.0f, UI_ALIGN_LEFT, UI_TEXT, pw - 100.0f, label);
    }
}

static void draw_home(void) {
    int kinds[MAX_ITEMS];
    int n = home_items(kinds);
    home_focus = clampi(home_focus, 0, n - 1);

    draw_topbar("Moonlight");
    draw_title("Choose a PC");

    float left = ui_left(), right = ui_right();
    int fit = ui_shelf_fit(right - left, CARD_W, CARD_GAP);
    home_first_i = ui_shelf_first(home_focus, home_first_i, fit, n);
    home_first_f = ui_approach(home_first_f, (float)home_first_i, UI_FOCUS_K);

    // The wide empty-state card is its own width, so it is laid out by hand.
    for (int i = 0; i < n; i++) {
        float target = (i == home_focus && !opt_open) ? 1.0f : (i == home_focus ? 0.6f : 0.0f);
        card_focus[i] = ui_approach(card_focus[i], target, UI_FOCUS_K);
    }
    for (int i = 0; i < n; i++) {
        float x = ui_shelf_card_x(left, CARD_W, CARD_GAP, i, home_first_f);
        if (kinds[0] == IT_EMPTY && i == 1) x = left + 520.0f + CARD_GAP;
        float over = 0.0f;                        // how far outside the safe row
        if (x < left) over = left - x;
        if (x + CARD_W > right) over = x + CARD_W - right;
        float alpha = clampf(1.0f - over / (CARD_W * 0.6f), 0.0f, 1.0f);
        if (alpha <= 0.01f) continue;
        float f = card_focus[i];
        switch (kinds[i]) {
        case IT_HOST:     draw_card_host(x, CARD_Y, f, alpha, i); break;
        case IT_ADD:      draw_card_simple(x, CARD_Y, f, alpha, UI_ICON_ADD, "Add PC", 1); break;
        case IT_SETTINGS: draw_card_simple(x, CARD_Y, f, alpha, UI_ICON_SETTINGS, "Settings", 0); break;
        default:          draw_empty_card(left, CARD_Y, f, alpha); break;
        }
    }
    // Chevrons when more cards wait off to a side.
    if (home_first_i > 0)
        ui_text(UI_FACE_HEAD, UI_T_CARD, left - 36.0f, CARD_Y + 120.0f, UI_ALIGN_LEFT, UI_TEXT_2, "<");
    if (home_first_i + fit < n)
        ui_text(UI_FACE_HEAD, UI_T_CARD, right + 12.0f, CARD_Y + 120.0f, UI_ALIGN_LEFT, UI_TEXT_2, ">");

    draw_summary(CARD_Y + CARD_H + 44.0f);

    int kind = kinds[home_focus];
    if (opt_open) {
        foot_t of[3];
        int no = 0;
        of[no++] = (foot_t)F_CROSS(opt_info ? "Back" : "Choose");
        of[no++] = (foot_t)F_CIRCLE("Close");
        draw_footer(of, no);
    } else {
        foot_t foot[4];
        int nf = 0;
        foot[nf++] = (foot_t)F_CROSS(kind == IT_HOST ? "Connect" : kind == IT_SETTINGS ? "Open" : "Search");
        if (kind == IT_HOST) foot[nf++] = (foot_t)F_TRIANGLE("Options");
        foot[nf++] = (foot_t)F_CIRCLE("Exit");
        foot[nf++] = (foot_t)F_KEY("SELECT", "Log");
        draw_footer(foot, nf);
    }

    draw_options_panel();

    // Focus follows status: the host the shelf rests on becomes the selected
    // host, so the existing probe in main.c reports what is running on it.
    if (home_focus != rest_focus) { rest_focus = home_focus; rest_time = 0.0f; }
    else rest_time += ui_dt();
    if (rest_time > 0.4f && kind == IT_HOST && home_focus != ui_get_selected_host_index())
        ui_select_host(home_focus);
}

// ===========================================================================
// Find PC
// ===========================================================================
static float disc_first_f;

static void discovery_input(const ps3_pad_state_t *p) {
    if (discovery_scanned) {
        int rows = discovered_host_count + 1;
        if (p->buttons_pressed & UP_FLAG)   active_host_idx = (active_host_idx + rows - 1) % rows;
        if (p->buttons_pressed & DOWN_FLAG) active_host_idx = (active_host_idx + 1) % rows;
        if (p->buttons_pressed & A_FLAG) {
            if (active_host_idx == discovered_host_count) manual_entry_requested = 1;
            else host_selection_confirmed = 1;
        }
    }
    if (p->buttons_pressed & B_FLAG) {
        ui_reset_host_selection();
        ui_set_state(UI_STATE_IP_ENTRY);
    }
}

static void draw_discovery(void) {
    draw_topbar("Moonlight");
    draw_title("Find a PC");
    if (!discovery_scanned) {
        ui_spinner(ui_centre(), 300.0f, 40.0f, ui_time());
        centre_message("Looking for PCs on your network...", NULL);
    } else {
        int rows = discovered_host_count + 1;
        const float row_h = 76.0f, gap = 14.0f, top = 176.0f;
        float avail = ui_bottom() - 60.0f - top;
        int vis = (int)((avail + gap) / (row_h + gap));
        if (vis < 1) vis = 1;
        static int first = 0;
        first = ui_shelf_first(active_host_idx, first, vis, rows);
        disc_first_f = ui_approach(disc_first_f, (float)first, UI_FOCUS_K);
        float w = ui_right() - ui_left();
        if (w > 760.0f) w = 760.0f;
        if (discovered_host_count == 0)
            ui_text(UI_FACE_BODY, UI_T_BODY, ui_left(), 150.0f, UI_ALIGN_LEFT, UI_TEXT_2,
                    "No PCs found automatically. Make sure Sunshine or Apollo is running.");
        for (int i = 0; i < rows; i++) {
            float y = top + ((float)i - disc_first_f) * (row_h + gap);
            if (y < top - row_h * 0.5f || y + row_h > top + vis * (row_h + gap) + row_h * 0.5f) continue;
            int focus = (i == active_host_idx);
            float x = ui_left();
            ui_glass(x, y, w, row_h, UI_R_ROW, focus ? 1.0f : 0.0f);
            if (focus) ui_glow(x, y, w, row_h, UI_R_ROW, ui_glow_breath());
            if (i == discovered_host_count) {
                ui_icon(UI_ICON_ADD, x + 44.0f, y + row_h * 0.5f, 40.0f, focus ? UI_ACCENT : UI_TEXT_2);
                ui_text(UI_FACE_BODY, UI_T_BODY, x + 90.0f, y + 22.0f, UI_ALIGN_LEFT, UI_TEXT, "Enter IP address...");
            } else {
                ui_icon(UI_ICON_COMPUTER, x + 44.0f, y + row_h * 0.5f, 44.0f, focus ? UI_ACCENT : UI_TEXT_2);
                ui_text_fit(UI_FACE_HEAD, UI_T_CARD, x + 90.0f, y + 12.0f, UI_ALIGN_LEFT, UI_TEXT,
                            w - 90.0f - 220.0f, discovered_hosts[i].name);
                ui_text(UI_FACE_BODY, UI_T_SECOND, x + w - 24.0f, y + 24.0f, UI_ALIGN_RIGHT, UI_TEXT_2,
                        discovered_hosts[i].address);
            }
        }
    }
    foot_t foot[3];
    int nf = 0;
    if (discovery_scanned) foot[nf++] = (foot_t)F_CROSS("Select");
    foot[nf++] = (foot_t)F_CIRCLE("Back");
    foot[nf++] = (foot_t)F_KEY("SELECT", "Log");
    draw_footer(foot, nf);
}

// ===========================================================================
// Pairing, games, error, settings, HUD: ported first, restyled in later phases
// ===========================================================================
static void pairing_input(const ps3_pad_state_t *p) {
    if (p->buttons_pressed & B_FLAG) ui_set_state(UI_STATE_IP_ENTRY);
}

static void draw_pairing(void) {
    draw_topbar("Moonlight");
    if (pairing_pin_str[0]) {
        char l[64];
        snprintf(l, sizeof(l), "PIN  %s", pairing_pin_str);
        centre_message(l, "Open the Sunshine web page, choose PIN, and enter this code.");
    } else {
        centre_message("Connecting...", NULL);
    }
    foot_t foot[2] = { F_CIRCLE("Cancel"), F_KEY("SELECT", "Log") };
    draw_footer(foot, 2);
}

static void applist_input(const ps3_pad_state_t *p) {
    if (current_app_list.count > 0) {
        if (p->buttons_pressed & LEFT_FLAG)
            active_app_idx = (active_app_idx + current_app_list.count - 1) % current_app_list.count;
        if (p->buttons_pressed & RIGHT_FLAG)
            active_app_idx = (active_app_idx + 1) % current_app_list.count;
        if (p->buttons_pressed & A_FLAG) app_selection_confirmed = 1;
    }
    if (p->buttons_pressed & B_FLAG) {
        app_selection_confirmed = 0;
        ui_set_state(UI_STATE_IP_ENTRY);
    }
}

static void draw_applist(void) {
    draw_topbar("Moonlight");
    draw_title("Games");
    if (current_app_list.count > 0)
        ui_text_fit(UI_FACE_HEAD, UI_T_CARD, ui_centre(), 320.0f, UI_ALIGN_CENTER, UI_TEXT, 700.0f,
                    current_app_list.apps[active_app_idx].name);
    foot_t foot[3] = { F_CROSS("Play"), F_CIRCLE("Back"), F_KEY("SELECT", "Log") };
    draw_footer(foot, 3);
}

static void error_input(const ps3_pad_state_t *p) {
    if (p->buttons_pressed & A_FLAG) {
        ui_set_state(UI_STATE_IP_ENTRY);
        ui_error_detail[0] = '\0';
    }
}

static void draw_error(void) {
    draw_topbar("Moonlight");
    centre_message("Couldn't connect", ui_error_detail);
    foot_t foot[2] = { F_CROSS("OK"), F_KEY("SELECT", "Log") };
    draw_footer(foot, 2);
}

static int set_cat, set_row, set_in_pane;

static void settings_input(const ps3_pad_state_t *p) {
    int ncat = ui_settings_cat_count();
    const int *rows;
    int nrows = ui_settings_cat_rows(set_cat, &rows);
    if (!set_in_pane) {
        if (p->buttons_pressed & UP_FLAG)   set_cat = (set_cat + ncat - 1) % ncat;
        if (p->buttons_pressed & DOWN_FLAG) set_cat = (set_cat + 1) % ncat;
        if (p->buttons_pressed & (RIGHT_FLAG | A_FLAG)) { set_in_pane = 1; set_row = 0; }
    } else {
        if (p->buttons_pressed & UP_FLAG)   set_row = (set_row + nrows - 1) % nrows;
        if (p->buttons_pressed & DOWN_FLAG) set_row = (set_row + 1) % nrows;
        if (p->buttons_pressed & LEFT_FLAG) ui_settings_change(rows[set_row], -1);
        if (p->buttons_pressed & (RIGHT_FLAG | A_FLAG)) ui_settings_change(rows[set_row], 1);
    }
    if (p->buttons_pressed & LB_FLAG) { set_cat = (set_cat + ncat - 1) % ncat; set_row = 0; }
    if (p->buttons_pressed & RB_FLAG) { set_cat = (set_cat + 1) % ncat; set_row = 0; }
    if (p->buttons_pressed & B_FLAG) {
        if (set_in_pane) set_in_pane = 0;
        else { ui_save_settings(); ui_set_state(UI_STATE_IP_ENTRY); }
    }
}

static void draw_settings(void) {
    draw_topbar("Moonlight");
    draw_title("Settings");
    const int *rows;
    int nrows = ui_settings_cat_rows(set_cat, &rows);
    for (int i = 0; i < nrows; i++) {
        char v[64];
        ui_settings_value(rows[i], v, sizeof(v));
        float y = 180.0f + i * 44.0f;
        ui_text(UI_FACE_BODY, UI_T_BODY, ui_left(), y, UI_ALIGN_LEFT,
                (set_in_pane && i == set_row) ? UI_ACCENT : UI_TEXT, ui_settings_label(rows[i]));
        ui_text(UI_FACE_BODY, UI_T_BODY, ui_right(), y, UI_ALIGN_RIGHT, UI_TEXT_2, v);
    }
    foot_t foot[3] = { F_CROSS("Change"), F_CIRCLE("Back"), F_KEY("SELECT", "Log") };
    draw_footer(foot, 3);
}

void ui_screens_draw_hud(void) {
    if (!show_stats) return;
    char l[96];
    snprintf(l, sizeof(l), "Rendered %d  Decoded %d  UI %d", ps3video_get_current_fps(),
             ps3video_get_decoded_fps(), ui_fps_actual);
    ui_text(UI_FACE_BODY, UI_T_MIN, ui_left(), ui_top(), UI_ALIGN_LEFT, UI_TEXT, l);
}

// ===========================================================================
// Entry points
// ===========================================================================
static int input_state = -1;

void ui_screens_init(void) {
    shown_state = -1;
    input_state = -1;
    trans_t = 1.0f;
}

void ui_screens_input(const ps3_pad_state_t *pad) {
    int st = ui_get_state();
    if (st != input_state) {
        input_state = st;
        if (st == UI_STATE_IP_ENTRY) home_enter();
        if (st == UI_STATE_SETTINGS) { set_cat = 0; set_row = 0; set_in_pane = 0; }
    }
    if (st == UI_STATE_STREAMING) return;

    // SELECT shows the log on any menu screen.
    if (pad->buttons_pressed & BACK_FLAG) log_open = !log_open;
    if (log_open && (pad->buttons_pressed & B_FLAG)) { log_open = 0; return; }

    switch (st) {
    case UI_STATE_IP_ENTRY:  home_input(pad); break;
    case UI_STATE_DISCOVERY: discovery_input(pad); break;
    case UI_STATE_PAIRING:   pairing_input(pad); break;
    case UI_STATE_APPLIST:   applist_input(pad); break;
    case UI_STATE_ERROR:     error_input(pad); break;
    case UI_STATE_SETTINGS:  settings_input(pad); break;
    default: break;
    }
}

void ui_screens_draw(void) {
    int st = ui_get_state();
    if (st != shown_state) {
        if (shown_state >= 0) trans_dir = (state_depth(st) >= state_depth(shown_state)) ? 1 : -1;
        shown_state = st;
        trans_t = 0.0f;
    }
    trans_t = clampf(trans_t + ui_dt() / UI_SCREEN_FADE_S, 0.0f, 1.0f);
    float e = 1.0f - (1.0f - trans_t) * (1.0f - trans_t);
    ui_set_alpha(e);
    ui_set_offset((1.0f - e) * UI_SCREEN_SLIDE * (float)trans_dir, 0.0f);

    switch (st) {
    case UI_STATE_IP_ENTRY:  draw_home(); break;
    case UI_STATE_DISCOVERY: draw_discovery(); break;
    case UI_STATE_PAIRING:   draw_pairing(); break;
    case UI_STATE_APPLIST:   draw_applist(); break;
    case UI_STATE_ERROR:     draw_error(); break;
    case UI_STATE_SETTINGS:  draw_settings(); break;
    default: break;
    }

    ui_set_alpha(1.0f);
    ui_set_offset(0.0f, 0.0f);
    draw_log_drawer();
}

void ui_screens_draw_overlays(void) {
    ui_set_alpha(1.0f);
    ui_set_offset(0.0f, 0.0f);
    draw_toast();
}
