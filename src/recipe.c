/*
 * The egatool-cli command line language: reads its options and writes
 * settings back as a command line (stored in saved PNGs). Every option
 * name is here, so reading and writing can't disagree.
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#include "recipe.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PALETTE_ARGS[EGT_PAL_COUNT] = {
    "ega", "vga", "cga", "cga-warm", "ega64", "cpc", "c64",
    "spectrum", "msx", "apple2", "gameboy", "nes", "db32", "pico8",
};

static const char *DITHER_ARGS[EGT_DITHER_COUNT] = {
    "checker", "ordered", "ordered2", "burkes", "floyd", "sierra", "stucki", "none",
};

static const char *LIMIT_ARGS[EGT_LIMIT_COUNT] = {
    "none", "spectrum", "c64", "c64-multi", "msx", "nes",
};

static const char *SMOOTH_ARGS[EGT_SMOOTH_COUNT] = { "none", "median", "kuwahara", "surface" };

static const char *OUTLINE_ARGS[EGT_OUTLINE_COUNT] = { "basic", "hires" };

const char *egt_builtin_palette_arg(EgtBuiltinPalette p)
{
    return (p >= 0 && p < EGT_PAL_COUNT) ? PALETTE_ARGS[p] : "ega";
}

const char *egt_dither_arg(EgtDither d)
{
    return (d >= 0 && d < EGT_DITHER_COUNT) ? DITHER_ARGS[d] : "?";
}

const char *egt_limit_arg(EgtLimit l)
{
    return (l >= 0 && l < EGT_LIMIT_COUNT) ? LIMIT_ARGS[l] : "?";
}

/* Position of word in names, or -1. */
static int find(const char *const *names, int n, const char *word)
{
    int k;
    for (k = 0; k < n; k++)
        if (!strcmp(names[k], word))
            return k;
    return -1;
}

/* The options that set one number, pointing into s. Reading and writing
 * both use this list. */
enum { NUM_OPTS = 15 };
typedef struct {
    const char *name;
    int *i;
    float *f;
} NumOpt;

static void num_opts(EgtSettings *s, NumOpt o[NUM_OPTS])
{
    const NumOpt list[NUM_OPTS] = {
        { "--prefer-solid", &s->opt.prefer_solid, NULL },
        { "--light-mix", &s->opt.light_mix, NULL },
        { "--harsh-mix", &s->opt.harsh_mix, NULL },
        { "--snap", &s->opt.snap, NULL },
        { "--consistency", &s->opt.consistency, NULL },
        { "--brightness", &s->adj.brightness, NULL },
        { "--contrast", &s->adj.contrast, NULL },
        { "--saturation", NULL, &s->adj.saturation },
        { "--calibrate-max", &s->calibrate_max, NULL },
        { "--radius", NULL, &s->filt.smooth_radius },
        { "--threshold", &s->filt.smooth_threshold, NULL },
        { "--outline", &s->filt.outline, NULL },
        { "--outline-dark", NULL, &s->filt.outline_darkness },
        { "--outline-min", &s->filt.outline_min_length, NULL },
        { "--min-region", &s->opt.min_region, NULL },
    };
    memcpy(o, list, sizeof list);
}

/* Strict number parsing: the whole word must be a number (no "5x", no
 * "rubbish" read as 0), within lo..hi. */
static bool parse_int(const char *opt, const char *text, long lo, long hi, int *out)
{
    char *end;
    long v;
    errno = 0;
    v = strtol(text, &end, 10);
    if (end == text || *end || errno || v < lo || v > hi) {
        fprintf(stderr, "%s: '%s' is not a whole number in %ld..%ld\n", opt, text, lo, hi);
        return false;
    }
    *out = (int)v;
    return true;
}

static bool parse_float(const char *opt, const char *text, float *out)
{
    char *end;
    double v;
    errno = 0;
    v = strtod(text, &end);
    if (end == text || *end || errno || !isfinite(v) || fabs(v) > 1e9) {
        fprintf(stderr, "%s: '%s' is not a number\n", opt, text);
        return false;
    }
    *out = (float)v;
    return true;
}

/* Reads a choice ("-d sierra") into *out; false if value isn't one. */
static bool parse_choice(const char *opt, const char *value, const char *const *names, int n,
                         int *out)
{
    *out = find(names, n, value);
    if (*out < 0)
        fprintf(stderr, "%s: unknown value '%s'\n", opt, value);
    return *out >= 0;
}

int egt_recipe_option(EgtRecipe *r, const char *opt, const char *value)
{
    NumOpt nums[NUM_OPTS];
    int k, v;
    if (!strcmp(opt, "--nearest")) {
        r->set.nearest = true;
        return 1;
    }
    if (!strcmp(opt, "--calibrate")) {
        r->set.calibrate = true;
        return 1;
    }
    if (!value)
        return 0;
    num_opts(&r->set, nums);
    for (k = 0; k < NUM_OPTS; k++)
        if (!strcmp(opt, nums[k].name)) {
            /* Ranges are checked with the rest (egt_settings_check). */
            bool ok = nums[k].i ? parse_int(opt, value, INT_MIN, INT_MAX, nums[k].i)
                                : parse_float(opt, value, nums[k].f);
            return ok ? 2 : -1;
        }
    if (!strcmp(opt, "-p") || !strcmp(opt, "--palette")) {
        r->palette = value;
    } else if (!strcmp(opt, "-x") || !strcmp(opt, "--scale")) {
        if (!parse_int(opt, value, 1, 8, &r->scale))
            return -1;
    } else if (!strcmp(opt, "-d") || !strcmp(opt, "--dither")) {
        if (!parse_choice(opt, value, DITHER_ARGS, EGT_DITHER_COUNT, &v))
            return -1;
        r->set.opt.dither = (EgtDither)v;
    } else if (!strcmp(opt, "--limit")) {
        if (!parse_choice(opt, value, LIMIT_ARGS, EGT_LIMIT_COUNT, &v))
            return -1;
        r->set.opt.limit = (EgtLimit)v;
    } else if (!strcmp(opt, "--smooth")) {
        if (!parse_choice(opt, value, SMOOTH_ARGS, EGT_SMOOTH_COUNT, &v))
            return -1;
        r->set.filt.smooth = (EgtSmooth)v;
    } else if (!strcmp(opt, "--outline-mode")) {
        if (!parse_choice(opt, value, OUTLINE_ARGS, EGT_OUTLINE_COUNT, &v))
            return -1;
        r->set.filt.outline_mode = (EgtOutlineMode)v;
    } else {
        return 0;
    }
    return 2;
}

typedef struct {
    char *buf;
    size_t len, cap;
    bool failed;
} Str;

/* Appends printf-style text to s, growing it; marks s failed on error. */
static void add(Str *s, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (s->failed)
        return;
    for (;;) {
        va_start(ap, fmt);
        n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
        va_end(ap);
        if (n < 0) {
            s->failed = true;
            return;
        }
        if ((size_t)n < s->cap - s->len) {
            s->len += (size_t)n;
            return;
        }
        {
            size_t cap = (s->cap + (size_t)n) * 2;
            char *b = realloc(s->buf, cap);
            if (!b) {
                s->failed = true;
                return;
            }
            s->buf = b;
            s->cap = cap;
        }
    }
}

/* A file name as one shell word: quoted unless plainly safe. */
static void add_word(Str *s, const char *w)
{
    const char *p;
    bool plain = *w != '\0';
    for (p = w; *p; p++)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/+", *p))
            plain = false;
    if (plain) {
        add(s, " %s", w);
        return;
    }
    add(s, " \"");
    for (p = w; *p; p++)
        add(s, (*p == '"' || *p == '\\' || *p == '$' || *p == '`') ? "\\%c" : "%c", *p);
    add(s, "\"");
}

char *egt_recipe_string(const EgtRecipe *r)
{
    Str s = { NULL, 0, 0, false };
    EgtSettings set = r->set, def;
    NumOpt now[NUM_OPTS], was[NUM_OPTS];
    int k;
    /* What egt_run processes: settings that don't apply are at their
     * defaults, so only what was changed (and applies) gets written. */
    egt_settings_normalize(&set);
    egt_settings_default(&def);
    num_opts(&set, now);
    num_opts(&def, was);
    s.cap = 256;
    s.buf = malloc(s.cap);
    if (!s.buf)
        return NULL;
    s.buf[0] = '\0';

    add(&s, "egatool-cli -p");
    add_word(&s, r->palette ? r->palette : "ega");
    add(&s, " -d %s", egt_dither_arg(set.opt.dither));
    if (set.opt.limit != EGT_LIMIT_NONE)
        add(&s, " --limit %s", egt_limit_arg(set.opt.limit));
    add(&s, " -s %dx%d", set.width, set.height);
    if (set.nearest)
        add(&s, " --nearest");
    if (set.calibrate)
        add(&s, " --calibrate");
    if (set.filt.smooth != EGT_SMOOTH_NONE)
        add(&s, " --smooth %s", SMOOTH_ARGS[set.filt.smooth]);
    if (set.filt.outline_mode != EGT_OUTLINE_BASIC)
        add(&s, " --outline-mode %s", OUTLINE_ARGS[set.filt.outline_mode]);
    for (k = 0; k < NUM_OPTS; k++) {
        if (now[k].i && *now[k].i != *was[k].i)
            add(&s, " %s %d", now[k].name, *now[k].i);
        if (now[k].f && *now[k].f != *was[k].f)
            add(&s, " %s %.2f", now[k].name, (double)*now[k].f);
    }
    if (r->scale > 1)
        add(&s, " -x %d", r->scale);
    add_word(&s, r->source ? r->source : "INPUT");
    add_word(&s, r->output ? r->output : "OUTPUT.png");
    if (s.failed) {
        free(s.buf);
        return NULL;
    }
    return s.buf;
}
