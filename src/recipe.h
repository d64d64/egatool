/*
 * The egatool-cli command line language. The settings that made an image
 * are written in it and stored in saved PNGs, so a result can be
 * reproduced or inspected.
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

/* Command-line name of a colour limit ("spectrum", "c64-multi"...). */
const char *egt_limit_arg(EgtLimit l);

/* Reads one option into r: opt is the option ("--snap"), value the word
 * after it or NULL. Returns the words used (1 or 2), 0 if opt is not a
 * recipe option, -1 if value is bad (said on stderr). Number ranges are
 * left to egt_settings_check. Size and file names are read by the CLI. */
int egt_recipe_option(EgtRecipe *r, const char *opt, const char *value);

/* The recipe as a egatool-cli command line, with palette, dither and size
 * and then only the settings that apply and differ from their defaults.
 * Returns a malloc'd string, or NULL. */
char *egt_recipe_string(const EgtRecipe *r);

#endif
