#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

// The logical canvas every menu is laid out on.  Height is always 720; width
// is 1280 on a 16:9 output and 960 on a 4:3 one.  Mapping to the physical
// output scales x and y separately, which is what keeps circles round on an
// anamorphic mode (720x480 at 16:9): the TV stretches the pixels back.
//
// Nothing outside this header and ui_layout.c may assume a width of 1280.

#include <stddef.h>

#define UI_LH          720.0f
#define UI_SAFE_X      64.0f   // left and right margin
#define UI_SAFE_TOP    40.0f
#define UI_SAFE_BOT    40.0f

typedef struct {
    int   pw, ph;   // physical output size
    float lw;       // logical width: 1280 or 960
} ui_layout_t;

extern ui_layout_t ui_lay;

typedef struct { float x, y, w, h; } ui_rect_t;

void ui_layout_init(int pw, int ph, int is_4x3);

static inline float lx(float x) { return x * (float)ui_lay.pw / ui_lay.lw; }
static inline float ly(float y) { return y * (float)ui_lay.ph / UI_LH; }

static inline float ui_left(void)   { return UI_SAFE_X; }
static inline float ui_right(void)  { return ui_lay.lw - UI_SAFE_X; }
static inline float ui_centre(void) { return ui_lay.lw * 0.5f; }
static inline float ui_top(void)    { return UI_SAFE_TOP; }
static inline float ui_bottom(void) { return UI_LH - UI_SAFE_BOT; }

static inline ui_rect_t ui_safe_rect(void) {
    ui_rect_t r = { UI_SAFE_X, UI_SAFE_TOP, ui_lay.lw - 2.0f * UI_SAFE_X,
                    UI_LH - UI_SAFE_TOP - UI_SAFE_BOT };
    return r;
}

// ---- Pure layout helpers (no console dependency; see tests/test_layout.c) ----

// How many whole cards of width `card_w` with `gap` between them fit in a row
// `avail` wide.  Never less than 1.
int   ui_shelf_fit(float avail, float card_w, float gap);

// First visible card index that keeps `focus` inside a window of `fit` cards,
// moving the window as little as possible from `first`.
int   ui_shelf_first(int focus, int first, int fit, int count);

// Left edge, in logical px, of card `i` when the row starts at `left` and the
// first visible card is the (possibly fractional) `first`.
float ui_shelf_card_x(float left, float card_w, float gap, int i, float first);

// Ellipsize `s` to at most `max_w` using `adv` (width of one byte, in logical
// px).  Writes at most `n` bytes to `out` and returns the resulting width.
typedef float (*ui_adv_fn)(unsigned char c, void *ctx);
float ui_fit_text(const char *s, float max_w, ui_adv_fn adv, void *ctx,
                  char *out, size_t n);

#endif
