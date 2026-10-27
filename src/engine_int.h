/*
 * EGAtool engine internals shared between engine.c and limits.c.
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#ifndef EGATOOL_ENGINE_INT_H
#define EGATOOL_ENGINE_INT_H

#include "engine.h"

/* Which colours each pixel may use: the image is split into cells of
 * cell_w x cell_h pixels, and the cell at (cx, cy) uses palette
 * pals[cell[cy * across + cx]]. Without a colour limit, one cell covers
 * the whole image. */
typedef struct {
    const EgtPalette *pals;
    int npals;
    const int *cell;
    int cell_w, cell_h, across;
} EgtPalMap;

/* Dithers src into dst (same size; may be the same image) with opt's
 * dither mode, each pixel with the palette map gives it (engine.c).
 * Returns false on allocation failure or a palette too big for the mode. */
bool egt__dither(const EgtImage *src, EgtImage *dst, const EgtPalMap *map,
                 const EgtConvertOptions *opt);

/* Conversion with a hardware colour limit (limits.c), for egt_convert():
 * chooses each cell's colours, then draws with egt__dither. */
bool egt__convert_limited(const EgtImage *src, EgtImage *dst, const EgtPalette *pal,
                          const EgtConvertOptions *opt);

#endif
