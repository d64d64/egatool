/*
 * EGAtool: hardware colour limits ("attribute clash").
 *
 * Old machines limit how many colours a small area of the screen may use:
 * the ZX Spectrum 2 per 8x8 cell, the C64 3 per cell plus a background
 * shared by all, the NES one of 4 small palettes per 16x16 area. A naive
 * conversion (each cell's most common colours, then dither) gives blocky
 * seams and wrong colours in busy cells. This works more like a pixel
 * artist:
 *
 * 1. Global choices first: the background colour every cell shares, and
 *    the NES's 4 palettes, grouped from what the areas would like best.
 * 2. Per cell, the colours that keep the picture readable: each candidate
 *    set is scored by how well dithering between its colours can match the
 *    cell, with brightness errors counting double (shapes before hue) and
 *    a cost for the noise of mixing distant colours (black + white for
 *    grey is a match, but a harsh one).
 * 3. Consistency: a cell pays for colours its neighbours don't have, and
 *    may take a neighbour's set outright, so sweeps over the image settle
 *    on matching sets across flat areas and gradients.
 * 4. Drawing: each pixel is dithered with its own cell's colours only, and
 *    error diffusion passes on only the error those colours can work off
 *    (no "add red" piling up in a cell that has no red).
 *
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine_int.h"

typedef struct {
    const char *name, *arg, *desc;
    int cell_w, cell_h; /* in the machine's pixels (a wide pixel counts once) */
    int wide;           /* 2: pixels are double width */
    int per_cell;       /* colours each cell picks */
    bool shared;        /* plus one background colour every cell can use */
    int subpalettes;    /* > 0: cells pick one of this many shared sets */
    bool bright_rule;   /* Spectrum: a cell's colours are all normal or all bright */
    EgtBuiltinPalette pal;
    int w, h;
} LimitDef;

static const LimitDef LIMITS[EGT_LIMIT_COUNT] = {
    { "None", "none", "Any colour anywhere",
      1, 1, 1, 0, false, 0, false, EGT_PAL_EGA, 320, 200 },
    { "ZX Spectrum", "spectrum", "2 colours per 8x8 cell,\nboth bright or both normal",
      8, 8, 1, 2, false, 0, true, EGT_PAL_SPECTRUM, 256, 192 },
    { "C64 hi-res", "c64", "2 colours per 8x8 cell",
      8, 8, 1, 2, false, 0, false, EGT_PAL_C64, 320, 200 },
    { "C64 multicolour", "c64-multi", "Double-wide pixels, 3 colours\nper 4x8 cell + 1 background",
      4, 8, 2, 3, true, 0, false, EGT_PAL_C64, 320, 200 },
    { "MSX", "msx", "2 colours per 8x1 line",
      8, 1, 1, 2, false, 0, false, EGT_PAL_MSX, 256, 192 },
    { "NES", "nes", "16x16 areas: 1 of 4 palettes\nof 3 colours + 1 background",
      16, 16, 1, 3, true, 4, false, EGT_PAL_NES, 256, 240 },
};

/* The rules for a limit; None for values out of range. */
static const LimitDef *limit_def(EgtLimit l)
{
    return &LIMITS[l > EGT_LIMIT_NONE && l < EGT_LIMIT_COUNT ? l : EGT_LIMIT_NONE];
}

const char *egt_limit_name(EgtLimit l) { return limit_def(l)->name; }
const char *egt_limit_arg(EgtLimit l) { return limit_def(l)->arg; }
const char *egt_limit_desc(EgtLimit l) { return limit_def(l)->desc; }

void egt_limit_machine(EgtLimit l, EgtBuiltinPalette *pal, int *width, int *height)
{
    const LimitDef *d = limit_def(l);
    if (pal)
        *pal = d->pal;
    if (width)
        *width = d->w;
    if (height)
        *height = d->h;
}

/* Brightness errors count this many times hue errors when choosing colours. */
#define L_WEIGHT 2.0f
/* Cost of a two-colour mix, per unit of its pattern's variance: mixing
 * distant colours matches the average but looks noisy. */
#define MIX_NOISE 0.02f
/* Colours tried per cell (sets are made of these). */
#define MAX_CAND 8
/* Fewer for the quick passes that pick the background colour. */
#define QUICK_CAND 6
#define MAX_SET 4
/* Palettes up to this size try every pair in 2-colour cells. */
#define ALL_PAIRS_MAX 16
#define MAX_OPT 136 /* C(16,2) pairs or C(8,3) sets, + singles */
/* Sets each cell keeps for the consistency sweeps (the rest fit too badly
 * to matter). */
#define KEEP_OPT 16
/* At consistency 100, a colour a neighbour lacks costs this much per pixel
 * of the cell (in weighted OKLab units squared, x255). */
#define CONSISTENCY_UNIT 120.0f
#define SWEEPS 4
#define BG_TRIES 5
/* NES: distinct favourite sets considered when grouping into palettes. */
#define NES_POOL 32
#define NES_ROUNDS 3

typedef struct {
    uint8_t c[MAX_SET];
    uint8_t n;
} CSet;

typedef struct {
    CSet set;
    float cost;
} Option;

typedef struct {
    const LimitDef *def;
    int lw, lh;      /* the image in machine pixels */
    int cw, ch;      /* cells across and down */
    float *rgb;      /* lw * lh * 3 */
    float *lab;      /* weighted OKLab, lw * lh * 3 */
    uint8_t *near1, *near2; /* nearest and second nearest colours per pixel */
    /* The nearest colours darker and lighter than each pixel: the
     * partners a dither between them would use (black and white for a
     * grey, though neither is nearest). */
    uint8_t *darker, *lighter;
    int n;
    float plab[EGT_MAX_COLORS][3];
    float prgb[EGT_MAX_COLORS][3];
    int8_t group[EGT_MAX_COLORS]; /* bright rule: 0 black, 1 normal, 2 bright */
    bool bright_rule;
    /* Each colour pair's dithered mixes, as the drawing makes them (sRGB
     * averages) in weighted OKLab: MIX_STEPS points from one to the
     * other. Mixing straight in OKLab would make red + black look far
     * darker than it does, and red + blue pass for brown. */
    float *mix;      /* npairs * MIX_STEPS * 3 */
    float *mix_noise; /* npairs: noise cost at a 50/50 mix */
} Ctx;

#define MIX_STEPS 3

/* Position of the colour pair (i, j), in either order, in a flat list of
 * all pairs. */
static int pair_index(const Ctx *c, int i, int j)
{
    if (i > j) {
        int t = i;
        i = j;
        j = t;
    }
    return i * c->n - i * (i + 1) / 2 + (j - i - 1);
}

/* sRGB level 0..255 to linear light 0..1. */
static float lin(float c)
{
    c /= 255.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* sRGB 0..255 to OKLab x 255, lightness scaled so squared distances count
 * it L_WEIGHT times. */
static void to_lab(float r, float g, float b, float *out)
{
    float lr = lin(r), lg = lin(g), lb = lin(b);
    float l = cbrtf(0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb);
    float m = cbrtf(0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb);
    float s = cbrtf(0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb);
    out[0] = (0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s) * 255.0f * sqrtf(L_WEIGHT);
    out[1] = (1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s) * 255.0f;
    out[2] = (0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s) * 255.0f;
}

static float dist2(const float *a, const float *b)
{
    float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return d0 * d0 + d1 * d1 + d2 * d2;
}

/* Distance from p to the segment a..b (squared) and where along it the
 * nearest point is (0..1). */
static float seg_dist(const float *p, const float *a, const float *b, float *t_out)
{
    float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
    float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = len2 > 0 ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2 : 0;
    float d0, d1, d2;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    d0 = ap[0] - t * ab[0];
    d1 = ap[1] - t * ab[1];
    d2 = ap[2] - t * ab[2];
    *t_out = t;
    return d0 * d0 + d1 * d1 + d2 * d2;
}

/* Cost of matching p (weighted OKLab) by dithering two colours: the
 * closest of their mixes plus the noise of that mix. The mixes form a
 * curve in OKLab, followed here as two straight pieces through the
 * 50/50 mix. */
static float pair_cost(const Ctx *c, const float *p, int pair)
{
    const float *m = c->mix + (size_t)pair * MIX_STEPS * 3;
    const float *m0 = m, *mid = m + (MIX_STEPS / 2) * 3, *m1 = m + (MIX_STEPS - 1) * 3;
    float t0, t1, d0 = seg_dist(p, m0, mid, &t0), d1 = seg_dist(p, mid, m1, &t1);
    float t = d0 <= d1 ? t0 / 2 : 0.5f + t1 / 2;
    return (d0 <= d1 ? d0 : d1) + c->mix_noise[pair] * 4 * t * (1 - t);
}

/* Pixel step for quick passes: about 32 pixels per cell. */
static int quick_sub(const Ctx *c)
{
    int sub = c->def->cell_w * c->def->cell_h / 32;
    return sub < 2 ? 2 : sub;
}

/* A cell's pixel rectangle, clipped at the image edge (the last row and
 * column of cells may be partial). */
static void cell_bounds(const Ctx *c, int cell, int *x0, int *y0, int *x1, int *y1)
{
    int cx = cell % c->cw, cy = cell / c->cw;
    *x0 = cx * c->def->cell_w;
    *y0 = cy * c->def->cell_h;
    *x1 = *x0 + c->def->cell_w < c->lw ? *x0 + c->def->cell_w : c->lw;
    *y1 = *y0 + c->def->cell_h < c->lh ? *y0 + c->def->cell_h : c->lh;
}

/* Scores options (adds to their costs) on the given cells: how well each
 * set can show them by dithering. The colours all the sets use are few
 * (a cell's candidates), so each pixel's cost per pair of them is worked
 * out once and shared by every set. sub > 1 samples every sub-th pixel
 * of each row, staggered between rows (quick passes). */
/* Colours the options of one call may use between them: every candidate
 * plus the background (callers stay within this). */
#define MAX_LOCAL (ALL_PAIRS_MAX + 1)
static void score_options(const Ctx *c, const int *cells, int ncells, int sub,
                          Option *opts, int nopt)
{
    int local[EGT_MAX_COLORS], cols[MAX_LOCAL], ncols = 0, o, i, j, k;
    /* per option: its pairs (or its single colour) as local indices */
    uint16_t lp[MAX_OPT][MAX_SET * (MAX_SET - 1) / 2];
    uint8_t nlp[MAX_OPT];
    /* the pairs any option uses, as (a, b) local indices, and their pair numbers */
    uint8_t need[MAX_LOCAL * MAX_LOCAL] = {0};
    int npair = 0, pair_ab[MAX_LOCAL * MAX_LOCAL], pair_id[MAX_LOCAL * MAX_LOCAL];
    for (i = 0; i < c->n; i++)
        local[i] = -1;
    for (o = 0; o < nopt; o++)
        for (i = 0; i < opts[o].set.n; i++) {
            int col = opts[o].set.c[i];
            if (local[col] < 0 && ncols < MAX_LOCAL) {
                local[col] = ncols;
                cols[ncols++] = col;
            }
        }
    for (o = 0; o < nopt; o++) {
        const CSet *s = &opts[o].set;
        nlp[o] = 0;
        if (s->n == 1)
            continue;
        for (i = 0; i < s->n; i++)
            for (j = i + 1; j < s->n; j++) {
                int a = local[s->c[i]], b = local[s->c[j]];
                int ab = a < b ? a * MAX_LOCAL + b : b * MAX_LOCAL + a;
                lp[o][nlp[o]++] = (uint16_t)ab;
                if (!need[ab]) {
                    need[ab] = 1;
                    pair_ab[npair] = ab;
                    pair_id[npair++] = pair_index(c, cols[ab / MAX_LOCAL], cols[ab % MAX_LOCAL]);
                }
            }
    }
    for (k = 0; k < ncells; k++) {
        int x0, y0, x1, y1, x, y, cx = cells[k] % c->cw, cy = cells[k] / c->cw;
        x0 = cx * c->def->cell_w;
        y0 = cy * c->def->cell_h;
        x1 = x0 + c->def->cell_w < c->lw ? x0 + c->def->cell_w : c->lw;
        y1 = y0 + c->def->cell_h < c->lh ? y0 + c->def->cell_h : c->lh;
        for (y = y0; y < y1; y++) {
            for (x = x0 + (sub > 1 ? y % sub : 0); x < x1; x += sub) {
                const float *p = c->lab + ((size_t)y * c->lw + x) * 3;
                float pc[MAX_LOCAL * MAX_LOCAL], sc[MAX_LOCAL];
                for (i = 0; i < ncols; i++)
                    sc[i] = dist2(p, c->plab[cols[i]]);
                for (i = 0; i < npair; i++)
                    pc[pair_ab[i]] = pair_cost(c, p, pair_id[i]);
                for (o = 0; o < nopt; o++) {
                    float best;
                    if (nlp[o] == 0) {
                        best = sc[local[opts[o].set.c[0]]];
                    } else {
                        best = pc[lp[o][0]];
                        for (i = 1; i < nlp[o]; i++)
                            if (pc[lp[o][i]] < best)
                                best = pc[lp[o][i]];
                    }
                    opts[o].cost += best;
                }
            }
        }
    }
}

/* How well set s can show the cell. */
static float cell_cost(const Ctx *c, int cell, const CSet *s, int sub)
{
    Option o;
    o.set = *s;
    o.cost = 0;
    score_options(c, &cell, 1, sub, &o, 1);
    return o.cost;
}

/* Colour votes of a cell: 2 for each pixel's nearest colour, 1 for its
 * second nearest and its darker and lighter dither partners. */
static void add_votes(const Ctx *c, int cell, float *votes)
{
    int x0, y0, x1, y1, x, y;
    cell_bounds(c, cell, &x0, &y0, &x1, &y1);
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            size_t i = (size_t)y * c->lw + x;
            votes[c->near1[i]] += 2;
            votes[c->near2[i]] += 1;
            votes[c->darker[i]] += 1;
            votes[c->lighter[i]] += 1;
        }
}

/* The (up to max) best voted colours, without exclude. */
static int top_colours(const Ctx *c, const float *votes, int exclude, int max, int *out)
{
    bool taken[EGT_MAX_COLORS] = {false};
    int m, i;
    for (m = 0; m < max; m++) {
        int best = -1;
        for (i = 0; i < c->n; i++)
            if (i != exclude && !taken[i] && votes[i] > 0 && (best < 0 || votes[i] > votes[best]))
                best = i;
        if (best < 0)
            break;
        taken[best] = true;
        out[m] = best;
    }
    return m;
}

/* Spectrum: a set may not mix normal and bright colours (black is both). */
static bool bright_ok(const Ctx *c, const CSet *s)
{
    int i, g = 0;
    if (!c->bright_rule)
        return true;
    for (i = 0; i < s->n; i++) {
        int gi = c->group[s->c[i]];
        if (gi && g && gi != g)
            return false;
        if (gi)
            g = gi;
    }
    return true;
}

/* Every set of per_cell colours from cand (fewer if there are fewer
 * candidates), plus bg if >= 0, scored on cells. Returns the count. */
static int make_options(const Ctx *c, const int *cells, int ncells, const int *cand, int m,
                        int bg, int sub, Option *out)
{
    int k = c->def->per_cell < m ? c->def->per_cell : m;
    int idx[MAX_SET], n = 0, i;
    if (m == 0) { /* nothing but the background (or a 1-colour palette) */
        out[0].set.n = 1;
        out[0].set.c[0] = (uint8_t)(bg >= 0 ? bg : 0);
        out[0].cost = 0;
        score_options(c, cells, ncells, sub, out, 1);
        return 1;
    }
    for (i = 0; i < k; i++)
        idx[i] = i;
    for (;;) {
        Option *o = &out[n];
        o->set.n = 0;
        for (i = 0; i < k && i < m; i++)
            o->set.c[o->set.n++] = (uint8_t)cand[idx[i]];
        if (bg >= 0)
            o->set.c[o->set.n++] = (uint8_t)bg;
        if (o->set.n > 0 && bright_ok(c, &o->set) && n < MAX_OPT - ALL_PAIRS_MAX) {
            o->cost = 0;
            n++;
        }
        /* next combination */
        for (i = k - 1; i >= 0 && idx[i] == m - k + i; i--)
            ;
        if (i < 0)
            break;
        idx[i]++;
        for (i++; i < k; i++)
            idx[i] = idx[i - 1] + 1;
    }
    if (n == 0) { /* every set broke the bright rule: single colours */
        for (i = 0; i < m; i++) {
            Option *o = &out[n];
            o->set.n = 0;
            o->set.c[o->set.n++] = (uint8_t)cand[i];
            if (bg >= 0 && bg != cand[i] && c->group[bg] == 0)
                o->set.c[o->set.n++] = (uint8_t)bg;
            o->cost = 0;
            n++;
        }
        if (n == 0 && bg >= 0) {
            out[0].set.n = 1;
            out[0].set.c[0] = (uint8_t)bg;
            out[0].cost = 0;
            n = 1;
        }
    }
    score_options(c, cells, ncells, sub, out, n);
    return n;
}

/* Keeps the keep cheapest options (in any order). Returns the count. */
static int prune_options(Option *o, int n, int keep)
{
    int i, j;
    if (n <= keep)
        return n;
    for (i = 0; i < keep; i++) {
        int best = i;
        for (j = i + 1; j < n; j++)
            if (o[j].cost < o[best].cost)
                best = j;
        if (best != i) {
            Option t = o[i];
            o[i] = o[best];
            o[best] = t;
        }
    }
    return keep;
}

/* Candidate colour sets for one cell, scored. Two-colour cells with small
 * palettes try every pair (on a sample of pixels, then the best few on
 * all); otherwise sets are built from the colours the cell's pixels want
 * most. */
static int cell_options(const Ctx *c, int cell, int bg, int max_cand, int sub, Option *out)
{
    float votes[EGT_MAX_COLORS] = {0};
    int cand[ALL_PAIRS_MAX];
    int m = 0, i;
    if (c->def->per_cell == 2 && c->n - (bg >= 0) <= ALL_PAIRS_MAX) {
        int n;
        for (i = 0; i < c->n; i++)
            if (i != bg)
                cand[m++] = i;
        /* Every pair on a sample of the pixels, then the best few on all. */
        n = make_options(c, &cell, 1, cand, m, bg, sub * 2, out);
        n = prune_options(out, n, KEEP_OPT);
        for (i = 0; i < n; i++)
            out[i].cost = 0;
        score_options(c, &cell, 1, sub, out, n);
        return n;
    }
    add_votes(c, cell, votes);
    m = top_colours(c, votes, bg, max_cand, cand);
    return prune_options(out, make_options(c, &cell, 1, cand, m, bg, sub, out), KEEP_OPT);
}

/* Index of the cheapest option. */
static int best_option(const Option *o, int n)
{
    int i, b = 0;
    for (i = 1; i < n; i++)
        if (o[i].cost < o[b].cost)
            b = i;
    return b;
}

static bool set_has(const CSet *s, int colour)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (s->c[i] == colour)
            return true;
    return false;
}

/* Colours of a that b lacks. */
static int mismatch(const CSet *a, const CSet *b)
{
    int i, m = 0;
    for (i = 0; i < a->n; i++)
        m += !set_has(b, a->c[i]);
    return m;
}

static bool set_equal(const CSet *a, const CSet *b)
{
    return a->n == b->n && mismatch(a, b) == 0;
}

/* The shared background: of the most common colours, the one that leaves
 * the cells best off. */
static int choose_background(const Ctx *c)
{
    Option scratch[MAX_OPT];
    float votes[EGT_MAX_COLORS] = {0};
    int cand[BG_TRIES], m, i, cell, ncells = c->cw * c->ch, best = 0;
    double best_total = 1e300;
    size_t p, npix = (size_t)c->lw * c->lh;
    for (p = 0; p < npix; p++)
        votes[c->near1[p]] += 1;
    m = top_colours(c, votes, -1, BG_TRIES, cand);
    if (m == 0)
        return 0;
    for (i = 0; i < m; i++) {
        double total = 0;
        for (cell = 0; cell < ncells && total < best_total; cell++) {
            int n = cell_options(c, cell, cand[i], QUICK_CAND, quick_sub(c), scratch);
            total += scratch[best_option(scratch, n)].cost;
        }
        if (total < best_total) {
            best_total = total;
            best = cand[i];
        }
    }
    return best;
}

/* NES-style: groups the areas' favourite sets into def->subpalettes
 * palettes. Fills pals; returns how many (fewer if the image needs fewer). */
static int choose_subpalettes(const Ctx *c, int bg, CSet *pals, bool *ok)
{
    Option scratch[MAX_OPT];
    int ncells = c->cw * c->ch, npool = 0, np = 0, i, j, round, cell;
    CSet pool[NES_POOL];
    int pool_count[NES_POOL];
    float *cost = NULL, *cur = NULL;
    int *assign = NULL, *member = NULL;

    /* Each area's own favourite, counted. */
    for (cell = 0; cell < ncells; cell++) {
        int n = cell_options(c, cell, bg, MAX_CAND, quick_sub(c), scratch);
        CSet *fav = &scratch[best_option(scratch, n)].set;
        for (j = 0; j < npool && !set_equal(&pool[j], fav); j++)
            ;
        if (j < npool) {
            pool_count[j]++;
        } else if (npool < NES_POOL) {
            pool[npool] = *fav;
            pool_count[npool++] = 1;
        }
    }
    cost = malloc((size_t)ncells * npool * sizeof *cost);
    cur = malloc((size_t)ncells * sizeof *cur);
    assign = malloc((size_t)ncells * sizeof *assign);
    member = malloc((size_t)ncells * sizeof *member);
    if (!cost || !cur || !assign || !member) {
        *ok = false;
        goto done;
    }
    for (cell = 0; cell < ncells; cell++)
        for (j = 0; j < npool; j++)
            cost[(size_t)cell * npool + j] = cell_cost(c, cell, &pool[j], quick_sub(c));

    /* Greedy: add the favourite that helps the whole picture most. */
    for (cell = 0; cell < ncells; cell++)
        cur[cell] = 1e30f;
    while (np < c->def->subpalettes && np < npool) {
        int pick = -1;
        double pick_total = 1e300;
        for (j = 0; j < npool; j++) {
            double total = 0;
            for (i = 0; i < np && !set_equal(&pals[i], &pool[j]); i++)
                ;
            if (i < np)
                continue;
            for (cell = 0; cell < ncells; cell++) {
                float d = cost[(size_t)cell * npool + j];
                total += d < cur[cell] ? d : cur[cell];
            }
            if (total < pick_total) {
                pick_total = total;
                pick = j;
            }
        }
        if (pick < 0)
            break;
        pals[np++] = pool[pick];
        for (cell = 0; cell < ncells; cell++) {
            float d = cost[(size_t)cell * npool + pick];
            if (d < cur[cell])
                cur[cell] = d;
        }
    }

    /* Refine: assign areas, then re-pick each palette for its own areas. */
    for (round = 0; round < NES_ROUNDS; round++) {
        int p;
        for (cell = 0; cell < ncells; cell++) {
            float best = 1e30f;
            for (p = 0; p < np; p++) {
                float d = cell_cost(c, cell, &pals[p], quick_sub(c));
                if (d < best) {
                    best = d;
                    assign[cell] = p;
                }
            }
        }
        for (p = 0; p < np; p++) {
            float votes[EGT_MAX_COLORS] = {0};
            int cand[MAX_CAND], m, nopt, members = 0;
            for (cell = 0; cell < ncells; cell++)
                if (assign[cell] == p) {
                    add_votes(c, cell, votes);
                    member[members++] = cell;
                }
            if (!members)
                continue;
            m = top_colours(c, votes, bg, MAX_CAND, cand);
            nopt = make_options(c, member, members, cand, m, bg, quick_sub(c), scratch);
            pals[p] = scratch[best_option(scratch, nopt)].set;
        }
    }
done:
    free(cost);
    free(cur);
    free(assign);
    free(member);
    return np;
}

/* ---- drawing -------------------------------------------------------------- */

static int nearest_in(const Ctx *c, const CSet *s, const float *v, float *d_out)
{
    int i, best = s->c[0];
    float bd = 1e30f;
    for (i = 0; i < s->n; i++) {
        float d = dist2(v, c->prgb[s->c[i]]);
        if (d < bd) {
            bd = d;
            best = s->c[i];
        }
    }
    if (d_out)
        *d_out = bd;
    return best;
}

/* The segment between two of the set's colours that comes closest to v
 * (in RGB): its ends, where v falls along it (0..1) and that point. */
static void best_segment(const Ctx *c, const CSet *s, const float *v, int *a, int *b, float *t,
                         float *q)
{
    int i, j;
    float bd = 1e30f;
    *a = *b = s->c[0];
    *t = 0;
    memcpy(q, c->prgb[s->c[0]], 3 * sizeof(float));
    for (i = 0; i < s->n; i++)
        for (j = i + 1; j < s->n; j++) {
            const float *pa = c->prgb[s->c[i]], *pb = c->prgb[s->c[j]];
            float ab[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
            float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
            float tt = len2 > 0 ? ((v[0] - pa[0]) * ab[0] + (v[1] - pa[1]) * ab[1] +
                                   (v[2] - pa[2]) * ab[2]) / len2 : 0;
            float p[3], d;
            tt = tt < 0 ? 0 : tt > 1 ? 1 : tt;
            p[0] = pa[0] + tt * ab[0];
            p[1] = pa[1] + tt * ab[1];
            p[2] = pa[2] + tt * ab[2];
            d = dist2(v, p);
            if (d < bd) {
                bd = d;
                *a = s->c[i];
                *b = s->c[j];
                *t = tt;
                memcpy(q, p, sizeof p);
            }
        }
}

/* Snap, as in the unlimited modes: 0 = solid (within snap levels of one of
 * the cell's colours), 1 = full dithering (from twice that). */
static float snap_amount(float d2, int snap)
{
    float d;
    if (snap <= 0)
        return 1;
    d = sqrtf(d2);
    if (d <= snap)
        return 0;
    if (d >= 2.0f * snap)
        return 1;
    return (d - snap) / snap;
}

/* Power mean of two levels: g = 1 averages the values; higher g leans
 * towards the brighter one, as averaging emitted light does. */
static float gamma_mix_f(float a, float b, double g)
{
    double m = (pow(a / 255.0, g) + pow(b / 255.0, g)) / 2;
    return (float)(255.0 * pow(m, 1.0 / g));
}

/* Draws the result: each pixel dithered with its own cell's colours only.
 * Error diffusion skips neighbours in cells with a different set, so no
 * seams form along cell edges. */
static void draw(const Ctx *c, const CSet *sets, const EgtConvertOptions *opt,
                 const Kernel *kernel, uint8_t *out, float *err)
{
    static const int BAYER4[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5},
    };
    int w = c->lw, h = c->lh, x, y, t;
    size_t stride = (size_t)(w + 4) * 3;
    double gamma = 1.0;
    if (opt->light_mix > 0) {
        double lm = (opt->light_mix > 100 ? 100 : opt->light_mix) / 100.0;
        gamma = 1.0 + 1.2 * lm * lm;
    }
    for (y = 0; y < h; y++) {
        float *row = err ? err + (size_t)(y % 3) * stride : NULL;
        for (x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            const float *s = c->rgb + i * 3;
            const CSet *set = &sets[(y / c->def->cell_h) * c->cw + x / c->def->cell_w];
            float d2, amount;
            int k = nearest_in(c, set, s, &d2);

            if (opt->dither == EGT_DITHER_NONE || set->n == 1) {
                out[i] = (uint8_t)k;
            } else if (opt->dither == EGT_DITHER_EGA_CHECKER) {
                int a, b, best_a = k, best_b = k;
                float best_d = 1e30f;
                for (a = 0; a < set->n; a++)
                    for (b = a + 1; b < set->n; b++) {
                        const float *pa = c->prgb[set->c[a]], *pb = c->prgb[set->c[b]];
                        float m[3], d;
                        if (gamma != 1.0) {
                            m[0] = gamma_mix_f(pa[0], pb[0], gamma);
                            m[1] = gamma_mix_f(pa[1], pb[1], gamma);
                            m[2] = gamma_mix_f(pa[2], pb[2], gamma);
                        } else {
                            m[0] = (pa[0] + pb[0]) / 2;
                            m[1] = (pa[1] + pb[1]) / 2;
                            m[2] = (pa[2] + pb[2]) / 2;
                        }
                        d = dist2(s, m);
                        if (d < best_d) {
                            best_d = d;
                            best_a = set->c[a];
                            best_b = set->c[b];
                        }
                    }
                out[i] = (uint8_t)((best_a == k && best_b == k) ||
                                   egt__keep_solid(sqrtf(d2), sqrtf(best_d),
                                                   opt->prefer_solid)
                                   ? k : ((x ^ y) & 1) ? best_b : best_a);
            } else if (!kernel) { /* ordered */
                int a, b;
                float tt, q[3], thr = (BAYER4[y & 3][x & 3] + 0.5f) / 16.0f;
                amount = snap_amount(d2, opt->snap);
                if (amount <= 0) {
                    out[i] = (uint8_t)k;
                } else {
                    best_segment(c, set, s, &a, &b, &tt, q);
                    tt = tt < 0.5f ? tt * amount : 1 - (1 - tt) * amount;
                    out[i] = (uint8_t)(tt > thr ? b : a);
                }
            } else { /* error diffusion */
                float *e = row + (size_t)(x + 2) * 3, v[3], q[3], tt;
                int a, b, ci;
                amount = snap_amount(d2, opt->snap);
                if (amount <= 0) {
                    out[i] = (uint8_t)k; /* solid; arriving error is dropped */
                    continue;
                }
                for (t = 0; t < 3; t++) {
                    v[t] = s[t] + e[t] / kernel->divisor * amount;
                    v[t] = v[t] < 0 ? 0 : v[t] > 255 ? 255 : v[t];
                }
                ci = nearest_in(c, set, v, NULL);
                out[i] = (uint8_t)ci;
                /* Only the part of v the cell's colours can reach carries on. */
                best_segment(c, set, v, &a, &b, &tt, q);
                {
                    float er = (q[0] - c->prgb[ci][0]) * amount;
                    float eg = (q[1] - c->prgb[ci][1]) * amount;
                    float eb = (q[2] - c->prgb[ci][2]) * amount;
                    for (t = 0; t < kernel->ntaps; t++) {
                        const KernelTap *tap = &kernel->taps[t];
                        int tx = x + tap->dx, ty = y + tap->dy;
                        float *te;
                        /* Error stays out of cells with other colours: there
                         * it would pile up along the cell edge as a line. */
                        if (tx >= 0 && tx < w && ty < h &&
                            !set_equal(set, &sets[(ty / c->def->cell_h) * c->cw +
                                                  tx / c->def->cell_w]))
                            continue;
                        te = err + (size_t)(ty % 3) * stride + (size_t)(tx + 2) * 3;
                        te[0] += er * tap->w;
                        te[1] += eg * tap->w;
                        te[2] += eb * tap->w;
                    }
                }
            }
        }
        if (row)
            memset(row, 0, stride * sizeof(float));
    }
}

/* ---- setup ---------------------------------------------------------------- */

/* Whether the palette is built like the Spectrum's (every colour is black
 * or channels at 0 or one of exactly two levels), and each colour's
 * brightness group. Only then does the bright rule apply. */
static bool classify_bright(Ctx *c, const EgtPalette *pal)
{
    int levels[2] = {0, 0}, nl = 0, i;
    for (i = 0; i < pal->count; i++) {
        const EgtColor *p = &pal->colors[i];
        int mx = p->r > p->g ? p->r : p->g;
        mx = mx > p->b ? mx : p->b;
        if ((p->r && p->r != mx) || (p->g && p->g != mx) || (p->b && p->b != mx))
            return false;
        if (mx && mx != levels[0] && mx != levels[1]) {
            if (nl == 2)
                return false;
            levels[nl++] = mx;
        }
    }
    if (nl != 2)
        return false;
    for (i = 0; i < pal->count; i++) {
        const EgtColor *p = &pal->colors[i];
        int mx = p->r > p->g ? p->r : p->g;
        mx = mx > p->b ? mx : p->b;
        c->group[i] = (int8_t)(mx == 0 ? 0 : mx == (levels[0] < levels[1] ? levels[0] : levels[1]) ? 1 : 2);
    }
    return true;
}

/* Converts with a hardware limit: global choices, per-cell sets, sweeps
 * for consistency, then drawing (see the comment at the top). */
bool egt__convert_limited(const EgtImage *src, EgtImage *dst, const EgtPalette *pal,
                          const EgtConvertOptions *opt, const Kernel *kernel)
{
    Ctx c;
    const LimitDef *def = limit_def(opt->limit);
    int ncells, cell, sweep, i, x, y, bg = -1;
    size_t npix;
    Option *opts = NULL;
    int *nopts = NULL;
    CSet *sets = NULL;
    uint8_t *out = NULL;
    float *err = NULL;
    bool ok = false;

    memset(&c, 0, sizeof c);
    c.def = def;
    c.lw = (src->width + def->wide - 1) / def->wide;
    c.lh = src->height;
    c.cw = (c.lw + def->cell_w - 1) / def->cell_w;
    c.ch = (c.lh + def->cell_h - 1) / def->cell_h;
    c.n = pal->count;
    ncells = c.cw * c.ch;
    npix = (size_t)c.lw * c.lh;
    for (i = 0; i < c.n; i++) {
        c.prgb[i][0] = pal->colors[i].r;
        c.prgb[i][1] = pal->colors[i].g;
        c.prgb[i][2] = pal->colors[i].b;
        to_lab(c.prgb[i][0], c.prgb[i][1], c.prgb[i][2], c.plab[i]);
    }
    {
        int a, b, k, npairs = c.n * (c.n - 1) / 2;
        c.mix = malloc(((size_t)npairs * MIX_STEPS * 3 + 1) * sizeof(float));
        c.mix_noise = malloc(((size_t)npairs + 1) * sizeof(float));
        if (!c.mix || !c.mix_noise)
            goto done;
        for (a = 0; a < c.n; a++)
            for (b = a + 1; b < c.n; b++) {
                int pi = pair_index(&c, a, b);
                float *m = c.mix + (size_t)pi * MIX_STEPS * 3;
                for (k = 0; k < MIX_STEPS; k++) {
                    float t = (float)k / (MIX_STEPS - 1);
                    to_lab(c.prgb[a][0] + t * (c.prgb[b][0] - c.prgb[a][0]),
                           c.prgb[a][1] + t * (c.prgb[b][1] - c.prgb[a][1]),
                           c.prgb[a][2] + t * (c.prgb[b][2] - c.prgb[a][2]), m + k * 3);
                }
                /* 4t(1-t) scaled: at 50/50 the pattern's variance is
                 * a quarter of the colours' squared distance. */
                c.mix_noise[pi] = MIX_NOISE * 0.25f * dist2(c.plab[a], c.plab[b]);
            }
    }
    c.bright_rule = def->bright_rule && classify_bright(&c, pal);
    if (!c.bright_rule)
        memset(c.group, 0, sizeof c.group);

    c.rgb = malloc(npix * 3 * sizeof(float));
    c.lab = malloc(npix * 3 * sizeof(float));
    c.near1 = malloc(npix);
    c.near2 = malloc(npix);
    c.darker = malloc(npix);
    c.lighter = malloc(npix);
    opts = malloc((size_t)ncells * MAX_OPT * sizeof *opts);
    nopts = malloc((size_t)ncells * sizeof *nopts);
    sets = malloc((size_t)ncells * sizeof *sets);
    out = malloc(npix);
    if (kernel)
        err = calloc((size_t)(c.lw + 4) * 3 * 3, sizeof(float));
    if (!c.rgb || !c.lab || !c.near1 || !c.near2 || !c.darker || !c.lighter || !opts || !nopts || !sets || !out ||
        (kernel && !err))
        goto done;

    /* The image in machine pixels (wide ones average their two). */
    for (y = 0; y < c.lh; y++)
        for (x = 0; x < c.lw; x++) {
            const uint8_t *p = src->pixels + ((size_t)y * src->width + (size_t)x * def->wide) * 4;
            float *q = c.rgb + ((size_t)y * c.lw + x) * 3;
            int t;
            if (def->wide == 2 && x * 2 + 1 < src->width)
                for (t = 0; t < 3; t++)
                    q[t] = (p[t] + p[4 + t]) / 2.0f;
            else
                for (t = 0; t < 3; t++)
                    q[t] = p[t];
        }
    for (npix = 0; npix < (size_t)c.lw * c.lh; npix++) {
        float *l = c.lab + npix * 3, d1 = 1e30f, d2 = 1e30f, dd = 1e30f, dl = 1e30f;
        int k, n1 = 0, n2 = 0, nd = -1, nl = -1;
        to_lab(c.rgb[npix * 3], c.rgb[npix * 3 + 1], c.rgb[npix * 3 + 2], l);
        for (k = 0; k < c.n; k++) {
            float d = dist2(l, c.plab[k]);
            if (c.plab[k][0] <= l[0] && d < dd) {
                dd = d;
                nd = k;
            }
            if (c.plab[k][0] >= l[0] && d < dl) {
                dl = d;
                nl = k;
            }
            if (d < d1) {
                d2 = d1;
                n2 = n1;
                d1 = d;
                n1 = k;
            } else if (d < d2) {
                d2 = d;
                n2 = k;
            }
        }
        c.near1[npix] = (uint8_t)n1;
        c.near2[npix] = (uint8_t)(c.n > 1 ? n2 : n1);
        c.darker[npix] = (uint8_t)(nd >= 0 ? nd : n1);
        c.lighter[npix] = (uint8_t)(nl >= 0 ? nl : n1);
    }

    /* 1. Global choices. */
    if (def->shared)
        bg = choose_background(&c);
    if (def->subpalettes > 0) {
        CSet pals[MAX_SET];
        bool sub_ok = true;
        int np = choose_subpalettes(&c, bg, pals, &sub_ok);
        if (!sub_ok)
            goto done;
        for (cell = 0; cell < ncells; cell++) {
            Option *o = opts + (size_t)cell * MAX_OPT;
            for (i = 0; i < np; i++) {
                o[i].set = pals[i];
                o[i].cost = cell_cost(&c, cell, &pals[i], 1);
            }
            nopts[cell] = np;
        }
    } else {
        /* 2. Each cell's own candidate sets. */
        for (cell = 0; cell < ncells; cell++)
            nopts[cell] = cell_options(&c, cell, bg, MAX_CAND, 1, opts + (size_t)cell * MAX_OPT);
    }
    for (cell = 0; cell < ncells; cell++) {
        Option *o = opts + (size_t)cell * MAX_OPT;
        sets[cell] = o[best_option(o, nopts[cell])].set;
    }

    /* 3. Consistency sweeps: each cell re-chooses given its neighbours,
     * among its own sets and its neighbours' sets. */
    if (opt->consistency > 0) {
        float lambda = opt->consistency / 100.0f * def->cell_w * def->cell_h * CONSISTENCY_UNIT;
        for (sweep = 0; sweep < SWEEPS; sweep++) {
            bool changed = false;
            for (cell = 0; cell < ncells; cell++) {
                int cx = cell % c.cw, cy = cell / c.cw, nb[4], nnb = 0, k;
                Option cand[MAX_OPT + 4];
                int ncand = nopts[cell];
                float best = 1e30f;
                CSet pick = sets[cell];
                if (cx > 0) nb[nnb++] = cell - 1;
                if (cx < c.cw - 1) nb[nnb++] = cell + 1;
                if (cy > 0) nb[nnb++] = cell - c.cw;
                if (cy < c.ch - 1) nb[nnb++] = cell + c.cw;
                memcpy(cand, opts + (size_t)cell * MAX_OPT, (size_t)ncand * sizeof *cand);
                if (def->subpalettes == 0)
                    for (k = 0; k < nnb; k++) {
                        cand[ncand].set = sets[nb[k]];
                        cand[ncand].cost = cell_cost(&c, cell, &sets[nb[k]], 1);
                        ncand++;
                    }
                for (i = 0; i < ncand; i++) {
                    float total = cand[i].cost;
                    for (k = 0; k < nnb; k++)
                        total += lambda * mismatch(&cand[i].set, &sets[nb[k]]);
                    if (total < best) {
                        best = total;
                        pick = cand[i].set;
                    }
                }
                if (!set_equal(&pick, &sets[cell])) {
                    sets[cell] = pick;
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
    }

    /* 4. Draw, then widen back to the output. */
    draw(&c, sets, opt, kernel, out, err);
    for (y = 0; y < src->height; y++)
        for (x = 0; x < src->width; x++) {
            const EgtColor *col = &pal->colors[out[(size_t)y * c.lw + x / def->wide]];
            uint8_t *d = dst->pixels + ((size_t)y * dst->width + x) * 4;
            d[0] = col->r;
            d[1] = col->g;
            d[2] = col->b;
            d[3] = 255;
        }
    ok = true;
done:
    free(c.mix);
    free(c.mix_noise);
    free(c.rgb);
    free(c.lab);
    free(c.near1);
    free(c.near2);
    free(c.darker);
    free(c.lighter);
    free(opts);
    free(nopts);
    free(sets);
    free(out);
    free(err);
    return ok;
}
