#include "ui_layout.h"
#include <string.h>

ui_layout_t ui_lay = { 1280, 720, 1280.0f };

void ui_layout_init(int pw, int ph, int is_4x3) {
    ui_lay.pw = (pw > 0) ? pw : 1280;
    ui_lay.ph = (ph > 0) ? ph : 720;
    ui_lay.lw = is_4x3 ? 960.0f : 1280.0f;
}

int ui_shelf_fit(float avail, float card_w, float gap) {
    int n = (int)((avail + gap) / (card_w + gap));
    return n < 1 ? 1 : n;
}

int ui_shelf_first(int focus, int first, int fit, int count) {
    if (focus < first) first = focus;
    if (focus >= first + fit) first = focus - fit + 1;
    if (first > count - fit) first = count - fit;
    if (first < 0) first = 0;
    return first;
}

float ui_shelf_card_x(float left, float card_w, float gap, int i, float first) {
    return left + ((float)i - first) * (card_w + gap);
}

static float width_of(const char *s, size_t len, ui_adv_fn adv, void *ctx) {
    float w = 0.0f;
    for (size_t i = 0; i < len; i++) w += adv((unsigned char)s[i], ctx);
    return w;
}

float ui_fit_text(const char *s, float max_w, ui_adv_fn adv, void *ctx,
                  char *out, size_t n) {
    if (n == 0) return 0.0f;
    size_t len = strlen(s);
    float full = width_of(s, len, adv, ctx);
    if (full <= max_w && len < n) {
        memcpy(out, s, len + 1);
        return full;
    }
    float dots = 3.0f * adv('.', ctx);
    size_t keep = 0;
    float w = 0.0f;
    while (keep < len && keep + 4 < n) {
        float a = adv((unsigned char)s[keep], ctx);
        if (w + a + dots > max_w) break;
        w += a;
        keep++;
    }
    while (keep > 0 && s[keep - 1] == ' ') {   // no space before the dots
        keep--;
        w -= adv(' ', ctx);
    }
    memcpy(out, s, keep);
    memcpy(out + keep, "...", 4);
    return w + dots;
}
