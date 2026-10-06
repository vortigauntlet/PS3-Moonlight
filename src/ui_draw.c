#include "ui_draw.h"
#include "ui_layout.h"
#include "ui_fonts.h"
#include <tiny3d.h>
#include <math.h>
#include <string.h>
#include <lv2/systime.h>

#define Z 65535.0f
#define CORNER_SEG 6
#define PTS_PER_RRECT (4 * (CORNER_SEG + 1))
#define PI_F 3.14159265f

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
static u64   t_first, t_prev;
static float g_dt = 1.0f / 60.0f, g_time;

void ui_draw_frame_begin(void) {
    u64 now = sysGetSystemTime();            // microseconds
    if (!t_first) { t_first = now; t_prev = now; }
    g_dt = (float)(now - t_prev) * 1e-6f;
    if (g_dt > 0.1f) g_dt = 0.1f;
    if (g_dt < 0.0f) g_dt = 0.0f;
    g_time = (float)(now - t_first) * 1e-6f;
    t_prev = now;
}
float ui_dt(void)   { return g_dt; }
float ui_time(void) { return g_time; }

float ui_approach(float v, float target, float k) {
    return v + (target - v) * (1.0f - expf(-k * g_dt));
}

float ui_glow_breath(void) {
    return 0.70f + 0.15f * sinf(g_time * PI_F);   // period 2 s
}

// ---------------------------------------------------------------------------
// Blend
// ---------------------------------------------------------------------------
static int g_blend = UI_BLEND_NORMAL;

void ui_blend_set(int mode) {
    if (mode == UI_BLEND_ADD) {
        tiny3d_BlendFunc(1,
            TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA | TINY3D_BLEND_FUNC_SRC_ALPHA_ZERO,
            TINY3D_BLEND_FUNC_DST_RGB_ONE | TINY3D_BLEND_FUNC_DST_ALPHA_ONE,
            TINY3D_BLEND_RGB_FUNC_ADD | TINY3D_BLEND_ALPHA_FUNC_ADD);
    } else {
        tiny3d_BlendFunc(1,
            TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA | TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA,
            TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA | TINY3D_BLEND_FUNC_DST_ALPHA_ONE_MINUS_SRC_ALPHA,
            TINY3D_BLEND_RGB_FUNC_ADD | TINY3D_BLEND_ALPHA_FUNC_ADD);
    }
    g_blend = mode;
}
int ui_blend_get(void) { return g_blend; }

// ---------------------------------------------------------------------------
// Vertex helpers
// ---------------------------------------------------------------------------
static float g_alpha = 1.0f, g_ox, g_oy;
void  ui_set_alpha(float a) { g_alpha = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); }
void  ui_set_offset(float dx, float dy) { g_ox = dx; g_oy = dy; }
float ui_get_alpha(void) { return g_alpha; }
float ui_get_offset_x(void) { return g_ox; }
float ui_get_offset_y(void) { return g_oy; }

static void vtx(float x, float y, ui_col_t c) {
    tiny3d_VertexPos(lx(x + g_ox), ly(y + g_oy), Z);
    tiny3d_VertexFcolor(c.r, c.g, c.b, c.a * g_alpha);
}

void ui_vertex(float x, float y, ui_col_t c) { vtx(x, y, c); }

static ui_col_t grad(ui_col_t top, ui_col_t bot, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return ui_col_mix(top, bot, t);
}

void ui_rect(float x, float y, float w, float h, ui_col_t top, ui_col_t bot) {
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    vtx(x, y, top);
    vtx(x + w, y, top);
    vtx(x, y + h, bot);
    vtx(x + w, y + h, bot);
    tiny3d_End();
}

void ui_quad(const float xy[8], const ui_col_t col[4]) {
    // Corners in order 0 1 3 2 make a strip from a convex quad given 0 1 2 3
    // clockwise.
    static const int order[4] = { 0, 1, 3, 2 };
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    for (int i = 0; i < 4; i++) vtx(xy[order[i] * 2], xy[order[i] * 2 + 1], col[order[i]]);
    tiny3d_End();
}

// Outline of a rounded rect, clockwise from the start of the top-left arc.
// `inset` shrinks the radius and the box (used for the inner edge of a ring).
static int rr_points(float x, float y, float w, float h, float r, float *px, float *py) {
    float m = (w < h ? w : h) * 0.5f;
    if (r > m) r = m;
    if (r < 0.0f) r = 0.0f;
    static const float a0[4] = { PI_F, 1.5f * PI_F, 0.0f, 0.5f * PI_F };
    const float cxs[4] = { x + r, x + w - r, x + w - r, x + r };
    const float cys[4] = { y + r, y + r, y + h - r, y + h - r };
    int n = 0;
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i <= CORNER_SEG; i++) {
            float a = a0[c] + (0.5f * PI_F) * (float)i / (float)CORNER_SEG;
            px[n] = cxs[c] + r * cosf(a);
            py[n] = cys[c] + r * sinf(a);
            n++;
        }
    }
    return n;
}

void ui_rrect(float x, float y, float w, float h, float r, ui_col_t top, ui_col_t bot) {
    float px[PTS_PER_RRECT], py[PTS_PER_RRECT];
    int n = rr_points(x, y, w, h, r, px, py);
    tiny3d_SetPolygon(TINY3D_TRIANGLE_FAN);
    vtx(x + w * 0.5f, y + h * 0.5f, grad(top, bot, 0.5f));
    for (int i = 0; i < n; i++) vtx(px[i], py[i], grad(top, bot, (py[i] - y) / h));
    vtx(px[0], py[0], grad(top, bot, (py[0] - y) / h));
    tiny3d_End();
}

void ui_ring(float x, float y, float w, float h, float r, float stroke,
             ui_col_t top, ui_col_t bot) {
    float ox[PTS_PER_RRECT], oy[PTS_PER_RRECT], ix[PTS_PER_RRECT], iy[PTS_PER_RRECT];
    int n = rr_points(x, y, w, h, r, ox, oy);
    rr_points(x + stroke, y + stroke, w - 2.0f * stroke, h - 2.0f * stroke,
              r - stroke, ix, iy);
    tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
    for (int i = 0; i <= n; i++) {
        int k = i % n;
        vtx(ox[k], oy[k], grad(top, bot, (oy[k] - y) / h));
        vtx(ix[k], iy[k], grad(top, bot, (iy[k] - y) / h));
    }
    tiny3d_End();
}

void ui_circle(float cx, float cy, float r, ui_col_t center, ui_col_t edge) {
    const int seg = 24;
    tiny3d_SetPolygon(TINY3D_TRIANGLE_FAN);
    vtx(cx, cy, center);
    for (int i = 0; i <= seg; i++) {
        float a = 2.0f * PI_F * (float)i / (float)seg;
        vtx(cx + r * cosf(a), cy + r * sinf(a), edge);
    }
    tiny3d_End();
}

// ---------------------------------------------------------------------------
// Glass
// ---------------------------------------------------------------------------
void ui_glass(float x, float y, float w, float h, float r, float focus) {
    const ui_theme_t *t = ui_theme();
    if (focus < 0.0f) focus = 0.0f;
    if (focus > 1.0f) focus = 1.0f;
    ui_col_t black = { 0.0f, 0.0f, 0.0f, 0.0f };

    // Shadow: three expanding rounded rects.
    static const float sh_a[3] = { 0.10f, 0.06f, 0.03f };
    for (int i = 2; i >= 0; i--) {
        float e = 5.0f * (float)(i + 1);
        ui_rrect(x - e, y - e + 7.0f, w + 2.0f * e, h + 2.0f * e, r + e,
                 ui_col_a(black, sh_a[i]), ui_col_a(black, sh_a[i]));
    }

    // Dark glass body (the Dark Aero part), then the white film over it.
    ui_rrect(x, y, w, h, r, ui_col_a(t->glass_tint, t->glass_tint_a * 0.85f),
             ui_col_a(t->glass_tint, t->glass_tint_a * 1.10f));
    float film = t->glass_a + (t->glass_a_focus - t->glass_a) * focus;
    ui_col_t white = { 1.0f, 1.0f, 1.0f, 1.0f };
    ui_rrect(x, y, w, h, r, ui_col_a(white, film + 0.04f), ui_col_a(white, film - 0.03f));

    // Aero shine: the top half, fading out.
    float gh = h * 0.5f;
    ui_rrect(x + 2.0f, y + 2.0f, w - 4.0f, gh, (r < gh * 0.5f ? r : gh * 0.5f),
             ui_col_a(white, t->gloss_a), ui_col_a(white, 0.0f));

    // Diagonal sheen: a soft wedge of light across the upper left, as on Vista
    // glass.  Stays inside the inset box so it never meets a rounded corner.
    if (t->sheen_a > 0.0f) {
        float x0 = x + r, x1 = x + w - r, y0 = y + 4.0f, y1 = y + h * 0.62f;
        float xy[8] = { x0, y0, x0 + (x1 - x0) * 0.55f, y0,
                        x0 + (x1 - x0) * 0.20f, y1, x0, y1 };
        ui_col_t c[4] = { ui_col_a(white, t->sheen_a), ui_col_a(white, t->sheen_a * 0.6f),
                          ui_col_a(white, 0.0f), ui_col_a(white, 0.0f) };
        ui_quad(xy, c);
    }

    // Rim: bright at the top, nearly gone at the bottom; cyan when focused.
    ui_col_t rim_top = ui_col_mix(ui_col_a(white, 0.35f), ui_col_a(UI_ACCENT, 0.95f), focus);
    ui_col_t rim_bot = ui_col_mix(ui_col_a(white, 0.06f), ui_col_a(UI_ACCENT, 0.45f), focus);
    ui_ring(x, y, w, h, r, UI_STROKE, rim_top, rim_bot);
}

void ui_glow(float x, float y, float w, float h, float r, float a) {
    int prev = ui_blend_get();
    ui_blend_set(UI_BLEND_ADD);
    const int rings = 4;
    const float band = 3.0f;
    for (int i = 0; i < rings; i++) {
        float f = 1.0f - (float)i / (float)rings;
        float e = band * (float)i;
        ui_col_t c = ui_col_a(UI_ACCENT, a * f * f * 0.55f);
        ui_ring(x - e - band, y - e - band, w + 2.0f * (e + band), h + 2.0f * (e + band),
                r + e + band, band, c, c);
    }
    ui_blend_set(prev);
}

// ---------------------------------------------------------------------------
// Small things
// ---------------------------------------------------------------------------
void ui_spinner(float cx, float cy, float r, float t) {
    const int dots = 8;
    float dr = r * 0.17f;
    float head = t * (float)dots;           // one revolution a second
    for (int i = 0; i < dots; i++) {
        float a = 2.0f * PI_F * (float)i / (float)dots - 0.5f * PI_F;
        float age = fmodf(head - (float)i + (float)dots * 4.0f, (float)dots) / (float)dots;
        float alpha = 1.0f - age;
        alpha = 0.12f + 0.88f * alpha * alpha;
        ui_col_t c = ui_col_a(UI_ACCENT, alpha);
        ui_circle(cx + r * cosf(a), cy + r * sinf(a), dr * (0.7f + 0.3f * alpha), c, c);
    }
}

#define PILL_H   32.0f
#define PILL_PAD 14.0f
#define PILL_DOT 5.0f

float ui_pill_width(const char *label) {
    return PILL_PAD + 2.0f * PILL_DOT + 8.0f + ui_text_width(UI_FACE_BODY, UI_T_MIN, label) + PILL_PAD;
}

float ui_pill(float x, float y, const char *label, ui_col_t color) {
    float w = ui_pill_width(label);
    ui_col_t dark = { 0.0f, 0.04f, 0.10f, 0.45f };
    ui_rrect(x, y, w, PILL_H, PILL_H * 0.5f, dark, dark);
    ui_ring(x, y, w, PILL_H, PILL_H * 0.5f, UI_STROKE, ui_col_a(color, 0.55f), ui_col_a(color, 0.20f));
    ui_circle(x + PILL_PAD + PILL_DOT, y + PILL_H * 0.5f, PILL_DOT, color, ui_col_a(color, 0.85f));
    ui_text(UI_FACE_BODY, UI_T_MIN, x + PILL_PAD + 2.0f * PILL_DOT + 8.0f, y + 4.0f,
            UI_ALIGN_LEFT, UI_TEXT, label);
    return w;
}

void ui_logo(float cx, float cy, float r) {
    // The Moonlight mark: a grey ring round a white disc crossed by four
    // spokes (geometry from the project's moonlight.svg, 256 units across).
    ui_col_t grey = UI_HEX(0x565C64);
    ui_col_t white = { 1.0f, 1.0f, 1.0f, 1.0f };
    ui_circle(cx, cy, r, grey, grey);
    ui_circle(cx, cy, r * 0.75f, white, white);
    float half = r * 0.75f * 0.99f;
    float w = r * 0.0625f * 2.0f;
    if (w < UI_STROKE) w = UI_STROKE;        // a visible stroke is never thinner than this
    for (int k = 0; k < 4; k++) {
        float a = PI_F * (float)k / 4.0f;
        float dx = cosf(a), dy = sinf(a);
        float px = -dy * w * 0.5f, py = dx * w * 0.5f;
        float xy[8] = { cx - dx * half + px, cy - dy * half + py, cx + dx * half + px, cy + dy * half + py,
                        cx + dx * half - px, cy + dy * half - py, cx - dx * half - px, cy - dy * half - py };
        ui_col_t c[4] = { grey, grey, grey, grey };
        ui_quad(xy, c);
    }
    ui_circle(cx, cy, w * 0.9f, grey, grey);
}
