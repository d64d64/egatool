/*
 * Writes the settings that made an image as an egatool-cli command line,
 * stored in saved PNGs.
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#include "recipe.h"

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

const char *egt_builtin_palette_arg(EgtBuiltinPalette p)
{
    return (p >= 0 && p < EGT_PAL_COUNT) ? PALETTE_ARGS[p] : "ega";
}

const char *egt_dither_arg(EgtDither d)
{
    return (d >= 0 && d < EGT_DITHER_COUNT) ? DITHER_ARGS[d] : "?";
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
    EgtSettings set = r->set;
    const EgtConvertOptions *o = &set.opt;
    const EgtFilters *f = &set.filt;
    /* What egt_run processes, so replaying gives the same pixels. */
    egt_settings_normalize(&set);
    s.cap = 256;
    s.buf = malloc(s.cap);
    if (!s.buf)
        return NULL;
    s.buf[0] = '\0';

    add(&s, "egatool-cli -p");
    add_word(&s, r->palette ? r->palette : "ega");
    add(&s, " -d %s", egt_dither_arg(o->dither));
    if (o->dither == EGT_DITHER_EGA_CHECKER) {
        add(&s, " --prefer-solid %d --light-mix %d", o->prefer_solid, o->light_mix);
        if (o->limit == EGT_LIMIT_NONE)
            add(&s, " --harsh-mix %d", o->harsh_mix);
    }
    else if (o->dither != EGT_DITHER_NONE)
        add(&s, " --snap %d", o->snap);
    if (o->limit > EGT_LIMIT_NONE && o->limit < EGT_LIMIT_COUNT)
        add(&s, " --limit %s --consistency %d", egt_limit_arg(o->limit), o->consistency);
    add(&s, " -s %dx%d", set.width, set.height);
    if (set.nearest)
        add(&s, " --nearest");
    add(&s, " --brightness %d --contrast %d --saturation %.2f",
        set.adj.brightness, set.adj.contrast, (double)set.adj.saturation);
    if (set.calibrate) {
        add(&s, " --calibrate");
        if (set.calibrate_max > 0)
            add(&s, " --calibrate-max %d", set.calibrate_max);
    }
    if (f->smooth != EGT_SMOOTH_NONE) {
        char name[16];
        size_t i;
        snprintf(name, sizeof name, "%s", egt_smooth_name(f->smooth));
        for (i = 0; name[i]; i++)
            if (name[i] >= 'A' && name[i] <= 'Z')
                name[i] = (char)(name[i] - 'A' + 'a');
        add(&s, " --smooth %s --radius %.2f", name, (double)f->smooth_radius);
        if (f->smooth == EGT_SMOOTH_SURFACE)
            add(&s, " --threshold %d", f->smooth_threshold);
    }
    if (f->outline > 0) {
        add(&s, " --outline %d --outline-mode %s --outline-dark %.2f", f->outline,
            f->outline_mode == EGT_OUTLINE_HIRES ? "hires" : "basic",
            (double)f->outline_darkness);
        if (f->outline_min_length > 0)
            add(&s, " --outline-min %d", f->outline_min_length);
    }
    if (o->min_region > 1)
        add(&s, " --min-region %d", o->min_region);
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
