/*
 * The whole conversion as one step, shared by the GUI and the CLI.
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#include "pipeline.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void egt_settings_default(EgtSettings *s)
{
    memset(s, 0, sizeof *s);
    s->opt.dither = EGT_DITHER_EGA_CHECKER;
    s->opt.prefer_solid = EGT_PREFER_SOLID_DEFAULT;
    s->opt.harsh_mix = 10;
    s->opt.snap = 16;
    s->opt.consistency = 30;
    s->filt.smooth_radius = 1.0f;
    s->filt.smooth_threshold = 25;
    s->filt.outline_darkness = 1.0f;
}

static bool in_range(double v, double lo, double hi)
{
    return isfinite(v) && v >= lo && v <= hi;
}

const char *egt_settings_check(const EgtSettings *s)
{
    if (s->width < 1 || s->height < 1 || s->width > EGT_MAX_SIZE || s->height > EGT_MAX_SIZE)
        return "size must be 1..16384 pixels each way";
    if ((unsigned)s->opt.dither >= EGT_DITHER_COUNT)
        return "unknown dither mode";
    if ((unsigned)s->opt.limit >= EGT_LIMIT_COUNT)
        return "unknown colour limit";
    if ((unsigned)s->filt.smooth >= EGT_SMOOTH_COUNT)
        return "unknown smoothing";
    if ((unsigned)s->filt.outline_mode >= EGT_OUTLINE_COUNT)
        return "unknown outline mode";
    if (!in_range(s->adj.brightness, -128, 127))
        return "brightness must be -128..127";
    if (!in_range(s->adj.contrast, -127, 127))
        return "contrast must be -127..127";
    if (!in_range(s->adj.saturation, -1, 1.5))
        return "saturation must be -1..1.5";
    if (!in_range(s->opt.prefer_solid, 0, 100))
        return "prefer-solid must be 0..100";
    if (!in_range(s->opt.light_mix, 0, 100))
        return "light-mix must be 0..100";
    if (!in_range(s->opt.harsh_mix, 0, 100))
        return "harsh-mix must be 0..100";
    if (!in_range(s->opt.snap, 0, EGT_SNAP_MAX))
        return "snap must be 0..64";
    if (!in_range(s->opt.consistency, 0, EGT_CONSISTENCY_MAX))
        return "consistency must be 0..300";
    if (!in_range(s->opt.min_region, 0, EGT_MIN_REGION_MAX))
        return "min-region must be 0..10000";
    if (!in_range(s->filt.smooth_radius, 0.25, 4))
        return "radius must be 0.25..4";
    if (!in_range(s->filt.smooth_threshold, 1, 100))
        return "threshold must be 1..100";
    if (!in_range(s->filt.outline, 0, 100))
        return "outline must be 0..100";
    if (!in_range(s->filt.outline_darkness, 0, 1))
        return "outline-dark must be 0..1";
    if (!in_range(s->filt.outline_min_length, 0, EGT_OUTLINE_MIN_MAX))
        return "outline-min must be 0..10000";
    if (!in_range(s->calibrate_max, 0, EGT_CALIBRATE_MAX_LIMIT))
        return "calibrate-max must be 0..442";
    return NULL;
}

/* As a recipe writes it: two decimals. */
static float two_decimals(float v)
{
    return roundf(v * 100.0f) / 100.0f;
}

void egt_settings_normalize(EgtSettings *s)
{
    EgtConvertOptions *o = &s->opt;
    EgtFilters *f = &s->filt;
    bool checker = o->dither == EGT_DITHER_EGA_CHECKER;

    s->adj.saturation = two_decimals(s->adj.saturation);
    if (s->adj.saturation == 0.0f)
        s->adj.saturation = 0.0f; /* not -0 */

    /* Unused values go back to their defaults (still valid). */
    if (f->smooth == EGT_SMOOTH_NONE)
        f->smooth_radius = 1.0f;
    else
        f->smooth_radius = egt_snap_radius(f->smooth_radius);
    if (f->smooth != EGT_SMOOTH_SURFACE)
        f->smooth_threshold = 25;
    if (f->outline <= 0) {
        f->outline = 0;
        f->outline_mode = EGT_OUTLINE_BASIC;
        f->outline_darkness = 1.0f;
        f->outline_min_length = 0;
    } else {
        f->outline_darkness = two_decimals(f->outline_darkness);
    }
    f->outline_mask = NULL;

    if (checker) {
        o->snap = 0;
        s->calibrate = false;
    } else {
        o->prefer_solid = o->harsh_mix = o->light_mix = 0;
        if (o->dither == EGT_DITHER_NONE)
            o->snap = 0;
    }
    if (!s->calibrate)
        s->calibrate_max = 0;
    if (o->limit != EGT_LIMIT_NONE) {
        o->harsh_mix = 0;
        o->min_region = 0;
    } else {
        o->consistency = 0;
    }
    if (o->min_region <= 1 || !egt_dither_supports_cleanup(o->dither))
        o->min_region = 0;
    o->detail = NULL;
}

void egt_cache_free(EgtCache *c)
{
    egt_image_free(&c->base);
    egt_image_free(&c->detail);
    free(c->mask);
    memset(c, 0, sizeof *c);
}

const char *egt_run_message(EgtRunStatus s)
{
    switch (s) {
    case EGT_RUN_OK:              return "done";
    case EGT_RUN_NO_MEMORY:       return "out of memory";
    case EGT_RUN_PALETTE_TOO_BIG: return "checkerboard needs a palette of at most 64 colours";
    case EGT_RUN_BAD_SETTINGS:    return "invalid settings";
    }
    return "?";
}

void egt_result_free(EgtResult *r)
{
    egt_image_free(&r->prepared);
    egt_image_free(&r->output);
    r->nanchors = 0;
}

/* Whether the cached base image was made with the same resize settings. */
static bool same_base(const EgtSettings *a, const EgtSettings *b)
{
    return a->width == b->width && a->height == b->height && a->nearest == b->nearest &&
           a->filt.smooth == b->filt.smooth && a->filt.smooth_radius == b->filt.smooth_radius &&
           a->filt.smooth_threshold == b->filt.smooth_threshold;
}

EgtRunStatus egt_run(const EgtImage *source, unsigned source_id, const EgtPalette *pal,
                     const EgtSettings *settings, EgtCache *cache, EgtResult *res)
{
    EgtCache local;
    EgtCache *c = cache ? cache : &local;
    EgtSettings s = *settings;
    EgtFilters filt;
    EgtConvertOptions opt;
    EgtRunStatus status = EGT_RUN_NO_MEMORY;
    int w, h;

    memset(res, 0, sizeof *res);
    memset(&local, 0, sizeof local);
    egt_settings_normalize(&s);
    if (egt_settings_check(&s) || pal->count < 1 || !source->pixels)
        return EGT_RUN_BAD_SETTINGS;
    w = s.width;
    h = s.height;

    /* Resize and smooth, unless the cache already has it. */
    if (!c->base.pixels || c->base_src != source_id || !same_base(&c->base_key, &s)) {
        egt_image_free(&c->base);
        c->base = egt_resize_smoothed(source, w, h, s.nearest, &s.filt);
        c->base_src = source_id;
        c->base_key = s;
        if (!c->base.pixels)
            goto done;
    }
    res->prepared = egt_image_copy(&c->base);
    res->output = egt_image_alloc(w, h);
    if (!res->prepared.pixels || !res->output.pixels)
        goto done;

    egt_adjust(&res->prepared, &s.adj);
    if (s.calibrate) {
        int n = egt_calibrate(&res->prepared, pal, s.calibrate_max, res->anchors);
        if (n < 0)
            goto done;
        res->nanchors = n;
    }

    filt = s.filt;
    filt.smooth = EGT_SMOOTH_NONE; /* done while resizing */
    if (filt.outline > 0 && filt.outline_mode == EGT_OUTLINE_HIRES) {
        if (!c->mask || c->mask_src != source_id || c->mask_w != w || c->mask_h != h ||
            c->mask_sens != filt.outline) {
            free(c->mask);
            c->mask = egt_outline_mask_hires(source, w, h, filt.outline);
            c->mask_src = source_id;
            c->mask_w = w;
            c->mask_h = h;
            c->mask_sens = filt.outline;
            if (!c->mask)
                goto done;
        }
        filt.outline_mask = c->mask;
    }
    if (!egt_filter(&res->prepared, &filt))
        goto done;

    opt = s.opt;
    if (opt.dither == EGT_DITHER_EGA_CHECKER && opt.limit == EGT_LIMIT_NONE) {
        if (pal->count > EGT_CHECKER_MAX_COLORS) {
            status = EGT_RUN_PALETTE_TOO_BIG;
            goto done;
        }
        if (opt.harsh_mix > 0) {
            if (!c->detail.pixels || c->detail_src != source_id || c->detail_w != w ||
                c->detail_h != h) {
                egt_image_free(&c->detail);
                c->detail = egt_detail_image(source, w, h);
                c->detail_src = source_id;
                c->detail_w = w;
                c->detail_h = h;
                if (!c->detail.pixels)
                    goto done;
            }
            opt.detail = &c->detail;
        }
    }
    if (egt_convert(&res->prepared, &res->output, pal, &opt))
        status = EGT_RUN_OK;

done:
    if (status != EGT_RUN_OK) {
        egt_image_free(&res->output);
        if (status != EGT_RUN_PALETTE_TOO_BIG)
            egt_image_free(&res->prepared);
    }
    if (!cache)
        egt_cache_free(&local);
    return status;
}
