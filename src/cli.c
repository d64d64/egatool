/*
 * egatool-cli: command line front end for the egatool engine.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine.h"
#include "imageio.h"
#include "recipe.h"


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
        "      --saturation F   -1..1\n"
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
        "Output format follows the extension: .png, .bmp or .tga. PNGs record\n"
        "the settings used, as this command line, in a Comment text chunk.\n");
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL, *pal_arg = "ega";
    EgtConvertOptions opt = { .dither = EGT_DITHER_EGA_CHECKER, .prefer_solid = EGT_PREFER_SOLID_DEFAULT,
                              .harsh_mix = 10, .snap = 16,
                              .consistency = 30 };
    EgtAdjust adj = { 0, 0, 0.0f };
    EgtFilters filt = { .smooth = EGT_SMOOTH_NONE, .smooth_radius = 1.0f,
                        .smooth_threshold = 25,
                        .outline_darkness = 1.0f, .outline_mode = EGT_OUTLINE_BASIC };
    EgtImage source, detail = {0, 0, NULL};
    uint8_t *mask = NULL;
    int nearest_resize = 0, want_pct = 100;
    bool auto_contrast = false, ok;
    int calibrate = 0, calibrate_max = 0;
    EgtPalette pal;
    EgtImage img, out;
    int want_w = 0, want_h = 0, verbose = 0, scale = 1, i;
    clock_t t0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
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
            opt.dither = (EgtDither)d; i++;
        } else if (!strcmp(a, "--prefer-solid") && next) {
            opt.prefer_solid = atoi(next); i++;
        } else if ((!strcmp(a, "-b") || !strcmp(a, "--bias")) && next) {
            /* Settings saved by earlier versions: about the same look. */
            opt.prefer_solid = (atoi(next) + 12) / 25; i++;
        } else if (!strcmp(a, "--light-mix") && next) {
            opt.light_mix = atoi(next); i++;
        } else if (!strcmp(a, "--harsh-mix") && next) {
            opt.harsh_mix = atoi(next); i++;
        } else if ((!strcmp(a, "-s") || !strcmp(a, "--size")) && next) {
            char pc;
            int pct;
            if (sscanf(next, "%d%c", &pct, &pc) == 2 && pc == '%') {
                want_pct = pct;
                if (pct < 1 || pct > 100) {
                    fprintf(stderr, "bad size '%s' (1%%..100%%)\n", next);
                    return 2;
                }
                want_w = want_h = 0;
            } else if (sscanf(next, "%dx%d", &want_w, &want_h) != 2 || want_w < 1 || want_h < 1) {
                fprintf(stderr, "bad size '%s'\n", next);
                return 2;
            }
            i++;
        } else if (!strcmp(a, "--brightness") && next) {
            adj.brightness = atoi(next); i++;
        } else if (!strcmp(a, "--contrast") && next) {
            adj.contrast = atoi(next); i++;
        } else if (!strcmp(a, "--saturation") && next) {
            adj.saturation = (float)atof(next); i++;
        } else if (!strcmp(a, "--nearest")) {
            nearest_resize = 1;
        } else if (!strcmp(a, "--smooth") && next) {
            int m;
            for (m = 0; m < EGT_SMOOTH_COUNT && strcasecmp_ascii(next, egt_smooth_name((EgtSmooth)m)); m++)
                ;
            if (m == EGT_SMOOTH_COUNT) {
                fprintf(stderr, "unknown smoothing '%s'\n", next);
                return 2;
            }
            filt.smooth = (EgtSmooth)m; i++;
        } else if (!strcmp(a, "--threshold") && next) {
            filt.smooth_threshold = atoi(next); i++;
        } else if (!strcmp(a, "--radius") && next) {
            filt.smooth_radius = (float)atof(next); i++;
        } else if (!strcmp(a, "--outline") && next) {
            filt.outline = atoi(next); i++;
        } else if (!strcmp(a, "--outline-mode") && next) {
            if (!strcmp(next, "basic"))
                filt.outline_mode = EGT_OUTLINE_BASIC;
            else if (!strcmp(next, "hires"))
                filt.outline_mode = EGT_OUTLINE_HIRES;
            else {
                fprintf(stderr, "unknown outline mode '%s'\n", next);
                return 2;
            }
            i++;
        } else if (!strcmp(a, "--outline-min") && next) {
            filt.outline_min_length = atoi(next); i++;
        } else if (!strcmp(a, "--outline-dark") && next) {
            filt.outline_darkness = (float)atof(next); i++;
        } else if (!strcmp(a, "--auto-contrast")) {
            auto_contrast = true;
        } else if (!strcmp(a, "--calibrate")) {
            calibrate = 1;
        } else if (!strcmp(a, "--calibrate-max") && next) {
            calibrate_max = atoi(next); i++;
        } else if (!strcmp(a, "--limit") && next) {
            int l;
            for (l = 0; l < EGT_LIMIT_COUNT && strcmp(next, egt_limit_arg((EgtLimit)l)); l++)
                ;
            if (l == EGT_LIMIT_COUNT) {
                fprintf(stderr, "unknown limit '%s'\n", next);
                return 2;
            }
            opt.limit = (EgtLimit)l; i++;
        } else if (!strcmp(a, "--consistency") && next) {
            opt.consistency = atoi(next); i++;
        } else if (!strcmp(a, "--snap") && next) {
            opt.snap = atoi(next); i++;
        } else if (!strcmp(a, "--min-region") && next) {
            opt.min_region = atoi(next); i++;
        } else if ((!strcmp(a, "-x") || !strcmp(a, "--scale")) && next) {
            scale = atoi(next); i++;
            if (scale < 1 || scale > 8) {
                fprintf(stderr, "scale must be 1..8\n");
                return 2;
            }
        } else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
            verbose = 1;
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

    {
        int k;
        for (k = 0; k < EGT_PAL_COUNT && strcmp(pal_arg, egt_builtin_palette_arg((EgtBuiltinPalette)k)); k++)
            ;
        if (k < EGT_PAL_COUNT)
            egt_builtin_palette((EgtBuiltinPalette)k, &pal);
        else if (!egt_load_jasc_palette(pal_arg, &pal)) {
            fprintf(stderr, "cannot load palette '%s' (expected JASC-PAL)\n", pal_arg);
            return 1;
        }
    }

    img = egt_load_image(in_path);
    if (!img.pixels) {
        fprintf(stderr, "cannot load '%s': %s\n", in_path, egt_load_error());
        return 1;
    }
    /* img becomes the target-size image; source keeps the original for
     * hi-res outlines. */
    source = img;
    if (!want_w) {
        /* The image's own size, or a percentage of it. */
        want_w = (int)lroundf(source.width * (want_pct / 100.0f));
        want_h = (int)lroundf(source.height * (want_pct / 100.0f));
        if (want_w < 1)
            want_w = 1;
        if (want_h < 1)
            want_h = 1;
    }
    if (want_w != source.width || want_h != source.height || filt.smooth != EGT_SMOOTH_NONE)
        img = egt_resize_smoothed(&source, want_w, want_h, nearest_resize, &filt);
    if (!img.pixels) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    t0 = clock();
    if (opt.dither == EGT_DITHER_EGA_CHECKER && opt.harsh_mix > 0 && opt.limit == EGT_LIMIT_NONE) {
        detail = egt_detail_image(&source, img.width, img.height);
        if (!detail.pixels) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        opt.detail = &detail;
    }
    if (filt.outline > 0 && filt.outline_mode == EGT_OUTLINE_HIRES) {
        mask = egt_outline_mask_hires(&source, img.width, img.height, filt.outline);
        if (!mask) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        filt.outline_mask = mask;
    }
    if (auto_contrast) {
        egt_auto_levels(&img, &adj);
        if (verbose)
            fprintf(stderr, "auto contrast: brightness %d, contrast %d\n",
                    adj.brightness, adj.contrast);
    }
    egt_adjust(&img, &adj);
    if (calibrate > 0 && opt.dither != EGT_DITHER_EGA_CHECKER) {
        EgtCalibAnchor anc[EGT_CALIBRATE_MAX];
        int k, na = egt_calibrate(&img, &pal, calibrate_max, anc);
        if (na < 0) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        for (k = 0; verbose && k < na; k++)
            fprintf(stderr, "calibrate: %.1f%% of the image, %d,%d,%d -> %d,%d,%d\n",
                    anc[k].share * 100.0, anc[k].from.r, anc[k].from.g, anc[k].from.b,
                    anc[k].to.r, anc[k].to.g, anc[k].to.b);
    }
    out = egt_image_alloc(img.width, img.height);
    if (!out.pixels || !egt_filter(&img, &filt)) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if (!egt_convert(&img, &out, &pal, &opt)) {
        fprintf(stderr, "conversion failed (checker mode supports at most %d colours)\n",
                EGT_CHECKER_MAX_COLORS);
        return 1;
    }
    if (verbose)
        fprintf(stderr, "%dx%d, %s, %d colours%s%s: %.1f ms\n", img.width, img.height,
                egt_dither_name(opt.dither), pal.count,
                opt.limit ? ", " : "", opt.limit ? egt_limit_name(opt.limit) : "",
                (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);

    if (scale > 1) {
        EgtImage big = egt_resize_nearest(&out, out.width * scale, out.height * scale);
        if (!big.pixels) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        egt_image_free(&out);
        out = big;
    }
    {
        EgtRecipe r = { 0 };
        char *comment;
        r.source = base_name(in_path);
        r.output = base_name(out_path);
        r.palette = pal_arg;
        r.width = want_w;
        r.height = want_h;
        r.nearest = nearest_resize != 0;
        r.adj = adj; /* after --auto-contrast: the values it picked */
        r.filt = filt;
        r.filt.smooth_radius = egt_snap_radius(filt.smooth_radius);
        r.opt = opt;
        r.scale = scale;
        r.calibrate = calibrate != 0;
        r.calibrate_max = calibrate_max;
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
    if (img.pixels != source.pixels)
        egt_image_free(&source);
    egt_image_free(&img);
    egt_image_free(&out);
    free(mask);
    egt_image_free(&detail);
    return 0;
}
