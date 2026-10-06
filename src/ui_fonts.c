#include "ui_fonts.h"
#include "ui_layout.h"
#include <tiny3d.h>
#include <libfont.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "ui.h"
#include "ui_draw.h"
#include "fonts/opensans_bold.h"
#include "fonts/michroma.h"
#include "fonts/materialicons.h"

#define FONT_TEXTURE_BYTES (7 * 512 * 1024)
#define BASE_FRAC 0.80f     // baseline as a fraction of the raster size

// libfont samples 95% of a cell across the quad it draws.
#define CELL_SAMPLE 0.95f

typedef struct {
    int     slot;           // libfont font index
    int     cell;           // cell edge, px
    int     raster;         // FreeType pixel size this face was built at
    int     first, last;
    int     ok;             // built
    float   adv[256];       // advance at `raster` px
    FT_Face ft;
} face_t;

static face_t faces[UI_FACE_COUNT];
static FT_Library ftlib;
static void *tex_block;
static int cur_face_build;     // face the callback is currently filling

// The 8x8 fallback font data lives in ui.c.
extern const unsigned char *ui_get_fallback_bitmap(void);

// Material codepoints, indexed by letter - 'a'.
static const unsigned icon_cp[] = {
    0xE30A, 0xE145, 0xE897, 0xE648, 0xE037, 0xE047,
    0xE8B8, 0xE86C, 0xE000, 0xE88E, 0xE5D5, 0xE872
};
#define NUM_ICONS ((int)(sizeof(icon_cp) / sizeof(icon_cp[0])))

// ---------------------------------------------------------------------------
// Procedural PlayStation button glyphs (chars 1..6 of the body face).  Drawn at
// the design size of 30 px and sampled at the face's raster size.
// ---------------------------------------------------------------------------
static float glyph_alpha(u8 chr, float dx, float dy) {
    float d = sqrtf(dx * dx + dy * dy);
    float alpha = 0.0f;
    const float r_outer = 11.5f, r_inner = 9.5f;

    if (chr >= 1 && chr <= 4 && d <= r_outer && d >= r_inner) {
        float edge = (d > r_outer - 0.75f) ? (r_outer - d) / 0.75f
                   : ((d < r_inner + 0.75f) ? (d - r_inner) / 0.75f : 1.0f);
        if (edge > 0.0f) alpha = fmaxf(alpha, edge * 220.0f);
    }
    if (chr == 1) {
        if (d < r_inner - 0.8f) {
            float l = fminf(fabsf(dx - dy), fabsf(dx + dy)) / 1.4142f;
            if (l < 1.6f && d < 6.5f)
                alpha = fmaxf(alpha, ((l < 0.9f) ? 1.0f : (1.6f - l) / 0.7f) * 255.0f);
        }
    } else if (chr == 2) {
        float dist = fabsf(d - 5.2f);
        if (dist < 1.6f)
            alpha = fmaxf(alpha, ((dist < 0.9f) ? 1.0f : (1.6f - dist) / 0.7f) * 255.0f);
    } else if (chr == 3) {
        float ty = dy + 1.0f;
        float db = fabsf(ty - 4.0f);
        float dl = fabsf(dx * 0.866f + ty * 0.5f + 1.5f);
        float dr = fabsf(-dx * 0.866f + ty * 0.5f + 1.5f);
        if (ty <= 4.2f && ty >= -5.5f && fabsf(dx) <= (ty + 5.5f) * 0.65f + 1.2f) {
            float t = fminf(db, fminf(dl, dr));
            if (t < 1.5f)
                alpha = fmaxf(alpha, ((t < 0.8f) ? 1.0f : (1.5f - t) / 0.7f) * 255.0f);
        }
    } else if (chr == 4) {
        float m = fmaxf(fabsf(dx), fabsf(dy));
        float sd = fabsf(m - 4.8f);
        if (sd < 1.5f && m <= 5.5f)
            alpha = fmaxf(alpha, ((sd < 0.8f) ? 1.0f : (1.5f - sd) / 0.7f) * 255.0f);
    } else if (chr == 5) {
        if (fabsf(dx) <= 2.2f && fabsf(dy) <= 8.5f) alpha = 240.0f;
        if (dy < -2.0f && dy >= -9.5f && fabsf(dx) <= (dy + 9.5f) * 0.9f + 0.8f) alpha = 255.0f;
        if (dy > 2.0f && dy <= 9.5f && fabsf(dx) <= (9.5f - dy) * 0.9f + 0.8f) alpha = 255.0f;
    } else if (chr == 6) {
        float dlft = sqrtf((dx + 4.0f) * (dx + 4.0f) + (dy + 2.5f) * (dy + 2.5f));
        float drgt = sqrtf((dx - 4.0f) * (dx - 4.0f) + (dy + 2.5f) * (dy + 2.5f));
        if (dlft <= 4.8f) alpha = fmaxf(alpha, ((dlft > 4.0f) ? (4.8f - dlft) / 0.8f : 1.0f) * 255.0f);
        if (drgt <= 4.8f) alpha = fmaxf(alpha, ((drgt > 4.0f) ? (4.8f - drgt) / 0.8f : 1.0f) * 255.0f);
        if (dy >= -2.5f && dy <= 8.5f) {
            float mw = (8.5f - dy) * 0.77f;
            if (fabsf(dx) <= mw + 0.8f)
                alpha = fmaxf(alpha, ((fabsf(dx) > mw) ? (mw + 0.8f - fabsf(dx)) / 0.8f : 1.0f) * 255.0f);
        }
    }
    return alpha > 255.0f ? 255.0f : alpha;
}

static void ps_glyph(face_t *f, u8 chr, u8 *bm, short *adv) {
    int C = f->cell;
    float s = (float)f->raster / 30.0f;
    float base = BASE_FRAC * (float)f->raster;
    float cx = 13.0f * s;
    float cy = base - 10.0f * s;      // the glyph centre sits a third of the em above the baseline
    for (int y = 0; y < C; y++) {
        for (int x = 0; x < C; x++) {
            float dx = ((float)x + 0.5f - cx) / s;
            float dy = ((float)y + 0.5f - cy) / s;
            if (fabsf(dx) > 14.0f || fabsf(dy) > 14.0f) continue;
            bm[y * C + x] = (u8)glyph_alpha(chr, dx, dy);
        }
    }
    *adv = (short)(28.0f * s + 0.5f);
    f->adv[chr] = 28.0f * s;
}

// ---------------------------------------------------------------------------
// One callback for every face: libfont hands over only the character, so the
// face being built is a global set before each AddFontFromTTF.  The bitmap
// stride is the cell width.
// ---------------------------------------------------------------------------
static void face_callback(u8 chr, u8 *bm, short *w, short *h, short *ycorr) {
    face_t *f = &faces[cur_face_build];
    int C = f->cell;
    memset(bm, 0, (size_t)C * (size_t)C);
    *w = 0; *h = 0; *ycorr = 0;

    if (cur_face_build == UI_FACE_BODY && chr >= 1 && chr <= 6) {
        ps_glyph(f, chr, bm, w);
        return;
    }
    if (!f->ft) return;

    FT_ULong cp = chr;
    face_t *src = f;
    if (cur_face_build == UI_FACE_ICON) {
        int i = (int)chr - 'a';
        if (i < 0 || i >= NUM_ICONS) return;
        cp = icon_cp[i];
    } else if (cur_face_build == UI_FACE_NUM) {
        cp = (FT_ULong)toupper(chr);
    } else if (cur_face_build == UI_FACE_DISPLAY) {
        // digits from Michroma, capitals from the heading face
        if (chr >= 'A' && chr <= 'Z') src = &faces[UI_FACE_HEAD];
        else if (!(chr >= '0' && chr <= '9')) return;
        if (!src->ft) return;
    }

    if (chr == ' ') {
        float sp = (cur_face_build == UI_FACE_ICON) ? 0.0f : 0.28f * (float)f->raster;
        *w = (short)(sp + 0.5f);
        f->adv[chr] = sp;
        return;
    }

    FT_UInt gi = FT_Get_Char_Index(src->ft, cp);
    if (gi == 0) return;
    if (src != f) FT_Set_Pixel_Sizes(src->ft, 0, (FT_UInt)f->raster);
    if (FT_Load_Glyph(src->ft, gi, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) return;
    FT_GlyphSlot g = src->ft->glyph;
    if (FT_Render_Glyph(g, FT_RENDER_MODE_NORMAL)) return;

    int bw = (int)g->bitmap.width, bh = (int)g->bitmap.rows;
    float adv = (float)g->linearHoriAdvance / 65536.0f;
    if (adv <= 0.0f) adv = (float)(g->advance.x >> 6);

    int ox, oy;
    if (cur_face_build == UI_FACE_ICON) {
        ox = (C - bw) / 2;                 // icons are centred in the cell
        oy = (C - bh) / 2;
        adv = (float)f->raster;
    } else {
        ox = g->bitmap_left;
        oy = (int)(BASE_FRAC * (float)f->raster + 0.5f) - g->bitmap_top;
        if (ox < 0) ox = 0;
    }
    for (int y = 0; y < bh; y++) {
        int yy = y + oy;
        if (yy < 0 || yy >= C) continue;
        for (int x = 0; x < bw; x++) {
            int xx = x + ox;
            if (xx < 0 || xx >= C) continue;
            bm[yy * C + xx] = g->bitmap.buffer[y * g->bitmap.pitch + x];
        }
    }
    *w = (short)(adv + 0.5f);
    f->adv[chr] = adv;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

// Raster size from the output height and the face's most-used logical size,
// capped to what its cell holds.
static int raster_for(float logical, int cell) {
    int r = (int)(logical * (float)ui_lay.ph / UI_LH + 0.5f);
    int cap = (int)(0.75f * (float)cell);
    if (r > cap) r = cap;
    if (r < 12) r = 12;
    return r;
}

static int open_memory_face(face_t *f, const unsigned char *data, unsigned len) {
    if (FT_New_Memory_Face(ftlib, data, (FT_Long)len, 0, &f->ft) != 0) { f->ft = NULL; return 0; }
    return 1;
}

static void add_face(int id, int cell, int raster, int first, int last, int slot, u8 **tex) {
    face_t *f = &faces[id];
    f->cell = cell; f->raster = raster; f->first = first; f->last = last;
    f->slot = slot;
    if (f->ft) FT_Set_Pixel_Sizes(f->ft, 0, (FT_UInt)raster);
    cur_face_build = id;
    *tex = AddFontFromTTF(*tex, (u8)first, (u8)last, cell, cell, face_callback);
    f->ok = 1;
}

void ui_fonts_init(void) {
    memset(faces, 0, sizeof(faces));
    ResetFont();

    tex_block = tiny3d_AllocTexture(FONT_TEXTURE_BYTES);
    if (!tex_block) { ui_push_log("Font: texture allocation failed"); return; }
    u8 *tex = (u8 *)tex_block;

    int have_ft = (FT_Init_FreeType(&ftlib) == 0);

    // Body: Rodin from the console.  Never embedded: it is Sony's font.
    static const char *rodin[] = {
        "/dev_flash/data/font/SCE-PS3-RD-R-LATIN.TTF",
        "/dev_flash/data/font/SCE-PS3-SR-R-LATIN.TTF",
        "/dev_flash/data/font/SCE-PS3-VR-R-LATIN.TTF",
        NULL
    };
    int slot = 0;
    int body_ttf = 0;
    if (have_ft) {
        for (int i = 0; rodin[i] && !body_ttf; i++)
            if (FT_New_Face(ftlib, rodin[i], 0, &faces[UI_FACE_BODY].ft) == 0) {
                char m[96];
                snprintf(m, sizeof(m), "Font: body = %s", rodin[i]);
                ui_push_log(m);
                body_ttf = 1;
            }
    }
    if (body_ttf) {
        add_face(UI_FACE_BODY, 48, raster_for(UI_T_BODY, 48), 1, 127, slot++, &tex);
    } else {
        // 8x8 bitmap font: cell 8, raster 8, advance 8.
        ui_push_log("Font: Rodin unavailable, using the 8x8 bitmap font");
        tex = AddFontFromBitmapArray((u8 *)ui_get_fallback_bitmap(), tex, 32, 127, 8, 8, 1, BIT7_FIRST_PIXEL);
        face_t *b = &faces[UI_FACE_BODY];
        b->cell = 8; b->raster = 8; b->first = 32; b->last = 127; b->ok = 1; b->slot = slot++;
        for (int c = 0; c < 256; c++) b->adv[c] = 8.0f;
    }

    // Embedded faces.  Any that fail resolve to the body face.
    if (have_ft && body_ttf) {
        if (open_memory_face(&faces[UI_FACE_HEAD], opensans_bold_ttf, opensans_bold_ttf_len))
            add_face(UI_FACE_HEAD, 64, raster_for(32.0f, 64), 32, 126, slot++, &tex);
        else
            ui_push_log("Font: heading face failed, using body");

        if (open_memory_face(&faces[UI_FACE_NUM], michroma_ttf, michroma_ttf_len))
            add_face(UI_FACE_NUM, 48, raster_for(UI_T_SPEC, 48), 32, 90, slot++, &tex);
        else
            ui_push_log("Font: number face failed, using body");

        if (faces[UI_FACE_NUM].ft && faces[UI_FACE_HEAD].ft) {
            // Shares Michroma's FreeType handle for the digits.
            faces[UI_FACE_DISPLAY].ft = faces[UI_FACE_NUM].ft;
            add_face(UI_FACE_DISPLAY, 96, raster_for(UI_T_PIN, 96), '0', 'Z', slot++, &tex);
        }

        if (open_memory_face(&faces[UI_FACE_ICON], materialicons_ttf, materialicons_ttf_len))
            add_face(UI_FACE_ICON, 48, raster_for(32.0f, 48), 'a', 'a' + NUM_ICONS - 1, slot++, &tex);
        else
            ui_push_log("Font: icon face failed");
    }

    SetCurrentFont(0);
    SetFontSize(16, 16);
    SetFontColor(0xffffffff, 0);

    char m[96];
    snprintf(m, sizeof(m), "Font: %d faces, %u of %u KB texture", slot,
             (unsigned)(((u8 *)tex - (u8 *)tex_block) / 1024), (unsigned)(FONT_TEXTURE_BYTES / 1024));
    ui_push_log(m);
}

void ui_fonts_shutdown(void) {
    for (int i = 0; i < UI_FACE_COUNT; i++) {
        if (faces[i].ft && i != UI_FACE_DISPLAY) FT_Done_Face(faces[i].ft);
        faces[i].ft = NULL;
    }
    if (ftlib) { FT_Done_FreeType(ftlib); ftlib = NULL; }
}

// ---------------------------------------------------------------------------
// Measure and draw
// ---------------------------------------------------------------------------
static face_t *resolve(int face) {
    if (face < 0 || face >= UI_FACE_COUNT || !faces[face].ok) return &faces[UI_FACE_BODY];
    return &faces[face];
}

// The byte actually drawn for `c`: upper-cased in the number face, '?' when the
// face has no such character, 0 when nothing is drawn.
static unsigned char map_char(const face_t *f, unsigned char c) {
    if (f == &faces[UI_FACE_NUM]) c = (unsigned char)toupper(c);
    if (c >= f->first && c <= f->last) return c;
    if ('?' >= f->first && '?' <= f->last && c < 0x80) return '?';
    return 0;
}

static float adv_logical(const face_t *f, unsigned char c, float size) {
    c = map_char(f, c);
    if (!c) return 0.0f;
    return f->adv[c] * size / (float)f->raster;
}

float ui_text_width(int face, float size, const char *str) {
    const face_t *f = resolve(face);
    float w = 0.0f;
    for (; *str; str++) w += adv_logical(f, (unsigned char)*str, size);
    return w;
}

static void draw_run(const face_t *f, float size, float x, float y, ui_col_t col, const char *str) {
    float px = size * (float)ui_lay.pw / ui_lay.lw;     // physical size per axis
    float py = size * (float)ui_lay.ph / UI_LH;
    if (px < 12.0f) px = 12.0f;
    if (py < 12.0f) py = 12.0f;
    float k = (float)f->cell * CELL_SAMPLE / (float)f->raster;
    int sx = (int)(px * k + 0.5f), sy = (int)(py * k + 0.5f);

    col.a *= ui_get_alpha();
    SetCurrentFont(f->slot);
    SetFontSize(sx, sy);
    u32 rgba = ((u32)(col.r * 255.0f + 0.5f) << 24) | ((u32)(col.g * 255.0f + 0.5f) << 16) |
               ((u32)(col.b * 255.0f + 0.5f) << 8) | (u32)(col.a * 255.0f + 0.5f);
    SetFontColor(rgba, 0);

    float cx = lx(x + ui_get_offset_x()), cy = ly(y + ui_get_offset_y());
    float per_adv = size / (float)f->raster * (float)ui_lay.pw / ui_lay.lw;
    for (; *str; str++) {
        unsigned char c = map_char(f, (unsigned char)*str);
        if (!c) continue;
        if (c != ' ') DrawChar(cx, cy, 0.0f, c);
        cx += f->adv[c] * per_adv;
    }
}

float ui_text(int face, float size, float x, float y, int align,
              ui_col_t color, const char *str) {
    const face_t *f = resolve(face);
    float w = ui_text_width(face, size, str);
    if (align == UI_ALIGN_CENTER) x -= w * 0.5f;
    else if (align == UI_ALIGN_RIGHT) x -= w;
    draw_run(f, size, x, y, color, str);
    return w;
}

float ui_text_shadow(int face, float size, float x, float y, int align,
                     ui_col_t color, const char *str) {
    ui_col_t sh = { 0.0f, 0.0f, 0.0f, 0.35f * color.a };
    ui_text(face, size, x + 2.0f, y + 2.0f, align, sh, str);
    return ui_text(face, size, x, y, align, color, str);
}

typedef struct { const face_t *f; float size; } adv_ctx_t;
static float adv_cb(unsigned char c, void *ctx) {
    const adv_ctx_t *a = (const adv_ctx_t *)ctx;
    return adv_logical(a->f, c, a->size);
}

float ui_text_fit(int face, float size, float x, float y, int align,
                  ui_col_t color, float max_w, const char *str) {
    char buf[160];
    adv_ctx_t ctx = { resolve(face), size };
    float w = ui_fit_text(str, max_w, adv_cb, &ctx, buf, sizeof(buf));
    if (align == UI_ALIGN_CENTER) x -= w * 0.5f;
    else if (align == UI_ALIGN_RIGHT) x -= w;
    draw_run(ctx.f, size, x, y, color, buf);
    return w;
}

void ui_fit_string(int face, float size, float max_w, const char *in, char *out, size_t n) {
    adv_ctx_t ctx = { resolve(face), size };
    ui_fit_text(in, max_w, adv_cb, &ctx, out, n);
}

int ui_text_wrap(int face, float size, float x, float y, int align,
                 ui_col_t color, float max_w, float line_h, int max_lines,
                 const char *str) {
    const face_t *f = resolve(face);
    int lines = 0;
    char line[160];
    while (*str && lines < max_lines) {
        while (*str == ' ') str++;
        if (!*str) break;
        size_t n = 0, last_space = 0;
        float w = 0.0f;
        while (str[n] && n < sizeof(line) - 1) {
            float a = adv_logical(f, (unsigned char)str[n], size);
            if (w + a > max_w && n > 0) {
                if (last_space > 0) n = last_space;   // break at the last space
                break;
            }
            w += a;
            if (str[n] == ' ') last_space = n;
            n++;
        }
        memcpy(line, str, n);
        line[n] = '\0';
        ui_text(face, size, x, y + (float)lines * line_h, align, color, line);
        str += n;
        lines++;
    }
    return lines;
}

void ui_icon(int icon, float cx, float cy, float size, ui_col_t color) {
    const face_t *f = &faces[UI_FACE_ICON];
    if (!f->ok) return;
    char s[2] = { (char)icon, 0 };
    float cell = (float)f->cell * size / (float)f->raster;   // logical edge of the whole cell
    draw_run(f, size, cx - cell * 0.5f, cy - cell * 0.5f, color, s);
}
