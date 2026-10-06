#ifndef UI_THEME_H
#define UI_THEME_H

// Palette, type scale and timings.  Two themes share one set of text and
// status colours (white text must read on both), and differ in the sky and in
// how dark the glass is:
//   Day   -- light Frutiger Aero: an aqua sky seen through clear glass.
//   Night -- dark Aero (Vista-era): near-black blue glass, glowing cyan
//            edges, light shafts and bokeh over a deep sky.

typedef struct { float r, g, b, a; } ui_col_t;

#define UI_HEX(h) { (float)(((h) >> 16) & 255) / 255.0f, \
                    (float)(((h) >> 8) & 255) / 255.0f,  \
                    (float)((h) & 255) / 255.0f, 1.0f }

static inline ui_col_t ui_col_a(ui_col_t c, float a) { c.a = a; return c; }
static inline ui_col_t ui_col_mul_a(ui_col_t c, float a) { c.a *= a; return c; }
static inline ui_col_t ui_col_mix(ui_col_t a, ui_col_t b, float t) {
    ui_col_t c = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                   a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
    return c;
}

static const ui_col_t UI_TEXT   = UI_HEX(0xFFFFFF);
static const ui_col_t UI_TEXT_2 = UI_HEX(0xCFE6F5);
static const ui_col_t UI_TEXT_3 = UI_HEX(0x8FB6CF);
static const ui_col_t UI_ACCENT = UI_HEX(0x5FE0FF);
static const ui_col_t UI_OK     = UI_HEX(0x6BE07A);
static const ui_col_t UI_WARN   = UI_HEX(0xFFC44D);
static const ui_col_t UI_BAD    = UI_HEX(0xFF7A7A);

// Classic PlayStation face-button colours for the footer glyphs.
static const ui_col_t UI_PS_CROSS    = UI_HEX(0x7DA7FF);
static const ui_col_t UI_PS_CIRCLE   = UI_HEX(0xFF6B6B);
static const ui_col_t UI_PS_TRIANGLE = UI_HEX(0x3FD69B);
static const ui_col_t UI_PS_SQUARE   = UI_HEX(0xF28BD4);

typedef struct {
    ui_col_t bg_top, bg_mid, bg_bot;
    ui_col_t glass_tint;     // dark layer under the white film (Dark Aero)
    float    glass_tint_a;
    float    glass_a;        // white film
    float    glass_a_focus;
    float    gloss_a;        // top-half shine
    float    sheen_a;        // diagonal streak
    float    wave_a;
    float    bubble_a;
    float    rays_a;         // light shafts
    int      night;
} ui_theme_t;

enum { UI_THEME_AUTO = 0, UI_THEME_DAY = 1, UI_THEME_NIGHT = 2 };

const ui_theme_t *ui_theme(void);
void ui_theme_set_mode(int mode);          // UI_THEME_*
int  ui_theme_get_mode(void);
void ui_theme_update(void);                // re-evaluate Auto against the clock

// Type scale, logical px.
#define UI_T_TITLE    40.0f
#define UI_T_CARD     28.0f
#define UI_T_BODY     24.0f
#define UI_T_SECOND   22.0f
#define UI_T_HINT     22.0f
#define UI_T_MIN      20.0f
#define UI_T_PIN      72.0f
#define UI_T_SPEC     22.0f

// Strokes: visible lines are at least this thick (2 px twitters on 480i and
// 1080i).
#define UI_STROKE     3.0f

// Radii.
#define UI_R_CARD     16.0f
#define UI_R_ROW      12.0f

// Timings.
#define UI_FOCUS_K        20.0f    // approach() rate: ~150 ms to settle
#define UI_SCREEN_FADE_S  0.18f
#define UI_SCREEN_SLIDE   24.0f
#define UI_TOAST_IN_S     0.20f
#define UI_TOAST_HOLD_S   2.5f
#define UI_TOAST_OUT_S    0.40f
#define UI_FOCUS_SCALE    1.08f
#define UI_FOCUS_LIFT     8.0f

#endif
