/*
 * egatool engine internals shared between engine.c and limits.c.
 */
#ifndef EGATOOL_ENGINE_INT_H
#define EGATOOL_ENGINE_INT_H

#include "engine.h"

typedef struct {
    int dx, dy, w;
} KernelTap;

typedef struct {
    int divisor;
    int ntaps;
    KernelTap taps[12];
} Kernel;

static const Kernel KERNEL_FLOYD = { 16, 4, {
    {1, 0, 7}, {-1, 1, 3}, {0, 1, 5}, {1, 1, 1} } };
static const Kernel KERNEL_BURKES = { 32, 7, {
    {1, 0, 8}, {2, 0, 4},
    {-2, 1, 2}, {-1, 1, 4}, {0, 1, 8}, {1, 1, 4}, {2, 1, 2} } };
static const Kernel KERNEL_SIERRA = { 32, 10, {
    {1, 0, 5}, {2, 0, 3},
    {-2, 1, 2}, {-1, 1, 4}, {0, 1, 5}, {1, 1, 4}, {2, 1, 2},
    {-1, 2, 2}, {0, 2, 3}, {1, 2, 2} } };
static const Kernel KERNEL_STUCKI = { 42, 12, {
    {1, 0, 8}, {2, 0, 4},
    {-2, 1, 2}, {-1, 1, 4}, {0, 1, 8}, {1, 1, 4}, {2, 1, 2},
    {-2, 2, 1}, {-1, 2, 2}, {0, 2, 4}, {1, 2, 2}, {2, 2, 1} } };

/* Hardware colour limits (limits.c): converts with opt->limit's per-cell
 * colour rules. Returns false on allocation failure. */
/* Checkerboard: each step of "Prefer solid" (0..100) lets the nearest
 * solid colour be this much further from the source (RGB distance) than
 * the best two-colour mix and still be used. */
#define PREFER_SOLID_STEP 0.25f

/* Whether to keep the solid colour instead of a checkerboard mix, given
 * their RGB distances from the source. */
bool egt__keep_solid(float solid_dist, float mix_dist, int prefer_solid);

bool egt__convert_limited(const EgtImage *src, EgtImage *dst, const EgtPalette *pal,
                          const EgtConvertOptions *opt, const Kernel *kernel);

#endif
