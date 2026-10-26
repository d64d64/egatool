/*
 * EGAtool tests: the engine on small generated images, and the command
 * line tool end to end. Run with `make test` (built with sanitizers).
 * Usage: test PATH-TO-egatool-cli
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "imageio.h"

static int checks, failures;
static const char *current;

#define CHECK(cond, ...) check_((cond), __LINE__, __VA_ARGS__)
static void check_(int ok, int line, const char *fmt, ...)
{
    va_list ap;
    checks++;
    if (ok)
        return;
    failures++;
    fprintf(stderr, "FAIL %s (line %d): ", current, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- helpers ------------------------------------------------------------ */

static unsigned rng_state = 1;
static unsigned rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7fff;
}

static void set_px(EgtImage *img, int x, int y, int r, int g, int b)
{
    uint8_t *p = img->pixels + ((size_t)y * img->width + x) * 4;
    p[0] = (uint8_t)r;
    p[1] = (uint8_t)g;
    p[2] = (uint8_t)b;
    p[3] = 255;
}

static const uint8_t *px(const EgtImage *img, int x, int y)
{
    return img->pixels + ((size_t)y * img->width + x) * 4;
}

static EgtImage flat(int w, int h, int r, int g, int b)
{
    EgtImage img = egt_image_alloc(w, h);
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            set_px(&img, x, y, r, g, b);
    return img;
}

/* Something photo-like: gradients, a few discs, a little noise. */
static EgtImage photo(int w, int h, unsigned seed)
{
    EgtImage img = egt_image_alloc(w, h);
    int x, y;
    rng_state = seed;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int r = x * 255 / (w - 1), g = y * 255 / (h - 1), b = 255 - (x + y) * 255 / (w + h - 2);
            int dx = x - w / 3, dy = y - h / 2, n = (int)(rnd() % 21) - 10;
            if (dx * dx + dy * dy < (h / 4) * (h / 4))
                r = 230, g = 200, b = 40;
            if ((x - 3 * w / 4) * (x - 3 * w / 4) + (y - h / 4) * (y - h / 4) < (h / 6) * (h / 6))
                r = 20, g = 30, b = 120;
            r += n, g += n, b += n;
            set_px(&img, x, y, r < 0 ? 0 : r > 255 ? 255 : r, g < 0 ? 0 : g > 255 ? 255 : g,
                   b < 0 ? 0 : b > 255 ? 255 : b);
        }
    return img;
}

static int pal_index(const EgtPalette *pal, const uint8_t *p)
{
    int i;
    for (i = 0; i < pal->count; i++)
        if (pal->colors[i].r == p[0] && pal->colors[i].g == p[1] && pal->colors[i].b == p[2])
            return i;
    return -1;
}

static bool all_in_palette(const EgtImage *img, const EgtPalette *pal)
{
    int x, y;
    for (y = 0; y < img->height; y++)
        for (x = 0; x < img->width; x++)
            if (pal_index(pal, px(img, x, y)) < 0)
                return false;
    return true;
}

static bool same_pixels(const EgtImage *a, const EgtImage *b)
{
    return a->width == b->width && a->height == b->height &&
           !memcmp(a->pixels, b->pixels, (size_t)a->width * a->height * 4);
}

static EgtConvertOptions options(EgtDither d)
{
    EgtConvertOptions o;
    memset(&o, 0, sizeof o);
    o.dither = d;
    o.prefer_solid = EGT_PREFER_SOLID_DEFAULT;
    o.snap = 16;
    o.consistency = 30;
    return o;
}

static EgtImage convert(const EgtImage *src, const EgtPalette *pal, const EgtConvertOptions *o)
{
    EgtImage out = egt_image_alloc(src->width, src->height);
    if (!egt_convert(src, &out, pal, o))
        egt_image_free(&out);
    return out;
}

static EgtPalette two_colours(int r1, int g1, int b1, int r2, int g2, int b2)
{
    EgtPalette p;
    memset(&p, 0, sizeof p);
    p.count = 2;
    p.colors[0] = (EgtColor){ (uint8_t)r1, (uint8_t)g1, (uint8_t)b1 };
    p.colors[1] = (EgtColor){ (uint8_t)r2, (uint8_t)g2, (uint8_t)b2 };
    return p;
}

/* Share of pixels in palette colour 0. */
static double share0(const EgtImage *img, const EgtPalette *pal)
{
    size_t n = 0, i, total = (size_t)img->width * img->height;
    for (i = 0; i < total; i++)
        n += pal_index(pal, img->pixels + i * 4) == 0;
    return (double)n / total;
}

/* ---- engine tests --------------------------------------------------------- */

static void test_palettes(void)
{
    EgtPalette p;
    int k;
    current = "palettes";
    for (k = 0; k < EGT_PAL_COUNT; k++) {
        egt_builtin_palette((EgtBuiltinPalette)k, &p);
        CHECK(p.count >= 2 && p.count <= EGT_MAX_COLORS, "%s has %d colours",
              egt_builtin_palette_name((EgtBuiltinPalette)k), p.count);
    }
    egt_builtin_palette(EGT_PAL_EGA, &p);
    CHECK(p.count == 16, "EGA has %d colours", p.count);
    egt_builtin_palette(EGT_PAL_VGA, &p);
    CHECK(p.count == 256, "VGA has %d colours", p.count);
}

static void test_dither_modes(void)
{
    EgtImage src = photo(53, 37, 7);
    EgtPalette ega, vga;
    int d;
    egt_builtin_palette(EGT_PAL_EGA, &ega);
    egt_builtin_palette(EGT_PAL_VGA, &vga);
    for (d = 0; d < EGT_DITHER_COUNT; d++) {
        EgtConvertOptions o = options((EgtDither)d);
        EgtImage a, b, solid, solid_out;
        current = egt_dither_name((EgtDither)d);
        a = convert(&src, &ega, &o);
        b = convert(&src, &ega, &o);
        CHECK(a.pixels != NULL, "conversion failed");
        if (!a.pixels)
            continue;
        CHECK(all_in_palette(&a, &ega), "output has colours outside the palette");
        CHECK(same_pixels(&a, &b), "two runs differ");
        /* A palette colour converts to itself, without dither dots. */
        solid = flat(24, 16, 0x55, 0xff, 0x55);
        solid_out = convert(&solid, &ega, &o);
        CHECK(solid_out.pixels && same_pixels(&solid, &solid_out), "a palette colour gets dithered");
        if (d != EGT_DITHER_EGA_CHECKER) {
            EgtImage v = convert(&src, &vga, &o);
            CHECK(v.pixels && all_in_palette(&v, &vga), "VGA output has colours outside the palette");
            egt_image_free(&v);
        } else {
            EgtImage v = convert(&src, &vga, &o);
            CHECK(v.pixels == NULL, "checkerboard accepted a 256-colour palette");
            egt_image_free(&v);
        }
        egt_image_free(&a);
        egt_image_free(&b);
        egt_image_free(&solid);
        egt_image_free(&solid_out);
    }
    egt_image_free(&src);
}

/* Extremes (pure white and black, saturated primaries) with every palette
 * and mode: only palette colours come out. A cache once mistook pure
 * white for an empty slot and drew garbage. */
static void test_extremes(void)
{
    EgtImage src = egt_image_alloc(16, 16);
    static const int cols[][3] = { { 255, 255, 255 }, { 0, 0, 0 }, { 255, 0, 0 }, { 0, 255, 0 },
                                   { 0, 0, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 255, 0, 255 } };
    int x, y, p, d;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            const int *c = cols[(x / 4 + y / 4 * 4) % 8];
            set_px(&src, x, y, c[0], c[1], c[2]);
        }
    for (p = 0; p < EGT_PAL_COUNT; p++) {
        EgtPalette pal;
        egt_builtin_palette((EgtBuiltinPalette)p, &pal);
        for (d = 0; d < EGT_DITHER_COUNT; d++) {
            EgtConvertOptions o = options((EgtDither)d);
            EgtImage out;
            char name[96];
            snprintf(name, sizeof name, "extremes, %s, %s", egt_builtin_palette_name((EgtBuiltinPalette)p),
                     egt_dither_name((EgtDither)d));
            current = name;
            out = convert(&src, &pal, &o);
            if (out.pixels) {
                /* Pure white (the top-left block) stays light: nothing
                 * under 40% of the palette's brightest colour. */
                int i, top = 0, dark = 0;
                for (i = 0; i < pal.count; i++) {
                    int l = 299 * pal.colors[i].r + 587 * pal.colors[i].g + 114 * pal.colors[i].b;
                    top = l > top ? l : top;
                }
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++) {
                        const uint8_t *q = px(&out, x, y);
                        dark += (299 * q[0] + 587 * q[1] + 114 * q[2]) * 10 < top * 4;
                    }
                CHECK(all_in_palette(&out, &pal), "colours outside the palette");
                CHECK(!dark, "%d dark pixels in pure white", dark);
            }
            egt_image_free(&out);
        }
    }
    egt_image_free(&src);
}

/* A colour halfway between two palette colours becomes an even mix. */
static void test_midpoint_mix(void)
{
    static const int pairs[][7] = {
        /* palette colours, then the source colour */
        { 0, 0, 0, 255, 255, 255, 128 },     /* black/white, grey */
        { 255, 0, 0, 0, 0, 255, -1 },        /* red/blue, purple */
    };
    int k, d;
    for (k = 0; k < 2; k++) {
        const int *q = pairs[k];
        EgtPalette pal = two_colours(q[0], q[1], q[2], q[3], q[4], q[5]);
        EgtImage src = q[6] >= 0 ? flat(32, 32, q[6], q[6], q[6]) : flat(32, 32, 128, 0, 128);
        for (d = 0; d < EGT_DITHER_COUNT; d++) {
            EgtConvertOptions o = options((EgtDither)d);
            EgtImage out;
            double s;
            if (d == EGT_DITHER_NONE)
                continue;
            /* Known: Ordered moves colours along the grey axis, so it can't
             * mix colours told apart by hue alone; Ordered 2-colour can. */
            if (k == 1 && d == EGT_DITHER_ORDERED)
                continue;
            o.snap = 0;
            o.prefer_solid = 0;
            current = egt_dither_name((EgtDither)d);
            out = convert(&src, &pal, &o);
            s = out.pixels ? share0(&out, &pal) : -1;
            CHECK(s > 0.4 && s < 0.6, "%s: %.0f%% of the first colour, expected about 50%%",
                  k ? "red/blue purple" : "black/white grey", s * 100);
            egt_image_free(&out);
        }
        egt_image_free(&src);
    }
}

/* ---- hardware limits ------------------------------------------------------- */

/* Colours (palette indices) used in a rectangle, as a bit set. */
typedef struct {
    uint64_t bits[4];
} Set;

static Set colours_in(const EgtImage *img, const EgtPalette *pal, int x0, int y0, int w, int h)
{
    Set s;
    int x, y;
    memset(&s, 0, sizeof s);
    for (y = y0; y < y0 + h && y < img->height; y++)
        for (x = x0; x < x0 + w && x < img->width; x++) {
            int i = pal_index(pal, px(img, x, y));
            if (i >= 0)
                s.bits[i / 64] |= 1ull << (i % 64);
        }
    return s;
}

static int set_count(const Set *s)
{
    int n = 0, i;
    for (i = 0; i < 256; i++)
        n += (int)((s->bits[i / 64] >> (i % 64)) & 1);
    return n;
}

static bool set_has(const Set *s, int i)
{
    return (s->bits[i / 64] >> (i % 64)) & 1;
}

static void set_del(Set *s, int i)
{
    s->bits[i / 64] &= ~(1ull << (i % 64));
}

static bool subset(const Set *a, const Set *b)
{
    int k;
    for (k = 0; k < 4; k++)
        if (a->bits[k] & ~b->bits[k])
            return false;
    return true;
}

/* Whether the cells' colours fit nsub palettes of `per` colours plus the
 * background bg (greedy: biggest sets first, each inside a palette). */
static bool fits_subpalettes(Set *cells, int ncells, int bg, int per, int nsub)
{
    Set pals[64];
    int npal = 0, i, j, size;
    for (size = per; size >= 1; size--)
        for (i = 0; i < ncells; i++) {
            Set s = cells[i];
            if (bg >= 0)
                set_del(&s, bg);
            if (set_count(&s) != size)
                continue;
            for (j = 0; j < npal && !subset(&s, &pals[j]); j++)
                ;
            if (j == npal) {
                if (npal == 64)
                    return false;
                pals[npal++] = s;
            }
        }
    return npal <= nsub;
}

static void check_limit(EgtLimit lim, const EgtPalette *pal, const EgtImage *out)
{
    int cw = 8, ch = 8, per = 2, wide = 1, nsub = 0, x, y, i, ncells = 0;
    bool shared = false;
    Set cells[4096];
    switch (lim) {
    case EGT_LIMIT_C64_MULTI: cw = 8; per = 3; wide = 2; shared = true; break;
    case EGT_LIMIT_MSX: ch = 1; break;
    case EGT_LIMIT_NES: cw = ch = 16; per = 3; shared = true; nsub = 4; break;
    default: break;
    }
    if (wide == 2)
        for (y = 0; y < out->height; y++)
            for (x = 0; x + 1 < out->width; x += 2)
                CHECK(!memcmp(px(out, x, y), px(out, x + 1, y), 3), "pixel pair %d,%d differs", x, y);
    for (y = 0; y < out->height; y += ch)
        for (x = 0; x < out->width; x += cw)
            cells[ncells++] = colours_in(out, pal, x, y, cw, ch);
    if (!shared) {
        for (i = 0; i < ncells; i++)
            CHECK(set_count(&cells[i]) <= per, "a cell has %d colours", set_count(&cells[i]));
        return;
    }
    /* Some background colour must make every cell fit. */
    {
        bool ok = false;
        int bg;
        for (bg = 0; bg < pal->count && !ok; bg++) {
            bool fits = true;
            for (i = 0; i < ncells && fits; i++) {
                Set s = cells[i];
                set_del(&s, bg);
                fits = set_count(&s) <= per;
            }
            if (fits && nsub)
                fits = fits_subpalettes(cells, ncells, bg, per, nsub);
            ok = fits;
        }
        CHECK(ok, "no background colour makes every cell fit the rule");
    }
}

/* Spectrum: a cell's colours are all normal or all bright (black is both). */
static void check_bright(const EgtPalette *pal, const EgtImage *out)
{
    int x, y, i;
    for (y = 0; y < out->height; y += 8)
        for (x = 0; x < out->width; x += 8) {
            Set s = colours_in(out, pal, x, y, 8, 8);
            int normal = 0, bright = 0;
            for (i = 0; i < pal->count; i++) {
                const EgtColor *c = &pal->colors[i];
                int m = c->r > c->g ? (c->r > c->b ? c->r : c->b) : (c->g > c->b ? c->g : c->b);
                if (!set_has(&s, i) || m == 0)
                    continue;
                if (m == 255)
                    bright = 1;
                else
                    normal = 1;
            }
            CHECK(!(normal && bright), "cell %d,%d mixes normal and bright colours", x, y);
        }
}

static void test_limits(void)
{
    /* Not a multiple of the cell size, so the last cells are partial. */
    EgtImage src = photo(70, 52, 3);
    int l, d, p;
    for (l = EGT_LIMIT_NONE + 1; l < EGT_LIMIT_COUNT; l++) {
        EgtBuiltinPalette own;
        int w, h;
        egt_limit_machine((EgtLimit)l, &own, &w, &h);
        for (p = 0; p < 2; p++) {
            EgtPalette pal;
            egt_builtin_palette(p ? EGT_PAL_EGA : own, &pal);
            for (d = 0; d < EGT_DITHER_COUNT; d++) {
                EgtConvertOptions o = options((EgtDither)d);
                EgtImage a, b;
                char name[96];
                if (d == EGT_DITHER_SIERRA || d == EGT_DITHER_STUCKI)
                    continue; /* same code path as the other diffusions */
                snprintf(name, sizeof name, "%s, %s palette, %s", egt_limit_name((EgtLimit)l),
                         p ? "EGA" : "own", egt_dither_name((EgtDither)d));
                current = name;
                o.limit = (EgtLimit)l;
                a = convert(&src, &pal, &o);
                CHECK(a.pixels != NULL, "conversion failed");
                if (!a.pixels)
                    continue;
                CHECK(all_in_palette(&a, &pal), "colours outside the palette");
                check_limit((EgtLimit)l, &pal, &a);
                if (l == EGT_LIMIT_SPECTRUM && !p)
                    check_bright(&pal, &a);
                b = convert(&src, &pal, &o);
                CHECK(b.pixels && same_pixels(&a, &b), "two runs differ");
                egt_image_free(&a);
                egt_image_free(&b);
            }
        }
    }
    egt_image_free(&src);
}

/* NES: a large area must get its colours even when the top of the picture
 * already has many different areas (the palette pool once filled up from
 * the top). */
static void test_nes_pool(void)
{
    EgtPalette pal;
    EgtImage src = egt_image_alloc(256, 240), out;
    EgtConvertOptions o = options(EGT_DITHER_NONE);
    int x, y, cx, cy, i, hits = 0, total = 0;
    current = "NES palette pool";
    egt_builtin_palette(EGT_PAL_NES, &pal);
    rng_state = 99;
    /* Top half: every 16x16 area a different mix of three colours. */
    for (cy = 0; cy < 8; cy++)
        for (cx = 0; cx < 16; cx++) {
            int c[3];
            for (i = 0; i < 3; i++)
                c[i] = 1 + (int)(rnd() % 20);
            for (y = 0; y < 16; y++)
                for (x = 0; x < 16; x++) {
                    const EgtColor *k = &pal.colors[c[(x / 4 + y / 4) % 3]];
                    set_px(&src, cx * 16 + x, cy * 16 + y, k->r, k->g, k->b);
                }
        }
    /* Bottom half: one colour not used above. */
    for (y = 128; y < 240; y++)
        for (x = 0; x < 256; x++) {
            const EgtColor *k = &pal.colors[40];
            set_px(&src, x, y, k->r, k->g, k->b);
        }
    o.limit = EGT_LIMIT_NES;
    out = convert(&src, &pal, &o);
    CHECK(out.pixels != NULL, "conversion failed");
    if (out.pixels) {
        for (y = 128; y < 240; y++)
            for (x = 0; x < 256; x++, total++)
                hits += pal_index(&pal, px(&out, x, y)) == 40;
        CHECK(hits > total * 9 / 10, "the bottom area kept its colour in only %d%% of pixels",
              hits * 100 / total);
    }
    egt_image_free(&src);
    egt_image_free(&out);
}

/* ---- resize and adjustments ---------------------------------------------- */

static void mean_rgb(const EgtImage *img, double *m)
{
    size_t i, n = (size_t)img->width * img->height;
    int c;
    m[0] = m[1] = m[2] = 0;
    for (i = 0; i < n; i++)
        for (c = 0; c < 3; c++)
            m[c] += img->pixels[i * 4 + c];
    for (c = 0; c < 3; c++)
        m[c] /= (double)n;
}

static void test_resize(void)
{
    EgtImage src = photo(64, 48, 11);
    static const int sizes[][2] = { { 32, 24 }, { 20, 15 }, { 7, 5 }, { 1, 1 } };
    double ms[3], mo[3];
    int k, c;
    current = "resize";
    mean_rgb(&src, ms);
    for (k = 0; k < 4; k++) {
        EgtImage a = egt_resize_area(&src, sizes[k][0], sizes[k][1]);
        EgtImage n = egt_resize_nearest(&src, sizes[k][0], sizes[k][1]);
        CHECK(a.pixels && a.width == sizes[k][0] && a.height == sizes[k][1], "area resize size");
        CHECK(n.pixels && n.width == sizes[k][0] && n.height == sizes[k][1], "nearest resize size");
        if (a.pixels) {
            mean_rgb(&a, mo);
            for (c = 0; c < 3; c++)
                CHECK(fabs(mo[c] - ms[c]) < 1.0, "to %dx%d the average moved %.2f levels",
                      sizes[k][0], sizes[k][1], mo[c] - ms[c]);
        }
        egt_image_free(&a);
        egt_image_free(&n);
    }
    {
        EgtImage same = egt_resize_area(&src, 64, 48);
        CHECK(same.pixels && same_pixels(&same, &src), "area resize to the same size changed pixels");
        egt_image_free(&same);
    }
    egt_image_free(&src);
}

static void test_adjust(void)
{
    EgtImage img = egt_image_alloc(256, 1);
    EgtAdjust adj;
    int v;
    current = "adjust";
    for (v = 0; v < 256; v++)
        set_px(&img, v, 0, v, v, v);
    memset(&adj, 0, sizeof adj);
    {
        EgtImage c = egt_image_copy(&img);
        egt_adjust(&c, &adj);
        CHECK(same_pixels(&c, &img), "zero adjustments changed pixels");
        egt_image_free(&c);
    }
    {
        EgtImage c = egt_image_copy(&img);
        adj.contrast = 64;
        egt_adjust(&c, &adj);
        CHECK(px(&c, 64, 0)[0] == 0 && px(&c, 191, 0)[0] == 255 && px(&c, 10, 0)[0] == 0,
              "contrast 64 should stretch 64..191 to 0..255");
        egt_image_free(&c);
    }
    {
        EgtImage c = egt_image_copy(&img);
        adj.contrast = -64;
        egt_adjust(&c, &adj);
        CHECK(px(&c, 0, 0)[0] == 64 && px(&c, 255, 0)[0] == 191, "contrast -64 should squeeze 0..255 to 64..191");
        egt_image_free(&c);
    }
    {
        EgtImage c = egt_image_copy(&img);
        adj.contrast = 0;
        adj.brightness = 127;
        egt_adjust(&c, &adj);
        CHECK(px(&c, 0, 0)[0] == 127 && px(&c, 200, 0)[0] == 255, "brightness 127");
        egt_image_free(&c);
    }
    {
        EgtImage c = flat(4, 4, 200, 60, 60);
        adj.brightness = 0;
        adj.saturation = -1;
        egt_adjust(&c, &adj);
        CHECK(abs(px(&c, 0, 0)[0] - px(&c, 0, 0)[1]) <= 1 && abs(px(&c, 0, 0)[1] - px(&c, 0, 0)[2]) <= 1,
              "saturation -1 should give grey");
        egt_image_free(&c);
    }
    egt_image_free(&img);
}

/* ---- files ------------------------------------------------------------------ */

static char tmpdir[64];

static const char *tmp(const char *name)
{
    static char buf[4][128];
    static int k;
    k = (k + 1) % 4;
    snprintf(buf[k], sizeof buf[k], "%s/%s", tmpdir, name);
    return buf[k];
}

/* The "Comment" text chunk of a PNG file, or NULL. */
static char *png_comment(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char head[8];
    char *found = NULL;
    if (!f)
        return NULL;
    if (fread(head, 1, 8, f) != 8) {
        fclose(f);
        return NULL;
    }
    for (;;) {
        unsigned char h[8];
        unsigned long len;
        char *data;
        if (fread(h, 1, 8, f) != 8)
            break;
        len = (unsigned long)h[0] << 24 | (unsigned long)h[1] << 16 | (unsigned long)h[2] << 8 | h[3];
        if (len > 1 << 20)
            break;
        data = malloc(len + 1);
        if (!data || fread(data, 1, len, f) != len || fseek(f, 4, SEEK_CUR)) {
            free(data);
            break;
        }
        data[len] = 0;
        if (!memcmp(h + 4, "tEXt", 4) && !strcmp(data, "Comment")) {
            found = strdup(data + 8);
            free(data);
            break;
        }
        free(data);
    }
    fclose(f);
    return found;
}

static void test_files(void)
{
    EgtImage img = photo(31, 17, 5);
    static const char *exts[] = { "png", "bmp", "tga" };
    int k;
    current = "files";
    CHECK(egt_save_image_comment(tmp("c.png"), &img, "egatool-cli -p ega in.png out.png"), "save with comment");
    {
        char *c = png_comment(tmp("c.png"));
        EgtImage back = egt_load_image(tmp("c.png"));
        CHECK(c && !strcmp(c, "egatool-cli -p ega in.png out.png"), "comment read back as '%s'", c ? c : "(none)");
        CHECK(back.pixels && same_pixels(&back, &img), "pixels read back differ");
        free(c);
        egt_image_free(&back);
    }
    for (k = 0; k < 3; k++) {
        char name[32];
        EgtImage back;
        snprintf(name, sizeof name, "plain.%s", exts[k]);
        CHECK(egt_save_image(tmp(name), &img), "save %s", exts[k]);
        back = egt_load_image(tmp(name));
        CHECK(back.pixels && same_pixels(&back, &img), "%s pixels read back differ", exts[k]);
        egt_image_free(&back);
        if (access("/dev/full", W_OK) == 0) {
            snprintf(name, sizeof name, "full.%s", exts[k]);
            if (symlink("/dev/full", tmp(name)) == 0)
                CHECK(!egt_save_image_comment(tmp(name), &img, k ? NULL : "x"),
                      "saving %s to a full disk reported success", exts[k]);
        }
    }
    egt_image_free(&img);
}

/* ---- command line ----------------------------------------------------------- */

static const char *cli;

static int run(const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    int st;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    st = system(cmd);
    return st == -1 ? -1 : (st >> 8) & 0xff;
}

static void test_cli_validation(void)
{
    static const char *bad[] = {
        "--brightness rubbish", "--brightness 300", "--contrast 128", "--saturation nan",
        "--saturation 2", "--snap -1", "--snap 65", "--consistency 301", "-s 16x16garbage",
        "-s 0x10", "--radius 9", "--threshold 0", "--outline-dark 1.5", "--prefer-solid 101",
        "--light-mix x", "--harsh-mix 5x", "-x 9", "--min-region -3", "--calibrate-max -1",
        "--outline 999999999999",
    };
    static const char *good[] = {
        "--brightness -128", "--contrast -127", "--saturation 1", "--snap 64", "-s 50%",
        "-s 16x12", "--radius 0.25", "--outline-dark 0", "--consistency 300",
    };
    size_t k;
    EgtImage img = photo(32, 24, 9);
    egt_save_image(tmp("in.png"), &img);
    egt_image_free(&img);
    current = "command line options";
    for (k = 0; k < sizeof bad / sizeof *bad; k++)
        CHECK(run("%s %s %s %s 2>/dev/null", cli, bad[k], tmp("in.png"), tmp("o.png")) == 2,
              "'%s' was accepted", bad[k]);
    for (k = 0; k < sizeof good / sizeof *good; k++)
        CHECK(run("%s %s %s %s 2>/dev/null", cli, good[k], tmp("in.png"), tmp("o.png")) == 0,
              "'%s' was rejected", good[k]);
    CHECK(run("%s -p vga -d checker %s %s 2>/dev/null", cli, tmp("in.png"), tmp("o.png")) == 1,
          "checkerboard with 256 colours should fail");
}

/* Replaying the settings saved in a PNG gives the same pixels. */
static void test_recipe_replay(void)
{
    static const char *cases[] = {
        "-d sierra --saturation 0.12345 --brightness 7 --contrast -9",
        "-d checker --outline 40 --outline-dark 0.555 --light-mix 33",
        "-d floyd --outline 30 --outline-mode hires --outline-min 3 --smooth surface --radius 1.3",
        "-p cga -d ordered --calibrate --snap 9 -s 40x30",
        "-p c64 -d burkes --limit c64-multi --consistency 120 -s 50%",
        "--auto-contrast -d none --min-region 4 -x 2",
    };
    size_t k;
    EgtImage img = photo(64, 48, 21);
    egt_save_image(tmp("src.png"), &img);
    egt_image_free(&img);
    for (k = 0; k < sizeof cases / sizeof *cases; k++) {
        char *c, *args, cmd[2048];
        EgtImage a, b;
        current = cases[k];
        if (run("%s %s %s %s 2>/dev/null", cli, cases[k], tmp("src.png"), tmp("first.png")) != 0) {
            CHECK(0, "conversion failed");
            continue;
        }
        c = png_comment(tmp("first.png"));
        CHECK(c && !strncmp(c, "egatool-cli ", 12), "no settings saved");
        if (!c)
            continue;
        /* The saved line names the files: run it on ours instead. */
        args = c + 12;
        {
            char *src = strstr(args, " src.png first.png");
            CHECK(src != NULL, "saved settings end oddly: %s", c);
            if (src)
                *src = 0;
        }
        snprintf(cmd, sizeof cmd, "%s %s %s %s 2>/dev/null", cli, args, tmp("src.png"), tmp("second.png"));
        CHECK(run("%s", cmd) == 0, "replaying failed: %s", args);
        a = egt_load_image(tmp("first.png"));
        b = egt_load_image(tmp("second.png"));
        CHECK(a.pixels && b.pixels && same_pixels(&a, &b), "replay differs: %s", args);
        egt_image_free(&a);
        egt_image_free(&b);
        free(c);
    }
}

int main(int argc, char **argv)
{
    char cmd[128];
    if (argc < 2) {
        fprintf(stderr, "usage: test PATH-TO-egatool-cli\n");
        return 2;
    }
    cli = argv[1];
    snprintf(tmpdir, sizeof tmpdir, "/tmp/egatool-test-XXXXXX");
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }
    test_palettes();
    test_dither_modes();
    test_extremes();
    test_midpoint_mix();
    test_limits();
    test_nes_pool();
    test_resize();
    test_adjust();
    test_files();
    test_cli_validation();
    test_recipe_replay();
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", tmpdir);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
