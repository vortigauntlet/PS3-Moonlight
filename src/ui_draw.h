#ifndef UI_DRAW_H
#define UI_DRAW_H

// 2D primitives for the menus.  tiny3d has no custom shaders here, so every
// shape is coloured triangles.  All coordinates are logical (ui_layout.h).

#include "ui_theme.h"

// ---- time ----
// Call once per frame before drawing.  dt is clamped, so a stall (OSK, a
// system dialog) does not make animations jump.
void  ui_draw_frame_begin(void);
float ui_dt(void);
float ui_time(void);                       // seconds since the first frame
// Frame-rate independent easing: identical on 50 and 60 Hz outputs.
float ui_approach(float v, float target, float k);

// ---- whole-screen transition state ----
// Everything drawn after these calls (shapes, text, images) is shifted by
// (dx, dy) logical px and has its alpha multiplied.  Used for screen changes.
void  ui_set_alpha(float a);
void  ui_set_offset(float dx, float dy);
float ui_get_alpha(void);
float ui_get_offset_x(void);
float ui_get_offset_y(void);

// ---- blend state ----
// tiny3d cannot report its blend state, so it is tracked here.  Text, glyphs
// and the video quad all need NORMAL; ADD is only ever held inside a primitive.
enum { UI_BLEND_NORMAL = 0, UI_BLEND_ADD };
void ui_blend_set(int mode);
int  ui_blend_get(void);

// One vertex of a polygon the caller has opened with tiny3d_SetPolygon, in
// logical coordinates.
void ui_vertex(float x, float y, ui_col_t c);

// ---- shapes ----
void ui_rect(float x, float y, float w, float h, ui_col_t top, ui_col_t bot);
void ui_rrect(float x, float y, float w, float h, float r, ui_col_t top, ui_col_t bot);
// A band of `stroke` logical px just inside a rounded rect.
void ui_ring(float x, float y, float w, float h, float r, float stroke,
             ui_col_t top, ui_col_t bot);
void ui_circle(float cx, float cy, float r, ui_col_t center, ui_col_t edge);
// Textured quad corners may be arbitrary; used by the moon mark and light rays.
void ui_quad(const float xy[8], const ui_col_t col[4]);

// The one card style in the app.  `focus` is 0..1.
void ui_glass(float x, float y, float w, float h, float r, float focus);
// The focus halo, additive.  `a` is the peak alpha; pass ui_glow_breath().
void ui_glow(float x, float y, float w, float h, float r, float a);
float ui_glow_breath(void);                // 0.55..0.85 on a 2 s sine

void ui_spinner(float cx, float cy, float r, float t);
// A status pill: dot plus label.  Returns its width.  `y` is the top.
float ui_pill_width(const char *label);
float ui_pill(float x, float y, const char *label, ui_col_t color);
// The crescent-moon mark.
void ui_moon(float cx, float cy, float r);

#endif
