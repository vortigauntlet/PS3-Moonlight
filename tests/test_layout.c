// Host-side checks for the pure layout maths (ui_layout.h).  Build and run with
//   make test
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ui_layout.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Layout constants the screens use.
#define CARD_W 240.0f
#define CARD_GAP 32.0f
#define EMPTY_CARD_W 520.0f
#define OPTIONS_W 380.0f
#define TILE_W 216.0f   /* focused game tile */

// A stand-in for a proportional face: capitals wide, 'i' and spaces narrow.
static float fake_adv(unsigned char c, void *ctx) {
    float size = *(float *)ctx;
    if (c == ' ' || c == 'i' || c == 'l' || c == '.') return 0.30f * size;
    if (c >= 'A' && c <= 'Z') return 0.68f * size;
    if (c == 'W' || c == 'M') return 0.85f * size;
    return 0.52f * size;
}

static void inside_safe(const char *what, float x, float w, int lw) {
    ui_rect_t s = ui_safe_rect();
    CHECK(x >= s.x - 0.01f && x + w <= s.x + s.w + 0.01f, "%s [%.1f..%.1f] outside the safe row at LW=%d",
          what, x, x + w, lw);
}

static void test_canvas(int pw, int ph, int is4x3) {
    ui_layout_init(pw, ph, is4x3);
    int lw = (int)ui_lay.lw;
    CHECK(lw == (is4x3 ? 960 : 1280), "logical width %d", lw);

    // Mapping: the logical canvas covers the whole output.
    CHECK(fabsf(lx(ui_lay.lw) - (float)pw) < 0.01f, "right edge maps to %f", lx(ui_lay.lw));
    CHECK(fabsf(ly(UI_LH) - (float)ph) < 0.01f, "bottom edge maps to %f", ly(UI_LH));

    // Safe rect sits inside the canvas with the documented margins.
    ui_rect_t s = ui_safe_rect();
    CHECK(s.x == 64.0f && s.y == 40.0f, "safe origin %f,%f", s.x, s.y);
    CHECK(s.x + s.w == ui_lay.lw - 64.0f, "safe right");
    CHECK(s.y + s.h == UI_LH - 40.0f, "safe bottom");

    // Anchors.
    CHECK(ui_centre() == ui_lay.lw * 0.5f, "centre");
    CHECK(ui_right() == ui_lay.lw - 64.0f, "right anchor");

    // Shelf: whole cards only, the focused one always inside the safe row.
    float avail = ui_right() - ui_left();
    int fit = ui_shelf_fit(avail, CARD_W, CARD_GAP);
    CHECK(fit == (is4x3 ? 3 : 4), "shelf fits %d cards at LW=%d", fit, lw);
    for (int count = 1; count <= 10; count++) {
        int first = 0;
        for (int step = 0; step < 2 * count; step++) {          // walk right, then left
            int focus = step < count ? step : 2 * count - 1 - step;
            first = ui_shelf_first(focus, first, fit, count);
            float x = ui_shelf_card_x(ui_left(), CARD_W, CARD_GAP, focus, (float)first);
            inside_safe("focused card", x, CARD_W, lw);
            CHECK(first >= 0 && first + (fit < count ? fit : count) <= count,
                  "window [%d..) runs past %d cards", first, count);
            // Every card in the resting window is whole and inside the row.
            for (int i = first; i < first + fit && i < count; i++)
                inside_safe("window card", ui_shelf_card_x(ui_left(), CARD_W, CARD_GAP, i, (float)first), CARD_W, lw);
        }
    }

    // Fixed-width panels and the empty-state card.
    inside_safe("empty-state card", ui_left(), EMPTY_CARD_W, lw);
    inside_safe("empty + settings card", ui_left(), EMPTY_CARD_W + CARD_GAP + CARD_W, lw);
    inside_safe("options panel", ui_right() - OPTIONS_W, OPTIONS_W, lw);

    // Game shelf: the focused tile is anchored at 30% and must be whole.
    float anchor = ui_lay.lw * 0.30f;
    inside_safe("focused game tile", anchor - TILE_W * 0.5f, TILE_W, lw);

    // A 63-character name fits its tile once ellipsized, and says so.
    char name[80];
    memset(name, 0, sizeof(name));
    for (int i = 0; i < 63; i++) name[i] = (i % 7 == 6) ? ' ' : (char)('A' + i % 26);
    float size = 22.0f;
    float max_w = TILE_W - 28.0f;
    char out[96];
    float w = ui_fit_text(name, max_w, fake_adv, &size, out, sizeof(out));
    CHECK(w <= max_w + 0.01f, "ellipsized name is %.1f wide, limit %.1f", w, max_w);
    CHECK(strlen(out) >= 4 && strcmp(out + strlen(out) - 3, "...") == 0, "no ellipsis in \"%s\"", out);
    CHECK(strlen(out) < strlen(name), "not shortened");

    // A short name is left alone.
    w = ui_fit_text("Steam", max_w, fake_adv, &size, out, sizeof(out));
    CHECK(strcmp(out, "Steam") == 0, "short name changed to \"%s\"", out);

    // Degenerate widths must not overrun or crash.
    w = ui_fit_text(name, 1.0f, fake_adv, &size, out, sizeof(out));
    CHECK(strlen(out) <= 3 + 0 || strcmp(out + strlen(out) - 3, "...") == 0, "tiny box: \"%s\"", out);
    w = ui_fit_text(name, 300.0f, fake_adv, &size, out, 8);
    CHECK(strlen(out) < 8, "output buffer overrun");
}

int main(void) {
    static const struct { int w, h, is4x3; const char *name; } modes[] = {
        { 1920, 1080, 0, "1080 16:9" }, { 1280, 720, 0, "720 16:9" }, { 1280, 720, 1, "720 4:3 set" },
        { 720, 576, 0, "576 16:9" }, { 720, 576, 1, "576 4:3" },
        { 720, 480, 0, "480 16:9" }, { 720, 480, 1, "480 4:3" }, { 1440, 1080, 1, "1080 4:3" },
    };
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        int before = failures;
        test_canvas(modes[i].w, modes[i].h, modes[i].is4x3);
        printf("%-12s %s\n", modes[i].name, failures == before ? "ok" : "FAILED");
    }
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
