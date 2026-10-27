/*
 * EGAtool engine: palette reduction and dithering on RGBA8 buffers.
 * No dependencies beyond the C standard library.
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#ifndef EGATOOL_ENGINE_H
#define EGATOOL_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#define EGT_VERSION "1.1"

#define EGT_MAX_COLORS 256

typedef struct {
    uint8_t r, g, b;
} EgtColor;

typedef struct {
    int count;
    EgtColor colors[EGT_MAX_COLORS];
} EgtPalette;

/* Image in tightly packed RGBA8 (4 bytes per pixel). */
typedef struct {
    int width, height;
    uint8_t *pixels;
} EgtImage;

typedef enum {
    EGT_PAL_EGA,
    EGT_PAL_VGA,
    EGT_PAL_CGA,      /* mode 4 palette 1, high intensity: cyan, magenta, white */
    EGT_PAL_CGA_WARM, /* mode 4 palette 0, high intensity: green, red, yellow */
    EGT_PAL_EGA64,    /* every colour EGA can show: 4 levels per channel */
    EGT_PAL_CPC,      /* Amstrad CPC hardware palette: 3 levels per channel */
    EGT_PAL_C64,      /* Commodore 64 (Pepto) */
    EGT_PAL_SPECTRUM, /* ZX Spectrum, normal and bright */
    EGT_PAL_MSX,      /* MSX1 / TMS9918 */
    EGT_PAL_APPLE2,   /* Apple II high resolution, 6 colours */
    EGT_PAL_GAMEBOY,  /* original Game Boy greens */
    EGT_PAL_NES,      /* NES 2C02, 55 distinct colours */
    EGT_PAL_DB32,     /* DawnBringer's 32 colour pixel art palette */
    EGT_PAL_PICO8,    /* PICO-8 fantasy console */
    EGT_PAL_COUNT
} EgtBuiltinPalette;

typedef enum {
    EGT_DITHER_EGA_CHECKER, /* solid colours and 2-colour checkerboards, the Sierra/LucasArts EGA look */
    EGT_DITHER_ORDERED,
    EGT_DITHER_ORDERED2, /* two-colour mixes: also for colours told apart by hue */
    EGT_DITHER_BURKES,
    EGT_DITHER_FLOYD_STEINBERG,
    EGT_DITHER_SIERRA,
    EGT_DITHER_STUCKI,
    EGT_DITHER_NONE,
    EGT_DITHER_COUNT
} EgtDither;

/* Hardware colour limits: the per-cell colour rules of old machines,
 * usable with any palette. Cells get their colours the way a pixel artist
 * would choose them (see limits.c), then the dither mode draws each cell
 * with only its own colours. */
typedef enum {
    EGT_LIMIT_NONE,
    EGT_LIMIT_SPECTRUM,  /* 2 colours per 8x8, both normal or both bright */
    EGT_LIMIT_C64_HIRES, /* 2 colours per 8x8 */
    EGT_LIMIT_C64_MULTI, /* double-wide pixels, 3 per 4x8 cell + 1 background */
    EGT_LIMIT_MSX,       /* 2 colours per 8x1 */
    EGT_LIMIT_NES,       /* 16x16 areas each use 1 of 4 palettes of 3 + 1 background */
    EGT_LIMIT_COUNT
} EgtLimit;

/* Checkerboard mode tests every colour pair, so palettes larger than this
 * are rejected for it (pairs grow quadratically). */
#define EGT_CHECKER_MAX_COLORS 64
/* Default for EgtConvertOptions.prefer_solid. */
#define EGT_PREFER_SOLID_DEFAULT 16

typedef struct {
    EgtDither dither;
    /* Checker only, 0..100: how much better a two-colour mix must look than
     * the nearest solid colour before it is used (0 = whenever it is
     * closer). */
    int prefer_solid;
    /* Checker only, 0..100: steer away from harsh colour pairs (black +
     * white) in small areas. Higher values reach larger areas, 100 = the
     * whole image; see egt_harsh_mix_area(). 0 = off. */
    int harsh_mix;
    /* Optional, from egt_detail_image(): the source at up to 2x the output
     * size, used to see which colours really occur around each pixel
     * (details thinner than a pixel blur away in the output-size image).
     * Without it, the image being converted is used. */
    const EgtImage *detail;
    /* Checker only, 0..100: how a checkerboard's colour is estimated. 0
     * averages pixel values (how visible pixels tend to look up close); 100
     * averages emitted light (how a fine pattern looks from a distance:
     * black + white reads as light grey). In between is a blend (the pair
     * is averaged with a gamma from 1.0 to 2.2). */
    int light_mix;
    /* Checker and none only: merge same-colour (or same-pattern) regions
     * smaller than this many pixels into their surroundings. 0 = off. */
    int min_region;
    /* Ordered and error diffusion only, in RGB levels: source colours this
     * close to a palette colour are drawn solid in it, without dithering
     * (and diffusion carries no error into or out of them); dithering
     * fades back in up to twice this distance. Keeps near-black and
     * near-white areas from filling with stray dots. 0 = off. */
    int snap;
    /* Per-cell colour rules (EGT_LIMIT_NONE = off). With a limit, the
     * checker mode's harsh_mix and min_region don't apply, and its
     * palette size limit doesn't either (pairs come from the cell). */
    EgtLimit limit;
    /* 0..300: how strongly neighbouring cells keep the same colours, so
     * flat areas and gradients don't show block seams. */
    int consistency;
} EgtConvertOptions;

typedef enum {
    EGT_SMOOTH_NONE,
    EGT_SMOOTH_MEDIAN,
    EGT_SMOOTH_KUWAHARA, /* edge-preserving, flattens areas like paint */
    EGT_SMOOTH_SURFACE,  /* averages only similar colours; keeps thin lines */
    EGT_SMOOTH_COUNT
} EgtSmooth;

typedef enum {
    EGT_OUTLINE_BASIC, /* edges of the target-size image, dark side only */
    EGT_OUTLINE_HIRES, /* Canny on the full-size source, mask scaled down */
    EGT_OUTLINE_COUNT
} EgtOutlineMode;

typedef struct {
    EgtSmooth smooth;
    float smooth_radius;    /* in target pixels, 0.25..4; see egt_snap_radius() */
    int smooth_threshold;   /* SURFACE only: colour difference (levels) still averaged, 1..100 */
    int outline;            /* edge sensitivity 0..100, 0 = off */
    float outline_darkness; /* 0..1, 1 = black */
    int outline_min_length; /* drop outline fragments shorter than this, 0 = keep all */
    EgtOutlineMode outline_mode;
    /* HIRES only: mask from egt_outline_mask_hires() at the image's size.
     * Computed separately because it needs the source and can be cached. */
    const uint8_t *outline_mask;
} EgtFilters;

typedef struct {
    int brightness; /* -128..127, 0 = unchanged */
    int contrast;   /* -127..127, 0 = unchanged; below 0 towards mid grey */
    float saturation; /* -1..1, 0 = unchanged */
} EgtAdjust;

const char *egt_dither_name(EgtDither d);
const char *egt_limit_name(EgtLimit l);
const char *egt_limit_desc(EgtLimit l); /* one line on the rule */
/* The machine's own palette and screen size, for picking them along with
 * the limit (they can still be changed afterwards). */
void egt_limit_machine(EgtLimit l, EgtBuiltinPalette *pal, int *width, int *height);
const char *egt_builtin_palette_name(EgtBuiltinPalette p);

void egt_builtin_palette(EgtBuiltinPalette which, EgtPalette *out);

/* Loads a JASC-PAL file. Returns false on error. */
bool egt_load_jasc_palette(const char *path, EgtPalette *out);

EgtImage egt_image_alloc(int width, int height);
EgtImage egt_image_copy(const EgtImage *src);
void egt_image_free(EgtImage *img);

/* Nearest-neighbour resize into a newly allocated image. */
EgtImage egt_resize_nearest(const EgtImage *src, int width, int height);

/* Box-filter (area average) resize: each output pixel averages the source
 * area it covers, partly covered source pixels weighted by their share, so
 * downscaling doesn't pick up noise and non-integer ratios sample evenly.
 * Upscaled axes fall back to nearest neighbour. */
EgtImage egt_resize_area(const EgtImage *src, int width, int height);

/* Auto contrast, like Photoshop's: sets brightness and contrast (keeps
 * saturation) so the darkest and lightest 0.5% of the image's channel
 * values become 0 and 255. Colours keep their balance; all channels get
 * the same stretch. */
void egt_auto_levels(const EgtImage *img, EgtAdjust *adj);

/* Calibration to solid areas: the largest flat areas of the image (connected
 * pixels of gently changing colour, at least 1% of the image each) are
 * moved exactly onto their nearest palette colour, so they convert to that
 * colour without dithering. Every other colour moves by a blend of those
 * shifts, weighted by how close it is to each area's colour: similar
 * colours move along, distant ones (more than ~64 levels away) hardly at
 * all. An area is skipped if that would change the rest of the image more
 * than the area itself (a brick wall whose nearest colour is grey). */
#define EGT_CALIBRATE_MAX 8

typedef struct {
    EgtColor from, to;
    float share; /* of the image, 0..1 */
} EgtCalibAnchor;

/* Calibrates img in place to up to EGT_CALIBRATE_MAX areas. max_shift > 0
 * skips areas further than that (RGB distance, levels) from their palette
 * colour; 0 = any change. Fills anchors (may be NULL) and returns how many
 * were used (0 if no area qualifies), or -1 on allocation failure. */
int egt_calibrate(EgtImage *img, const EgtPalette *pal, int max_shift, EgtCalibAnchor *anchors);

/* Brightness/contrast/saturation, in place. */
void egt_adjust(EgtImage *img, const EgtAdjust *adj);

/* Canny edge detection on the full-resolution source, reduced to a
 * width x height mask (1 = outline pixel). Returns NULL on allocation
 * failure; free() the result. */
uint8_t *egt_outline_mask_hires(const EgtImage *source, int width, int height,
                                int sensitivity);

/* Nearest supported smoothing radius: quarter steps below 1, half steps
 * from 1 to 4. */
float egt_snap_radius(float radius);

/* Resizes source to width x height (area average, or nearest if requested)
 * with the smoothing from f applied along the way, on an intermediate image
 * 2x the target size (4x below radius 1, less if the source is smaller), so
 * small radii keep detail at its real size. Returns an image with NULL pixels on failure. */
EgtImage egt_resize_smoothed(const EgtImage *source, int width, int height,
                             bool nearest, const EgtFilters *f);

/* Outline inking, in place (smoothing is done by egt_resize_smoothed). Returns false on allocation
 * failure (image left unchanged). */
bool egt_filter(EgtImage *img, const EgtFilters *f);

/* Largest area (pixels) harsh_mix still applies to in a width x height
 * image; -1 = everywhere, 0 = off. Scaled with the image so a setting
 * covers the same share of the picture at any resolution. */
int egt_harsh_mix_area(int harsh_mix, int width, int height);

/* Source reduced (area average) to at most 2x width x height, for
 * EgtConvertOptions.detail. Returns an image with NULL pixels on failure. */
EgtImage egt_detail_image(const EgtImage *source, int width, int height);

/* Whether min_region cleanup applies to this dither mode. */
bool egt_dither_supports_cleanup(EgtDither d);

/* Converts src into dst (same size, already allocated; may equal src).
 * Returns false if the options are not usable with this palette. */
bool egt_convert(const EgtImage *src, EgtImage *dst, const EgtPalette *pal,
                 const EgtConvertOptions *opt);

#endif
