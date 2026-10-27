/*
 * EGAtool command line tool (egatool-cli): converts one image with the
 * given options and saves it with those settings in a PNG comment.
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine.h"
#include "imageio.h"
#include "pipeline.h"
#include "recipe.h"

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
    const char *in_path = NULL, *out_path = NULL;
    EgtRecipe r;
    EgtImage source;
    EgtResult res;
    EgtRunStatus status;
    EgtPalette pal;
    EgtImage out;
    int want_pct = 100, want_w = 0, want_h = 0, verbose = 0, i, k;
    bool auto_contrast = false, ok;
    const char *problem;
    clock_t t0;

    memset(&r, 0, sizeof r);
    r.palette = "ega";
    r.scale = 1;
    egt_settings_default(&r.set);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        int used = egt_recipe_option(&r, a, next);
        if (used < 0)
            return 2;
        if (used > 0) {
            i += used - 1;
        } else if ((!strcmp(a, "-s") || !strcmp(a, "--size")) && next) {
            if (!parse_size(next, &want_w, &want_h, &want_pct))
                return 2;
            i++;
        } else if (!strcmp(a, "--auto-contrast")) {
            auto_contrast = true;
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
    }
    if (!in_path || !out_path) {
        usage();
        return 2;
    }
    /* Ranges, checked before any work (the size is checked once known). */
    r.set.width = r.set.height = 1;
    if ((problem = egt_settings_check(&r.set)) != NULL) {
        fprintf(stderr, "%s\n", problem);
        return 2;
    }

    for (k = 0; k < EGT_PAL_COUNT && strcmp(r.palette, egt_builtin_palette_arg((EgtBuiltinPalette)k)); k++)
        ;
    if (k < EGT_PAL_COUNT) {
        egt_builtin_palette((EgtBuiltinPalette)k, &pal);
    } else if (!egt_load_jasc_palette(r.palette, &pal)) {
        fprintf(stderr, "cannot load palette '%s' (expected JASC-PAL)\n", r.palette);
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
    r.set.width = want_w;
    r.set.height = want_h;
    if ((problem = egt_settings_check(&r.set)) != NULL) {
        fprintf(stderr, "%s\n", problem);
        egt_image_free(&source);
        return 2;
    }
    if (auto_contrast) {
        /* On the source, like the GUI's Auto button. */
        egt_auto_levels(&source, &r.set.adj);
        if (verbose)
            fprintf(stderr, "auto contrast: brightness %d, contrast %d\n",
                    r.set.adj.brightness, r.set.adj.contrast);
    }

    t0 = clock();
    status = egt_run(&source, 1, &pal, &r.set, NULL, &res);
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
        fprintf(stderr, "%dx%d, %s, %d colours%s%s: %.1f ms\n", r.set.width, r.set.height,
                egt_dither_name(r.set.opt.dither), pal.count,
                r.set.opt.limit ? ", " : "", r.set.opt.limit ? egt_limit_name(r.set.opt.limit) : "",
                (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);

    out = res.output;
    if (r.scale > 1) {
        out = egt_resize_nearest(&res.output, res.output.width * r.scale, res.output.height * r.scale);
        if (!out.pixels) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
    }
    {
        /* After --auto-contrast: with the values it picked. */
        char *comment;
        r.source = base_name(in_path);
        r.output = base_name(out_path);
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
