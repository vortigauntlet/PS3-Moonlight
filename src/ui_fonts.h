#ifndef UI_FONTS_H
#define UI_FONTS_H

#include <stddef.h>
#include "ui_theme.h"

// Faces.  Each is one libfont font; text is laid out with our own advance
// tables because libfont has no string-width function.
enum {
    UI_FACE_BODY = 0,   // Rodin from the console (or the 8x8 fallback)
    UI_FACE_HEAD,       // Open Sans Bold: titles, card titles, wordmark
    UI_FACE_NUM,        // Michroma: stat values (upper case only)
    UI_FACE_DISPLAY,    // Michroma digits + Open Sans capitals, very large
    UI_FACE_ICON,       // Material Icons subset
    UI_FACE_COUNT
};

enum { UI_ALIGN_LEFT = 0, UI_ALIGN_CENTER, UI_ALIGN_RIGHT };

// Icons, by the ASCII letter the icon face maps to a Material codepoint.
enum {
    UI_ICON_COMPUTER = 'a', UI_ICON_ADD, UI_ICON_LOCK, UI_ICON_WIFI_OFF,
    UI_ICON_PLAY, UI_ICON_STOP, UI_ICON_SETTINGS, UI_ICON_CHECK,
    UI_ICON_ERROR, UI_ICON_INFO, UI_ICON_REFRESH, UI_ICON_DELETE
};

// PlayStation button glyphs, chars of the body face.
#define UI_G_CROSS    "\x01"
#define UI_G_CIRCLE   "\x02"
#define UI_G_TRIANGLE "\x03"
#define UI_G_SQUARE   "\x04"
#define UI_G_DPAD     "\x05"

// Build every face into one texture block.  Must run on the UI thread after
// tiny3d_Init and after ui_layout_init.  Falls back per face, never fails.
void ui_fonts_init(void);
void ui_fonts_shutdown(void);

// Width in logical px of `str` at logical size `size`.
float ui_text_width(int face, float size, const char *str);

// Draw.  `y` is the top of the line box (the em box of `size`); `x` is the
// anchor named by `align`.  Returns the drawn width in logical px.
float ui_text(int face, float size, float x, float y, int align,
              ui_col_t color, const char *str);
// The same, with the soft drop shadow used on bright areas.
float ui_text_shadow(int face, float size, float x, float y, int align,
                     ui_col_t color, const char *str);
// Ellipsized to `max_w` logical px.  Every host and app name goes through here.
float ui_text_fit(int face, float size, float x, float y, int align,
                  ui_col_t color, float max_w, const char *str);

// The ellipsized copy of `in` that fits `max_w`, without drawing it.
void ui_fit_string(int face, float size, float max_w, const char *in, char *out, size_t n);

// Greedy word wrap of `str` into at most `max_lines` lines of `max_w`;
// returns the number of lines drawn, each `line_h` apart.
int ui_text_wrap(int face, float size, float x, float y, int align,
                 ui_col_t color, float max_w, float line_h, int max_lines,
                 const char *str);

// One icon, centred on (cx, cy), `size` logical px square.
void ui_icon(int icon, float cx, float cy, float size, ui_col_t color);

#endif
