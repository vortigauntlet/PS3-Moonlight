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

// ---- light shafts (Dark Aero) ---------------------------------------------

static void rays(const ui_theme_t *t) {
    if (t->rays_a <= 0.0f) return;
    ui_blend_set(UI_BLEND_ADD);
    float w = ui_lay.lw;
    float time = ui_time();
    for (int i = 0; i < 3; i++) {
        float phase = time * (0.05f + 0.02f * (float)i) + (float)i * 2.1f;
        float a = t->rays_a * (0.65f + 0.35f * sinf(phase));
        float x0 = w * (0.08f + 0.30f * (float)i) + 30.0f * sinf(phase * 0.7f);
        float spread = 70.0f + 25.0f * (float)i;
        float drift = 330.0f;
        ui_col_t top = { 0.55f, 0.85f, 1.0f, a };
        ui_col_t bot = { 0.55f, 0.85f, 1.0f, 0.0f };
        float xy[8] = { x0, 0.0f, x0 + spread, 0.0f, x0 + spread + drift, UI_LH * 0.85f, x0 + drift, UI_LH * 0.85f };
        ui_col_t col[4] = { top, top, bot, bot };
        ui_quad(xy, col);
    }
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

// ---- bubbles ---------------------------------------------------------------

typedef struct { float x, y0, r, speed, sway, phase; } bubble_t;
static const bubble_t bubbles[10] = {
    { 0.08f, 0.92f, 34.0f,  8.0f, 14.0f, 0.3f },
    { 0.17f, 0.31f, 14.0f, 12.0f,  9.0f, 1.7f },
    { 0.29f, 0.74f, 52.0f,  6.0f, 18.0f, 2.9f },
    { 0.41f, 0.12f, 20.0f, 10.0f, 10.0f, 4.1f },
    { 0.52f, 0.58f, 12.0f, 14.0f,  8.0f, 0.9f },
    { 0.63f, 0.88f, 60.0f,  6.5f, 20.0f, 5.2f },
    { 0.72f, 0.40f, 26.0f,  9.0f, 12.0f, 3.3f },
    { 0.83f, 0.66f, 16.0f, 13.0f, 10.0f, 2.2f },
    { 0.91f, 0.20f, 40.0f,  7.0f, 16.0f, 4.8f },
    { 0.96f, 0.80f, 22.0f, 11.0f,  9.0f, 1.1f },
};

static void bubble_field(const ui_theme_t *t) {
    float time = ui_time();
    for (int i = 0; i < 10; i++) {
        const bubble_t *b = &bubbles[i];
        float span = UI_LH + 2.0f * b->r;
        float y = fmodf(b->y0 * span - b->speed * time + span * 8.0f, span) - b->r;
        float x = b->x * ui_lay.lw + b->sway * sinf(time * 0.35f + b->phase);
        ui_col_t centre = { 0.85f, 0.95f, 1.0f, 0.03f };
        ui_col_t rim = { 0.85f, 0.95f, 1.0f, t->bubble_a };
        ui_circle(x, y, b->r, centre, rim);
        ui_col_t spec = { 1.0f, 1.0f, 1.0f, 0.35f };
        ui_col_t spec0 = { 1.0f, 1.0f, 1.0f, 0.0f };
        ui_circle(x - b->r * 0.42f, y - b->r * 0.42f, b->r * 0.14f + 1.5f, spec, spec0);
    }
}

void ui_bg_draw(void) {
    const ui_theme_t *t = ui_theme();
    sky(t);
    rays(t);
    wave(t);
    bubble_field(t);
}
