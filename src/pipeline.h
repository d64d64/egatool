/*
 * The whole conversion as one step, shared by the GUI and the CLI: resize
 * and smoothing, adjustments, Solid areas, outlines, then dithering. Both
 * front ends describe what they want in EgtSettings, so they always run
 * the same steps, and a saved recipe reproduces a result exactly.
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#ifndef EGATOOL_PIPELINE_H
#define EGATOOL_PIPELINE_H

#include "engine.h"

typedef struct {
    int width, height;     /* target size */
    bool nearest;          /* resize with nearest neighbour, not area average */
    EgtAdjust adj;
    EgtFilters filt;       /* outline_mask is ignored (made by egt_run) */
    EgtConvertOptions opt; /* detail is ignored (made by egt_run) */
    bool calibrate;        /* Solid areas (not for checkerboard) */
    int calibrate_max;     /* Solid areas: largest colour change, 0 = any */
} EgtSettings;

/* Valid ranges, shared by the CLI's checks and the GUI's sliders. */
#define EGT_MAX_SIZE 16384
#define EGT_SNAP_MAX 64
#define EGT_CONSISTENCY_MAX 300
#define EGT_CALIBRATE_MAX_LIMIT 442 /* the largest RGB distance */
#define EGT_MIN_REGION_MAX 10000
#define EGT_OUTLINE_MIN_MAX 10000

/* Defaults: checkerboard, nothing adjusted or filtered, size 0 (set it). */
void egt_settings_default(EgtSettings *s);

/* NULL if every value is in range, else a message naming the problem. */
const char *egt_settings_check(const EgtSettings *s);

/* Puts the settings in the form they are processed and recorded in: values
 * that don't apply go back to their defaults (no snap for checkerboard...),
 * and values are rounded as a saved recipe writes them (2 decimals, radius
 * snapped). Two settings that give the same result then compare equal.
 * This is the one place that says which settings apply. */
void egt_settings_normalize(EgtSettings *s);

/* Intermediate results kept between runs on the same source (the GUI
 * reconverts on every slider move). Zero-initialise; free with
 * egt_cache_free. Keyed by the caller's source id. */
typedef struct {
    unsigned base_src;
    EgtSettings base_key; /* resize and smoothing of base */
    EgtImage base;        /* resized and smoothed source */
    unsigned mask_src;
    int mask_w, mask_h, mask_sens;
    uint8_t *mask;        /* hi-res outlines */
    unsigned detail_src;
    int detail_w, detail_h;
    EgtImage detail;      /* for checkerboard's Calm harsh mixes */
} EgtCache;

void egt_cache_free(EgtCache *c);

typedef enum {
    EGT_RUN_OK,
    EGT_RUN_NO_MEMORY,
    EGT_RUN_PALETTE_TOO_BIG, /* checkerboard without a limit: at most EGT_CHECKER_MAX_COLORS */
    EGT_RUN_BAD_SETTINGS,    /* egt_settings_check failed, or an empty palette or source */
} EgtRunStatus;

const char *egt_run_message(EgtRunStatus s);

typedef struct {
    EgtImage prepared; /* what the converter saw (also set on PALETTE_TOO_BIG) */
    EgtImage output;   /* the result (only on EGT_RUN_OK) */
    EgtCalibAnchor anchors[EGT_CALIBRATE_MAX];
    int nanchors;      /* Solid areas used */
} EgtResult;

void egt_result_free(EgtResult *r);

/* Converts source with settings s (normalised first, on a copy). cache may
 * be NULL; source_id tells sources apart in it (any value without a
 * cache). Fills res; on failure everything is freed except, for
 * PALETTE_TOO_BIG, res->prepared. */
EgtRunStatus egt_run(const EgtImage *source, unsigned source_id, const EgtPalette *pal,
                     const EgtSettings *s, EgtCache *cache, EgtResult *res);

#endif
