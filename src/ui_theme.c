#include "ui_theme.h"
#include <time.h>

static const ui_theme_t day_theme = {
    UI_HEX(0x0A2E52), UI_HEX(0x11598A), UI_HEX(0x1C8FB5),
    UI_HEX(0x06304F), 0.22f,       // glass tint, alpha
    0.10f, 0.18f,                  // film: rest, focused
    0.16f,                         // gloss
    0.05f,                         // sheen
    0.22f,                         // wave
    0.60f,                         // moon
    0.04f,                         // light shafts
    0
};

// Dark Aero: the glass is nearly black and carries a cold blue edge; the
// shine does the work of showing it is glass at all.
static const ui_theme_t night_theme = {
    UI_HEX(0x060E22), UI_HEX(0x0D2748), UI_HEX(0x134A70),
    UI_HEX(0x020814), 0.58f,
    0.07f, 0.15f,
    0.14f,
    0.10f,
    0.20f,
    0.90f,
    0.20f,
    1
};

static int mode = UI_THEME_AUTO;
static int night_now = 0;

int ui_theme_get_mode(void) { return mode; }

void ui_theme_set_mode(int m) {
    mode = (m == UI_THEME_DAY || m == UI_THEME_NIGHT) ? m : UI_THEME_AUTO;
    ui_theme_update();
}

// Day is 07:00-19:00 by the console clock.
void ui_theme_update(void) {
    if (mode == UI_THEME_DAY) { night_now = 0; return; }
    if (mode == UI_THEME_NIGHT) { night_now = 1; return; }
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    int hour = tm ? tm->tm_hour : 20;
    night_now = !(hour >= 7 && hour < 19);
}

const ui_theme_t *ui_theme(void) {
    return night_now ? &night_theme : &day_theme;
}
