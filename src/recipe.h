/*
 * The settings that made an image, written as a egatool-cli command line
 * and stored in saved PNGs, so a result can be reproduced or inspected.
 */
#ifndef EGATOOL_RECIPE_H
#define EGATOOL_RECIPE_H

#include "engine.h"

typedef struct {
    const char *source;  /* input file name, or NULL (pasted image) */
    const char *output;  /* output file name, or NULL */
    const char *palette; /* CLI name (see egt_builtin_palette_arg) or a .pal file */
    int width, height;   /* 0 = original size */
    bool nearest;
    EgtAdjust adj;
    EgtFilters filt;     /* outline_mask is ignored */
    EgtConvertOptions opt; /* detail is ignored */
    int scale;
    bool calibrate;      /* ignored for checker */
    int calibrate_max;   /* calibrate: largest colour change, 0 = any */
} EgtRecipe;

/* Command-line name of a built-in palette ("ega", "cga-warm"...). */
const char *egt_builtin_palette_arg(EgtBuiltinPalette p);

/* Command-line name of a dither mode ("checker", "sierra"...). */
const char *egt_dither_arg(EgtDither d);

/* The recipe as a egatool-cli command line, listing only the settings
 * that apply (no checker options for Sierra, no radius without
 * smoothing...). Returns a malloc'd string, or NULL. */
char *egt_recipe_string(const EgtRecipe *r);

#endif
