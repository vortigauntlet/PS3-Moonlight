#include "ui_bg.h"
#include "ui_draw.h"
#include "ui_layout.h"
#include <tiny3d.h>
#include <math.h>

#define PI_F 3.14159265f

// ---- sky -------------------------------------------------------------------

static void sky(const ui_theme_t *t) {
    float w = ui_lay.lw;
    ui_rect(0.0f, 0.0f, w, UI_LH * 0.5f, t->bg_top, t->bg_mid);
    ui_rect(0.0f, UI_LH * 0.5f, w, UI_LH * 0.5f, t->bg_mid, t->bg_bot);

    // A wide, soft lift at the upper centre.
    ui_col_t c = { 1.0f, 1.0f, 1.0f, 0.06f };
    ui_col_t e = { 1.0f, 1.0f, 1.0f, 0.0f };
    const int seg = 24;
    float cx = w * 0.5f, cy = 150.0f, rx = w * 0.55f, ry = 260.0f;
    tiny3d_SetPolygon(TINY3D_TRIANGLE_FAN);
    ui_vertex(cx, cy, c);
    for (int i = 0; i <= seg; i++) {
        float a = 2.0f * PI_F * (float)i / (float)seg;
        ui_vertex(cx + rx * cosf(a), cy + ry * sinf(a), e);
    }
    tiny3d_End();
}

// ---- the moon ----------------------------------------------------------------

static float moon_x(void) { return ui_lay.lw * 0.85f + 10.0f * sinf(ui_time() * 0.05f); }
static float moon_y(void) { return 150.0f + 4.0f * sinf(ui_time() * 0.07f); }
static float moon_breath(void) { return 0.92f + 0.08f * sinf(ui_time() * 0.45f); }

// The glow the moon throws over the sky: three nested soft discs.
static void moon_halo(const ui_theme_t *t) {
    float mx = moon_x(), my = moon_y(), k = t->moon_a * moon_breath();
    ui_blend_set(UI_BLEND_ADD);
    static const float radius[3] = { 1100.0f, 600.0f, 260.0f };
    static const float alpha[3]  = { 0.13f, 0.12f, 0.16f };
    for (int i = 0; i < 3; i++) {
        ui_col_t c = { 0.72f, 0.84f, 1.0f, alpha[i] * k };
        ui_col_t e = { 0.72f, 0.84f, 1.0f, 0.0f };
        ui_circle(mx, my, radius[i], c, e);
    }
    ui_blend_set(UI_BLEND_NORMAL);
}

static void moon_disc(const ui_theme_t *t) {
    float mx = moon_x(), my = moon_y(), r = 66.0f, k = t->moon_a;
    ui_col_t bright = { 0.86f, 0.91f, 0.99f, 0.70f * k + 0.2f };
    ui_col_t edge = { 0.58f, 0.68f, 0.86f, 0.70f * k + 0.2f };
    ui_circle(mx, my, r, bright, edge);
    // The maria: a few darker, softer patches.
    static const float m[6][3] = { { -0.30f, -0.22f, 0.30f }, { 0.22f, -0.36f, 0.20f }, { 0.10f, 0.12f, 0.34f },
                                   { -0.38f, 0.30f, 0.16f }, { 0.42f, 0.18f, 0.14f }, { -0.05f, 0.50f, 0.12f } };
    for (int i = 0; i < 6; i++) {
        ui_col_t c = { 0.38f, 0.47f, 0.66f, 0.42f * k };
        ui_col_t e = { 0.38f, 0.47f, 0.66f, 0.0f };
        ui_circle(mx + m[i][0] * r, my + m[i][1] * r, m[i][2] * r, c, e);
    }
    // A soft bloom round the disc, additive, with no visible edge.
    ui_blend_set(UI_BLEND_ADD);
    ui_col_t bc = { 0.8f, 0.9f, 1.0f, 0.16f * k }, be = { 0.8f, 0.9f, 1.0f, 0.0f };
    ui_circle(mx, my, r * 1.9f, bc, be);
    ui_blend_set(UI_BLEND_NORMAL);
}

// ---- wave ------------------------------------------------------------------

#define WAVE_SEG 64

static float wave_y(float x, float base, float a1, float p1, float a2, float p2, float k) {
    float time = ui_time();
    float u = x / ui_lay.lw;
    return base + a1 * sinf(2.0f * PI_F * (k * u + time / p1))
                + a2 * sinf(2.0f * PI_F * (2.3f * k * u + 0.37f) - 2.0f * PI_F * time / p2);
}

static void ribbon(const ui_theme_t *t, float base, float a1, float p1, float a2, float p2,
                   float k, float half, float tint) {
    float w = ui_lay.lw;
    ui_col_t mid = { 1.0f - 0.45f * tint, 1.0f - 0.10f * tint, 1.0f, t->wave_a };
    ui_col_t edge = mid;
    edge.a = 0.0f;
    for (int side = 0; side < 2; side++) {
        tiny3d_SetPolygon(TINY3D_TRIANGLE_STRIP);
        for (int i = 0; i <= WAVE_SEG; i++) {
            float x = w * (float)i / (float)WAVE_SEG;
            float y = wave_y(x, base, a1, p1, a2, p2, k);
            float taper = sinf(PI_F * (float)i / (float)WAVE_SEG);   // thin at the ends
            float hh = half * (0.35f + 0.65f * taper) * (side ? 1.0f : 1.0f);
            ui_vertex(x, y, ui_col_mul_a(mid, 0.4f + 0.6f * taper));
            ui_vertex(x, y + (side ? hh : -hh), edge);
        }
        tiny3d_End();
    }
}

static void wave(const ui_theme_t *t) {
    ui_blend_set(UI_BLEND_ADD);
    ribbon(t, 565.0f, 26.0f, 23.0f, 12.0f, 31.0f, 1.0f, 70.0f, 0.6f);
    ribbon(t, 600.0f, 20.0f, 29.0f, 10.0f, 21.0f, 1.4f, 58.0f, 1.0f);
    ui_blend_set(UI_BLEND_NORMAL);
}

void ui_bg_draw(void) {
    const ui_theme_t *t = ui_theme();
    sky(t);
    moon_halo(t);
    moon_disc(t);
    wave(t);
}

// A last faint wash of moonlight over the finished screen, so the glass and
// text pick it up too.  Additive, so it can only brighten.
void ui_bg_draw_light(void) {
    const ui_theme_t *t = ui_theme();
    float mx = moon_x(), my = moon_y();
    ui_blend_set(UI_BLEND_ADD);
    ui_col_t c = { 0.70f, 0.82f, 1.0f, 0.055f * t->moon_a * moon_breath() };
    ui_col_t e = { 0.70f, 0.82f, 1.0f, 0.0f };
    ui_circle(mx, my, 1100.0f, c, e);
    ui_blend_set(UI_BLEND_NORMAL);
}
