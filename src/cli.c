/*
 * EGAtool command line tool (egatool-cli): converts one image with the
 * given options and saves it with those settings in a PNG comment.
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine.h"
#include "imageio.h"
#include "pipeline.h"
#include "recipe.h"

/* Case-insensitive compare (strcasecmp is POSIX, not standard C). */
static int strcasecmp_ascii(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* File name without its directories. */
static const char *base_name(const char *path)
{
    const char *p, *b = path;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

static void usage(void)
{
    int i;
    fprintf(stderr,
        "EGAtool " EGT_VERSION "\n"
        "usage: egatool-cli [options] input output\n"
        "  -p, --palette NAME   ");
    for (i = 0; i < EGT_PAL_COUNT; i++)
        fprintf(stderr, "%s%s%s", egt_builtin_palette_arg((EgtBuiltinPalette)i),
                i == 0 ? " (default)" : "", (i == 6 || i == EGT_PAL_COUNT - 1) ? ",\n                       " : ", ");
    fprintf(stderr,
        "or a JASC .pal file\n"
        "  -d, --dither NAME    ");
    for (i = 0; i < EGT_DITHER_COUNT; i++)
        fprintf(stderr, "%s%s", egt_dither_arg((EgtDither)i), i + 1 < EGT_DITHER_COUNT ? ", " : "");
    fprintf(stderr,
        " (default checker)\n"
        "      --prefer-solid N checker: how much better a mix must look than\n"
        "                       the nearest solid colour, 0..100, default %d\n"
        "      --light-mix N    checker: 0 = judge checkerboards by pixel values,\n"
        "                       100 = by emitted light, default 0\n"
        "      --harsh-mix N    checker: calm harsh colour pairs in small areas,\n"
        "                       0..100 (100 = everywhere), default 10\n"
        "      --calibrate      not checker: make the largest flat areas exact\n"
        "                       palette colours, shifting similar colours along\n"
        "      --calibrate-max N  calibrate: skip areas whose colour would change\n"
        "                       by more than N levels (RGB distance), default any\n"
        "      --snap N         ordered/diffusion: colours within N levels of a\n"
        "                       palette colour stay solid, 0..64, default 16\n", EGT_PREFER_SOLID_DEFAULT);
    fprintf(stderr,
        "      --limit NAME     hardware colour limits per cell, with any palette:\n");
    for (i = 1; i < EGT_LIMIT_COUNT; i++) {
        const char *d = egt_limit_desc((EgtLimit)i);
        fprintf(stderr, "                         %-10s ", egt_limit_arg((EgtLimit)i));
        for (; *d; d++)
            fputc(*d == '\n' ? ' ' : *d, stderr);
        fputc('\n', stderr);
    }
    fprintf(stderr,
        "      --consistency N  limits: how strongly neighbouring cells keep the\n"
        "                       same colours, 0..300, default 30\n"
        "  -s, --size WxH       resize first (area average), e.g. 320x200,\n"
        "                       or a percentage of the image's size, e.g. 50%%\n"
        "      --nearest        resize with nearest neighbour instead\n"
        "      --auto-contrast  set brightness and contrast so the darkest and\n"
        "                       lightest 0.5%% become black and white\n"
        "      --brightness N   -128..127\n"
        "      --contrast N     -127..127 (below 0: softer, towards mid grey)\n"
        "      --saturation F   -1..1.5\n"
        "      --smooth NAME    none, median, kuwahara or surface (default none)\n"
        "      --radius F       smoothing radius in target pixels, 0.25..4\n"
        "                       (quarter steps below 1, then half steps), default 1\n"
        "      --threshold N    surface smoothing: colour difference still\n"
        "                       averaged, 1..100 levels, default 25\n"
        "      --outline N      ink outline sensitivity 1..100, default off\n"
        "      --outline-mode M basic (edges at target size) or hires (Canny on\n"
        "                       the full-size source, smoother lines)\n"
        "      --outline-dark F outline darkness 0..1, default 1\n"
        "      --outline-min N  drop outline pieces shorter than N pixels\n"
        "      --min-region N   merge smaller regions (checker/none, not with\n"
        "                       --limit), default off\n"
        "  -x, --scale N        save enlarged N times (1..8, pixel-exact), default 1\n"
        "  -v, --verbose        print timing and the saved settings\n"
        "      --version        print the version\n"
        "Output format follows the extension: .png, .bmp or .tga. PNGs record\n"
        "the settings used, as this command line, in a Comment text chunk.\n");
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

static bool parse_float(const char *opt, const char *text, double lo, double hi, float *out)
{
    char *end;
    double v;
    errno = 0;
    v = strtod(text, &end);
    if (end == text || *end || errno || !isfinite(v) || v < lo || v > hi) {
        fprintf(stderr, "%s: '%s' is not a number in %g..%g\n", opt, text, lo, hi);
        return false;
    }
    *out = (float)v;
    return true;
}

/* WxH or N% (1..100). */
static bool parse_size(const char *text, int *w, int *h, int *pct)
{
    char *end;
    long a, b;
    errno = 0;
    a = strtol(text, &end, 10);
    if (end != text && !strcmp(end, "%") && !errno && a >= 1 && a <= 100) {
        *pct = (int)a;
        *w = *h = 0;
        return true;
    }
    if (end != text && *end == 'x' && !errno && a >= 1 && a <= EGT_MAX_SIZE) {
        const char *t = end + 1;
        b = strtol(t, &end, 10);
        if (end != t && !*end && !errno && b >= 1 && b <= EGT_MAX_SIZE) {
            *w = (int)a;
            *h = (int)b;
            return true;
        }
    }
    fprintf(stderr, "bad size '%s' (WxH up to %d each, or 1%%..100%%)\n", text, EGT_MAX_SIZE);
    return false;
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL, *pal_arg = "ega";
    EgtSettings set;
    EgtImage source;
    EgtResult res;
    EgtRunStatus status;
    EgtPalette pal;
    EgtImage out;
    int want_pct = 100, want_w = 0, want_h = 0, verbose = 0, scale = 1, i, k;
    bool auto_contrast = false, ok;
    const char *problem;
    clock_t t0;

    egt_settings_default(&set);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        bool good = true;
#define INT_OPT(name, field) \
        else if (!strcmp(a, name) && next) { good = parse_int(a, next, INT_MIN, INT_MAX, &(field)); i++; }
#define FLOAT_OPT(name, field) \
        else if (!strcmp(a, name) && next) { good = parse_float(a, next, -1e9, 1e9, &(field)); i++; }
        if ((!strcmp(a, "-p") || !strcmp(a, "--palette")) && next) {
            pal_arg = next; i++;
        } else if ((!strcmp(a, "-d") || !strcmp(a, "--dither")) && next) {
            int d;
            for (d = 0; d < EGT_DITHER_COUNT && strcmp(next, egt_dither_arg((EgtDither)d)); d++)
                ;
            if (d == EGT_DITHER_COUNT) {
                fprintf(stderr, "unknown dither '%s'\n", next);
                return 2;
            }
            set.opt.dither = (EgtDither)d; i++;
        } else if ((!strcmp(a, "-b") || !strcmp(a, "--bias")) && next) {
            /* Settings saved by earlier versions: about the same look. */
            int b;
            good = parse_int(a, next, 0, 2500, &b);
            set.opt.prefer_solid = (b + 12) / 25; i++;
        } else if ((!strcmp(a, "-s") || !strcmp(a, "--size")) && next) {
            good = parse_size(next, &want_w, &want_h, &want_pct); i++;
        } else if ((!strcmp(a, "-x") || !strcmp(a, "--scale")) && next) {
            good = parse_int(a, next, 1, 8, &scale); i++;
        }
        INT_OPT("--prefer-solid", set.opt.prefer_solid)
        INT_OPT("--light-mix", set.opt.light_mix)
        INT_OPT("--harsh-mix", set.opt.harsh_mix)
        INT_OPT("--snap", set.opt.snap)
        INT_OPT("--consistency", set.opt.consistency)
        INT_OPT("--min-region", set.opt.min_region)
        INT_OPT("--brightness", set.adj.brightness)
        INT_OPT("--contrast", set.adj.contrast)
        FLOAT_OPT("--saturation", set.adj.saturation)
        INT_OPT("--threshold", set.filt.smooth_threshold)
        FLOAT_OPT("--radius", set.filt.smooth_radius)
        INT_OPT("--outline", set.filt.outline)
        INT_OPT("--outline-min", set.filt.outline_min_length)
        FLOAT_OPT("--outline-dark", set.filt.outline_darkness)
        INT_OPT("--calibrate-max", set.calibrate_max)
        else if (!strcmp(a, "--nearest")) {
            set.nearest = true;
        } else if (!strcmp(a, "--smooth") && next) {
            int m;
            for (m = 0; m < EGT_SMOOTH_COUNT && strcasecmp_ascii(next, egt_smooth_name((EgtSmooth)m)); m++)
                ;
            if (m == EGT_SMOOTH_COUNT) {
                fprintf(stderr, "unknown smoothing '%s'\n", next);
                return 2;
            }
            set.filt.smooth = (EgtSmooth)m; i++;
        } else if (!strcmp(a, "--outline-mode") && next) {
            if (!strcmp(next, "basic"))
                set.filt.outline_mode = EGT_OUTLINE_BASIC;
            else if (!strcmp(next, "hires"))
                set.filt.outline_mode = EGT_OUTLINE_HIRES;
            else {
                fprintf(stderr, "unknown outline mode '%s'\n", next);
                return 2;
            }
            i++;
        } else if (!strcmp(a, "--auto-contrast")) {
            auto_contrast = true;
        } else if (!strcmp(a, "--calibrate")) {
            set.calibrate = true;
        } else if (!strcmp(a, "--limit") && next) {
            int l;
            for (l = 0; l < EGT_LIMIT_COUNT && strcmp(next, egt_limit_arg((EgtLimit)l)); l++)
                ;
            if (l == EGT_LIMIT_COUNT) {
                fprintf(stderr, "unknown limit '%s'\n", next);
                return 2;
            }
            set.opt.limit = (EgtLimit)l; i++;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
            verbose = 1;
        } else if (!strcmp(a, "--version")) {
            printf("egatool-cli %s\n", EGT_VERSION);
            return 0;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "unknown or incomplete option '%s'\n", a);
            usage();
            return 2;
        } else if (!in_path) {
            in_path = a;
        } else if (!out_path) {
            out_path = a;
        } else {
            usage();
            return 2;
        }
        if (!good)
            return 2;
#undef INT_OPT
#undef FLOAT_OPT
    }
    if (!in_path || !out_path) {
        usage();
        return 2;
    }
    /* Ranges, checked before any work (the size is checked once known). */
    set.width = set.height = 1;
    if ((problem = egt_settings_check(&set)) != NULL) {
        fprintf(stderr, "%s\n", problem);
        return 2;
    }

    for (k = 0; k < EGT_PAL_COUNT && strcmp(pal_arg, egt_builtin_palette_arg((EgtBuiltinPalette)k)); k++)
        ;
    if (k < EGT_PAL_COUNT) {
        egt_builtin_palette((EgtBuiltinPalette)k, &pal);
    } else if (!egt_load_jasc_palette(pal_arg, &pal)) {
        fprintf(stderr, "cannot load palette '%s' (expected JASC-PAL)\n", pal_arg);
        return 1;
    }

    source = egt_load_image(in_path);
    if (!source.pixels) {
        fprintf(stderr, "cannot load '%s': %s\n", in_path, egt_load_error());
        return 1;
    }
    if (!want_w) {
        /* The image's own size, or a percentage of it. */
        want_w = (int)lroundf(source.width * (want_pct / 100.0f));
        want_h = (int)lroundf(source.height * (want_pct / 100.0f));
        want_w = want_w < 1 ? 1 : want_w;
        want_h = want_h < 1 ? 1 : want_h;
    }
    set.width = want_w;
    set.height = want_h;
    if ((problem = egt_settings_check(&set)) != NULL) {
        fprintf(stderr, "%s\n", problem);
        egt_image_free(&source);
        return 2;
    }
    if (auto_contrast) {
        /* On the source, like the GUI's Auto button. */
        egt_auto_levels(&source, &set.adj);
        if (verbose)
            fprintf(stderr, "auto contrast: brightness %d, contrast %d\n",
                    set.adj.brightness, set.adj.contrast);
    }

    t0 = clock();
    status = egt_run(&source, 1, &pal, &set, NULL, &res);
    if (status != EGT_RUN_OK) {
        fprintf(stderr, "conversion failed: %s\n", egt_run_message(status));
        egt_result_free(&res);
        egt_image_free(&source);
        return 1;
    }
    for (k = 0; verbose && k < res.nanchors; k++)
        fprintf(stderr, "calibrate: %.1f%% of the image, %d,%d,%d -> %d,%d,%d\n",
                res.anchors[k].share * 100.0, res.anchors[k].from.r, res.anchors[k].from.g,
                res.anchors[k].from.b, res.anchors[k].to.r, res.anchors[k].to.g, res.anchors[k].to.b);
    if (verbose)
        fprintf(stderr, "%dx%d, %s, %d colours%s%s: %.1f ms\n", set.width, set.height,
                egt_dither_name(set.opt.dither), pal.count,
                set.opt.limit ? ", " : "", set.opt.limit ? egt_limit_name(set.opt.limit) : "",
                (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);

    out = res.output;
    if (scale > 1) {
        out = egt_resize_nearest(&res.output, res.output.width * scale, res.output.height * scale);
        if (!out.pixels) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
    }
    {
        EgtRecipe r;
        char *comment;
        memset(&r, 0, sizeof r);
        r.source = base_name(in_path);
        r.output = base_name(out_path);
        r.palette = pal_arg;
        r.set = set; /* after --auto-contrast: the values it picked */
        r.scale = scale;
        comment = egt_recipe_string(&r);
        ok = egt_save_image_comment(out_path, &out, comment);
        if (verbose && comment)
            fprintf(stderr, "settings: %s\n", comment);
        free(comment);
    }
    if (!ok) {
        fprintf(stderr, "cannot write '%s'\n", out_path);
        return 1;
    }
    if (out.pixels != res.output.pixels)
        egt_image_free(&out);
    egt_result_free(&res);
    egt_image_free(&source);
    return 0;
}
