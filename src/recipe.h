/*
 * The settings that made an image, written as a egatool-cli command line
 * and stored in saved PNGs, so a result can be reproduced or inspected.
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#ifndef EGATOOL_RECIPE_H
#define EGATOOL_RECIPE_H

#include "pipeline.h"

typedef struct {
    const char *source;  /* input file name, or NULL (pasted image) */
    const char *output;  /* output file name, or NULL */
    const char *palette; /* CLI name (see egt_builtin_palette_arg) or a .pal file */
    EgtSettings set;     /* written normalised (egt_settings_normalize) */
    int scale;           /* export enlargement, 1..8 */
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
