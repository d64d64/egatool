/*
 * EGAtool engine: built-in palettes, colour adjustments, Solid areas,
 * and the dither modes (checkerboard, ordered, error diffusion, none).
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#include "engine.h"
#include "engine_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The VGA BIOS's default 256-colour palette (mode 13h), from its 6-bit DAC
 * levels: the 16 EGA colours, a 16-step grey ramp, then 24 hues around the
 * colour wheel at 3 brightnesses x 3 saturations, and 8 black. */
static void vga_palette(EgtColor *out)
{
    static const uint8_t EGA6[4] = { 0, 21, 42, 63 };
    static const uint8_t GREY[16] = { 0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63 };
    /* Per brightness, per saturation: the 5 levels a hue ramps through. */
    static const uint8_t RAMP[3][3][5] = {
        { { 0, 16, 31, 47, 63 }, { 31, 39, 47, 55, 63 }, { 45, 49, 54, 58, 63 } },
        { { 0, 7, 14, 21, 28 }, { 14, 17, 21, 24, 28 }, { 20, 22, 24, 26, 28 } },
        { { 0, 4, 8, 12, 16 }, { 8, 10, 12, 14, 16 }, { 11, 12, 13, 15, 16 } },
    };
    uint8_t six[256][3];
    int i, k, br, sat, hue;
    memset(six, 0, sizeof six);
    for (i = 0; i < 16; i++) { /* RGBI, with dark yellow as brown */
        int hi = i & 8 ? 1 : 0;
        six[i][0] = EGA6[(i & 4 ? 2 : 0) + hi];
        six[i][1] = EGA6[(i & 2 ? 2 : 0) + hi];
        six[i][2] = EGA6[(i & 1 ? 2 : 0) + hi];
        if (i == 6)
            six[i][1] = 21;
    }
    for (i = 0; i < 16; i++)
        six[16 + i][0] = six[16 + i][1] = six[16 + i][2] = GREY[i];
    k = 32;
    for (br = 0; br < 3; br++) {
        for (sat = 0; sat < 3; sat++) {
            const uint8_t *L = RAMP[br][sat];
            for (hue = 0; hue < 24; hue++, k++) {
                /* blue -> magenta -> red -> yellow -> green -> cyan -> blue:
                 * one channel ramps at a time while the others sit at the
                 * top or bottom. */
                int seg = hue / 4, step = hue % 4, up = L[step], down = L[4 - step];
                int r = 0, g = 0, b = 0;
                switch (seg) {
                case 0: r = up;   g = L[0]; b = L[4]; break;
                case 1: r = L[4]; g = L[0]; b = down; break;
                case 2: r = L[4]; g = up;   b = L[0]; break;
                case 3: r = down; g = L[4]; b = L[0]; break;
                case 4: r = L[0]; g = L[4]; b = up;   break;
                default: r = L[0]; g = down; b = L[4]; break;
                }
                six[k][0] = (uint8_t)r;
                six[k][1] = (uint8_t)g;
                six[k][2] = (uint8_t)b;
            }
        }
    }
    for (i = 0; i < 256; i++) { /* 6-bit DAC level to 8 bits */
        out[i].r = (uint8_t)((six[i][0] * 255 + 31) / 63);
        out[i].g = (uint8_t)((six[i][1] * 255 + 31) / 63);
        out[i].b = (uint8_t)((six[i][2] * 255 + 31) / 63);
    }
}


static const EgtColor EGA_COLORS[16] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
    {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
    {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
};

/* CGA mode 4, palette 1 high intensity. */
static const EgtColor CGA_COLORS[4] = {
    {0x00, 0x00, 0x00}, {0xFF, 0x55, 0xFF}, {0x55, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF},
};

/* CGA mode 4, palette 0 high intensity (the warm one). */
static const EgtColor CGA_WARM_COLORS[4] = {
    {0x00, 0x00, 0x00}, {0x55, 0xFF, 0x55}, {0xFF, 0x55, 0x55}, {0xFF, 0xFF, 0x55},
};

/* Commodore 64, Philip "Pepto" Timmermann's measured palette. */
static const EgtColor C64_COLORS[16] = {
    {0x00, 0x00, 0x00}, {0xFF, 0xFF, 0xFF}, {0x68, 0x37, 0x2B}, {0x70, 0xA4, 0xB2},
    {0x6F, 0x3D, 0x86}, {0x58, 0x8D, 0x43}, {0x35, 0x28, 0x79}, {0xB8, 0xC7, 0x6F},
    {0x6F, 0x4F, 0x25}, {0x43, 0x39, 0x00}, {0x9A, 0x67, 0x59}, {0x44, 0x44, 0x44},
    {0x6C, 0x6C, 0x6C}, {0x9A, 0xD2, 0x84}, {0x6C, 0x5E, 0xB5}, {0x95, 0x95, 0x95},
};

/* ZX Spectrum: black, then the 7 colours at normal (0xD7) and bright
 * (0xFF) intensity. */
static const EgtColor SPECTRUM_COLORS[15] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xD7}, {0xD7, 0x00, 0x00}, {0xD7, 0x00, 0xD7},
    {0x00, 0xD7, 0x00}, {0x00, 0xD7, 0xD7}, {0xD7, 0xD7, 0x00}, {0xD7, 0xD7, 0xD7},
    {0x00, 0x00, 0xFF}, {0xFF, 0x00, 0x00}, {0xFF, 0x00, 0xFF}, {0x00, 0xFF, 0x00},
    {0x00, 0xFF, 0xFF}, {0xFF, 0xFF, 0x00}, {0xFF, 0xFF, 0xFF},
};

/* MSX1 / TMS9918 (also ColecoVision, SG-1000, TI-99/4A); colour 0 is
 * transparent on the hardware. */
static const EgtColor MSX_COLORS[15] = {
    {0x00, 0x00, 0x00}, {0x21, 0xC8, 0x42}, {0x5E, 0xDC, 0x78}, {0x54, 0x55, 0xED},
    {0x7D, 0x76, 0xFC}, {0xD4, 0x52, 0x4D}, {0x42, 0xEB, 0xF5}, {0xFC, 0x55, 0x54},
    {0xFF, 0x79, 0x78}, {0xD4, 0xC1, 0x54}, {0xE6, 0xCE, 0x80}, {0x21, 0xB0, 0x3B},
    {0xC9, 0x5B, 0xBA}, {0xCC, 0xCC, 0xCC}, {0xFF, 0xFF, 0xFF},
};

/* Apple II high resolution: black, green, violet, orange, blue, white. */
static const EgtColor APPLE2_COLORS[6] = {
    {0x00, 0x00, 0x00}, {0x14, 0xF5, 0x3C}, {0xFF, 0x44, 0xFD}, {0xFF, 0x6A, 0x3C},
    {0x14, 0xCF, 0xFD}, {0xFF, 0xFF, 0xFF},
};

/* Original Game Boy (DMG): the four shades of its green LCD. */
static const EgtColor GAMEBOY_COLORS[4] = {
    {0x0F, 0x38, 0x0F}, {0x30, 0x62, 0x30}, {0x8B, 0xAC, 0x0F}, {0x9B, 0xBC, 0x0F},
};

/* NES (2C02): the common measured palette, duplicate blacks removed. */
static const EgtColor NES_COLORS[55] = {
    {0x7C, 0x7C, 0x7C}, {0x00, 0x00, 0xFC}, {0x00, 0x00, 0xBC}, {0x44, 0x28, 0xBC},
    {0x94, 0x00, 0x84}, {0xA8, 0x00, 0x20}, {0xA8, 0x10, 0x00}, {0x88, 0x14, 0x00},
    {0x50, 0x30, 0x00}, {0x00, 0x78, 0x00}, {0x00, 0x68, 0x00}, {0x00, 0x58, 0x00},
    {0x00, 0x40, 0x58}, {0x00, 0x00, 0x00}, {0xBC, 0xBC, 0xBC}, {0x00, 0x78, 0xF8},
    {0x00, 0x58, 0xF8}, {0x68, 0x44, 0xFC}, {0xD8, 0x00, 0xCC}, {0xE4, 0x00, 0x58},
    {0xF8, 0x38, 0x00}, {0xE4, 0x5C, 0x10}, {0xAC, 0x7C, 0x00}, {0x00, 0xB8, 0x00},
    {0x00, 0xA8, 0x00}, {0x00, 0xA8, 0x44}, {0x00, 0x88, 0x88}, {0xF8, 0xF8, 0xF8},
    {0x3C, 0xBC, 0xFC}, {0x68, 0x88, 0xFC}, {0x98, 0x78, 0xF8}, {0xF8, 0x78, 0xF8},
    {0xF8, 0x58, 0x98}, {0xF8, 0x78, 0x58}, {0xFC, 0xA0, 0x44}, {0xF8, 0xB8, 0x00},
    {0xB8, 0xF8, 0x18}, {0x58, 0xD8, 0x54}, {0x58, 0xF8, 0x98}, {0x00, 0xE8, 0xD8},
    {0x78, 0x78, 0x78}, {0xFC, 0xFC, 0xFC}, {0xA4, 0xE4, 0xFC}, {0xB8, 0xB8, 0xF8},
    {0xD8, 0xB8, 0xF8}, {0xF8, 0xB8, 0xF8}, {0xF8, 0xA4, 0xC0}, {0xF0, 0xD0, 0xB0},
    {0xFC, 0xE0, 0xA8}, {0xF8, 0xD8, 0x78}, {0xD8, 0xF8, 0x78}, {0xB8, 0xF8, 0xB8},
    {0xB8, 0xF8, 0xD8}, {0x00, 0xFC, 0xFC}, {0xF8, 0xD8, 0xF8},
};

/* PICO-8 fantasy console. */
static const EgtColor PICO8_COLORS[16] = {
    {0x00, 0x00, 0x00}, {0x1D, 0x2B, 0x53}, {0x7E, 0x25, 0x53}, {0x00, 0x87, 0x51},
    {0xAB, 0x52, 0x36}, {0x5F, 0x57, 0x4F}, {0xC2, 0xC3, 0xC7}, {0xFF, 0xF1, 0xE8},
    {0xFF, 0x00, 0x4D}, {0xFF, 0xA3, 0x00}, {0xFF, 0xEC, 0x27}, {0x00, 0xE4, 0x36},
    {0x29, 0xAD, 0xFF}, {0x83, 0x76, 0x9C}, {0xFF, 0x77, 0xA8}, {0xFF, 0xCC, 0xAA},
};

/* DawnBringer 32 by Richard "DawnBringer" Fhager. */
static const EgtColor DB32_COLORS[32] = {
    {0x00, 0x00, 0x00}, {0x22, 0x20, 0x34}, {0x45, 0x28, 0x3c}, {0x66, 0x39, 0x31},
    {0x8f, 0x56, 0x3b}, {0xdf, 0x71, 0x26}, {0xd9, 0xa0, 0x66}, {0xee, 0xc3, 0x9a},
    {0xfb, 0xf2, 0x36}, {0x99, 0xe5, 0x50}, {0x6a, 0xbe, 0x30}, {0x37, 0x94, 0x6e},
    {0x4b, 0x69, 0x2f}, {0x52, 0x4b, 0x24}, {0x32, 0x3c, 0x39}, {0x3f, 0x3f, 0x74},
    {0x30, 0x60, 0x82}, {0x5b, 0x6e, 0xe1}, {0x63, 0x9b, 0xff}, {0x5f, 0xcd, 0xe4},
    {0xcb, 0xdb, 0xfc}, {0xff, 0xff, 0xff}, {0x9b, 0xad, 0xb7}, {0x84, 0x7e, 0x87},
    {0x69, 0x6a, 0x6a}, {0x59, 0x56, 0x52}, {0x76, 0x42, 0x8a}, {0xac, 0x32, 0x32},
    {0xd9, 0x57, 0x63}, {0xd7, 0x7b, 0xba}, {0x8f, 0x97, 0x4a}, {0x8a, 0x6f, 0x30},
};

static const char *DITHER_NAMES[EGT_DITHER_COUNT] = {
    "Checkerboard", "Ordered", "Burkes", "Floyd-Steinberg",
    "Sierra", "Stucki", "None",
};

static const char *PALETTE_NAMES[EGT_PAL_COUNT] = {
    "EGA", "VGA", "CGA", "CGA warm", "EGA 64", "Amstrad CPC", "Commodore 64",
    "ZX Spectrum", "MSX", "Apple II hi-res", "Game Boy", "NES", "DawnBringer 32", "PICO-8",
};

const char *egt_dither_name(EgtDither d)
{
    return (d >= 0 && d < EGT_DITHER_COUNT) ? DITHER_NAMES[d] : "?";
}

const char *egt_builtin_palette_name(EgtBuiltinPalette p)
{
    return (p >= 0 && p < EGT_PAL_COUNT) ? PALETTE_NAMES[p] : "?";
}

void egt_builtin_palette(EgtBuiltinPalette which, EgtPalette *out)
{
    const EgtColor *src;
    int n, i;
    switch (which) {
    case EGT_PAL_EGA64:
        /* In EGA register order: bits 0-2 are blue, green, red at 0xAA,
         * bits 3-5 add 0x55 to them. */
        out->count = 64;
        for (i = 0; i < 64; i++) {
            out->colors[i].r = (uint8_t)((i >> 2 & 1) * 0xAA + (i >> 5 & 1) * 0x55);
            out->colors[i].g = (uint8_t)((i >> 1 & 1) * 0xAA + (i >> 4 & 1) * 0x55);
            out->colors[i].b = (uint8_t)((i & 1) * 0xAA + (i >> 3 & 1) * 0x55);
        }
        return;
    case EGT_PAL_CPC:
        /* In firmware colour order: number = 9 * green + 3 * red + blue,
         * each 0, 1 or 2 (off, half, full). */
        out->count = 27;
        for (i = 0; i < 27; i++) {
            static const uint8_t LEVEL[3] = { 0x00, 0x80, 0xFF };
            out->colors[i].r = LEVEL[i / 3 % 3];
            out->colors[i].g = LEVEL[i / 9];
            out->colors[i].b = LEVEL[i % 3];
        }
        return;
    case EGT_PAL_C64: src = C64_COLORS; n = 16; break;
    case EGT_PAL_SPECTRUM: src = SPECTRUM_COLORS; n = 15; break;
    case EGT_PAL_MSX: src = MSX_COLORS; n = 15; break;
    case EGT_PAL_APPLE2: src = APPLE2_COLORS; n = 6; break;
    case EGT_PAL_GAMEBOY: src = GAMEBOY_COLORS; n = 4; break;
    case EGT_PAL_NES: src = NES_COLORS; n = (int)(sizeof NES_COLORS / sizeof NES_COLORS[0]); break;
    case EGT_PAL_DB32: src = DB32_COLORS; n = 32; break;
    case EGT_PAL_PICO8: src = PICO8_COLORS; n = 16; break;
    case EGT_PAL_VGA:
        out->count = 256;
        vga_palette(out->colors);
        return;
    case EGT_PAL_CGA: src = CGA_COLORS; n = 4; break;
    case EGT_PAL_CGA_WARM: src = CGA_WARM_COLORS; n = 4; break;
    case EGT_PAL_EGA:
    default:          src = EGA_COLORS; n = 16; break;
    }
    out->count = n;
    memcpy(out->colors, src, (size_t)n * sizeof(EgtColor));
}

bool egt_load_jasc_palette(const char *path, EgtPalette *out)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int n, i;
    EgtPalette pal;

    if (!f)
        return false;
    if (!fgets(line, sizeof line, f) || strncmp(line, "JASC-PAL", 8) != 0 ||
        !fgets(line, sizeof line, f) || strncmp(line, "0100", 4) != 0 ||
        !fgets(line, sizeof line, f) || sscanf(line, "%d", &n) != 1 ||
        n < 1 || n > EGT_MAX_COLORS) {
        fclose(f);
        return false;
    }
    for (i = 0; i < n; i++) {
        int r, g, b;
        if (!fgets(line, sizeof line, f) || sscanf(line, "%d %d %d", &r, &g, &b) != 3 ||
            r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
            fclose(f);
            return false;
        }
        pal.colors[i].r = (uint8_t)r;
        pal.colors[i].g = (uint8_t)g;
        pal.colors[i].b = (uint8_t)b;
    }
    fclose(f);
    pal.count = n;
    *out = pal;
    return true;
}

EgtImage egt_image_alloc(int width, int height)
{
    EgtImage img;
    img.width = width;
    img.height = height;
    img.pixels = malloc((size_t)width * (size_t)height * 4);
    if (!img.pixels)
        img.width = img.height = 0;
    return img;
}

EgtImage egt_image_copy(const EgtImage *src)
{
    EgtImage img = egt_image_alloc(src->width, src->height);
    if (img.pixels)
        memcpy(img.pixels, src->pixels, (size_t)src->width * (size_t)src->height * 4);
    return img;
}

void egt_image_free(EgtImage *img)
{
    free(img->pixels);
    img->pixels = NULL;
    img->width = img->height = 0;
}

EgtImage egt_resize_nearest(const EgtImage *src, int width, int height)
{
    EgtImage dst = egt_image_alloc(width, height);
    int x, y;
    if (!dst.pixels)
        return dst;
    for (y = 0; y < height; y++) {
        int sy = (int)(((int64_t)y * src->height) / height);
        const uint32_t *srow = (const uint32_t *)src->pixels + (size_t)sy * src->width;
        uint32_t *drow = (uint32_t *)dst.pixels + (size_t)y * width;
        for (x = 0; x < width; x++)
            drow[x] = srow[((int64_t)x * src->width) / width];
    }
    return dst;
}

static uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* ---- OKLab (Björn Ottosson), for perceptually even saturation ---------- */

static float srgb_to_linear_lut[256];
static uint8_t linear_to_srgb_lut[4096];

static void init_srgb_luts(void)
{
    static bool done;
    int i;
    if (done)
        return;
    for (i = 0; i < 256; i++) {
        double c = i / 255.0;
        srgb_to_linear_lut[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
    }
    for (i = 0; i < 4096; i++) {
        double c = i / 4095.0;
        c = c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055;
        linear_to_srgb_lut[i] = clamp8((int)lround(c * 255));
    }
    done = true;
}

static uint8_t linear_to_srgb8(float c)
{
    if (c <= 0)
        return 0;
    if (c >= 1)
        return 255;
    return linear_to_srgb_lut[(int)(c * 4095 + 0.5f)];
}

static void linear_to_oklab(const float *rgb, float *lab)
{
    float l = cbrtf(0.4122214708f * rgb[0] + 0.5363325363f * rgb[1] + 0.0514459929f * rgb[2]);
    float m = cbrtf(0.2119034982f * rgb[0] + 0.6806995451f * rgb[1] + 0.1073969566f * rgb[2]);
    float s = cbrtf(0.0883024619f * rgb[0] + 0.2817188376f * rgb[1] + 0.6299787005f * rgb[2]);
    lab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    lab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    lab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

static void oklab_to_linear(const float *lab, float *rgb)
{
    float l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    float m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    float s = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    rgb[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    rgb[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    rgb[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

/* Scales chroma by k at constant perceived lightness and hue. Where that
 * leaves the sRGB gamut, the largest chroma that fits is used instead of
 * clipping channels (clipping shifts hue and lightness). */
static void saturate_pixel(uint8_t *p, float k)
{
    float rgb[3], lab[3], t[3], lo = 1.0f, hi = k;
    int i, c;
    for (c = 0; c < 3; c++)
        rgb[c] = srgb_to_linear_lut[p[c]];
    linear_to_oklab(rgb, lab);
    t[0] = lab[0];
    t[1] = lab[1] * k;
    t[2] = lab[2] * k;
    oklab_to_linear(t, rgb);
    if (k > 1 && (rgb[0] < -1e-4f || rgb[1] < -1e-4f || rgb[2] < -1e-4f ||
                  rgb[0] > 1.0001f || rgb[1] > 1.0001f || rgb[2] > 1.0001f)) {
        /* Binary search for the gamut edge between the original chroma
         * (in gamut) and the requested one. */
        for (i = 0; i < 10; i++) {
            float mid = (lo + hi) / 2;
            t[1] = lab[1] * mid;
            t[2] = lab[2] * mid;
            oklab_to_linear(t, rgb);
            if (rgb[0] < -1e-4f || rgb[1] < -1e-4f || rgb[2] < -1e-4f ||
                rgb[0] > 1.0001f || rgb[1] > 1.0001f || rgb[2] > 1.0001f)
                hi = mid;
            else
                lo = mid;
        }
        t[1] = lab[1] * lo;
        t[2] = lab[2] * lo;
        oklab_to_linear(t, rgb);
    }
    for (c = 0; c < 3; c++)
        p[c] = linear_to_srgb8(rgb[c]);
}

void egt_adjust(EgtImage *img, const EgtAdjust *adj)
{
    uint8_t lut[256];
    size_t i, n = (size_t)img->width * (size_t)img->height;
    int v;
    int c = adj->contrast < -127 ? -127 : adj->contrast > 127 ? 127 : adj->contrast;
    float k = 1.0f + adj->saturation;

    /* Brightness shifts levels. Contrast c > 0 stretches [c, 255-c] to
     * [0, 255]; c < 0 squeezes [0, 255] into [-c, 255+c], towards mid grey. */
    for (v = 0; v < 256; v++) {
        int t = v + adj->brightness;
        if (c >= 0)
            t = (t - c) * 255 / (255 - 2 * c);
        else
            t = -c + (int)lround(t * (255.0 + 2 * c) / 255.0);
        lut[v] = clamp8(t);
    }

    if (k < 0)
        k = 0;
    init_srgb_luts();
    for (i = 0; i < n; i++) {
        uint8_t *p = img->pixels + i * 4;
        p[0] = lut[p[0]];
        p[1] = lut[p[1]];
        p[2] = lut[p[2]];
        if (k != 1.0f)
            saturate_pixel(p, k);
    }
}

void egt_auto_levels(const EgtImage *img, EgtAdjust *adj)
{
    size_t hist[256] = { 0 }, i, n = (size_t)img->width * (size_t)img->height;
    size_t clip, sum;
    int lo = 0, hi = 255, c;
    for (i = 0; i < n; i++)
        for (c = 0; c < 3; c++)
            hist[img->pixels[i * 4 + c]]++;
    clip = n * 3 / 200; /* 0.5% */
    for (sum = 0; lo < 255 && (sum += hist[lo]) <= clip; lo++)
        ;
    for (sum = 0; hi > 0 && (sum += hist[hi]) <= clip; hi--)
        ;
    if (hi - lo < 16) { /* nearly flat: stretching would only amplify noise */
        adj->brightness = adj->contrast = 0;
        return;
    }
    /* egt_adjust maps v to (v + b - c) * 255 / (255 - 2c): solve for
     * lo -> 0 and hi -> 255. */
    adj->brightness = (int)lround((255 - hi - lo) / 2.0);
    adj->contrast = (int)lround((255 - hi + lo) / 2.0);
    if (adj->contrast > 127)
        adj->contrast = 127;
}

/* Index of the palette colour nearest in RGB. */
static int nearest(const EgtPalette *pal, int r, int g, int b)
{
    int i, best = 0, best_d = 0x7fffffff;
    for (i = 0; i < pal->count; i++) {
        int dr = r - pal->colors[i].r;
        int dg = g - pal->colors[i].g;
        int db = b - pal->colors[i].b;
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

/* ---- calibration to solid areas ---------------------------------------- */

#define CAL_NEIGHBOUR_DIFF 6   /* max channel step between neighbours of a flat area */
#define CAL_MEAN_DIFF 24       /* max channel distance from the area's running mean */
#define CAL_MERGE_DIST 16.0    /* areas whose colours are this close count as one */
#define CAL_MIN_SHARE 0.01     /* smallest area used, as a share of the image */
#define CAL_REACH 64.0         /* colours this far from an area's colour move half as much */

typedef struct {
    double sum[3];
    long count;
} CalArea;

/* How far a colour at squared distance d2 from an area's colour moves
 * along with it, 0..1 (1 at the area's colour itself). */
static double cal_follow(double d2)
{
    return 1.0 / (1.0 + d2 / (CAL_REACH * CAL_REACH));
}

static void srgb_to_oklab(double r, double g, double b, float *lab)
{
    float lin[3];
    double v[3];
    int c;
    v[0] = r / 255.0;
    v[1] = g / 255.0;
    v[2] = b / 255.0;
    for (c = 0; c < 3; c++)
        lin[c] = (float)(v[c] <= 0.04045 ? v[c] / 12.92 : pow((v[c] + 0.055) / 1.055, 2.4));
    linear_to_oklab(lin, lab);
}

/* Palette colour that looks closest (OKLab): a mid green area should become
 * green, not the dark grey that is nearer in RGB. */
static int nearest_perceptual(const EgtPalette *pal, const double *rgb)
{
    float q[3], t[3];
    int i, best = 0;
    double best_d = 1e30;
    srgb_to_oklab(rgb[0], rgb[1], rgb[2], q);
    for (i = 0; i < pal->count; i++) {
        double d;
        srgb_to_oklab(pal->colors[i].r, pal->colors[i].g, pal->colors[i].b, t);
        d = (double)(t[0] - q[0]) * (t[0] - q[0]) + (double)(t[1] - q[1]) * (t[1] - q[1])
          + (double)(t[2] - q[2]) * (t[2] - q[2]);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

int egt_calibrate(EgtImage *img, const EgtPalette *pal, int max_shift, EgtCalibAnchor *anchors)
{
    const int areas = EGT_CALIBRATE_MAX;
    int w = img->width, h = img->height, nanchor = 0, i, j, c;
    size_t n = (size_t)w * (size_t)h, p, top;
    int32_t *label = NULL, *stack = NULL;
    CalArea *list = NULL, best[EGT_CALIBRATE_MAX * 4];
    size_t nlist = 0, cap = 0;
    double from[EGT_CALIBRATE_MAX][3], shift[EGT_CALIBRATE_MAX][3];
    EgtColor to[EGT_CALIBRATE_MAX];
    long min_count;
    int nbest = 0;

    if (pal->count < 1 || n == 0)
        return 0;
    min_count = (long)(n * CAL_MIN_SHARE);
    if (min_count < 1)
        min_count = 1;
    label = malloc(n * sizeof *label);
    stack = malloc(n * sizeof *stack);
    if (!label || !stack)
        goto fail;
    for (p = 0; p < n; p++)
        label[p] = -1;

    /* Flood fill flat areas: neighbours step at most CAL_NEIGHBOUR_DIFF per
     * channel and stay near the area's running mean (so a long smooth
     * gradient doesn't become one area). Only large ones are kept. */
    for (p = 0; p < n; p++) {
        CalArea a = { { 0, 0, 0 }, 0 };
        if (label[p] >= 0)
            continue;
        top = 0;
        stack[top++] = (int32_t)p;
        label[p] = 1;
        while (top) {
            int32_t q = stack[--top];
            int x = q % w, y = q / w, k;
            const uint8_t *qc = img->pixels + (size_t)q * 4;
            for (c = 0; c < 3; c++)
                a.sum[c] += qc[c];
            a.count++;
            for (k = 0; k < 4; k++) {
                int nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3);
                int32_t r;
                const uint8_t *rc;
                bool ok = true;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                    continue;
                r = ny * w + nx;
                if (label[r] >= 0)
                    continue;
                rc = img->pixels + (size_t)r * 4;
                for (c = 0; c < 3 && ok; c++)
                    ok = abs(rc[c] - qc[c]) <= CAL_NEIGHBOUR_DIFF &&
                         fabs(rc[c] - a.sum[c] / a.count) <= CAL_MEAN_DIFF;
                if (ok) {
                    label[r] = 1;
                    stack[top++] = r;
                }
            }
        }
        if (a.count < min_count / 4)
            continue; /* too small even to add up with similar areas */
        if (nlist == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            CalArea *nl = realloc(list, ncap * sizeof *nl);
            if (!nl)
                goto fail;
            list = nl;
            cap = ncap;
        }
        list[nlist++] = a;
    }
    free(label);
    free(stack);
    label = stack = NULL;

    /* Merge areas of similar colour (the sky split by a tree), largest
     * first, then keep the largest. */
    for (;;) {
        size_t bi = 0;
        long bc = 0;
        for (p = 0; p < nlist; p++)
            if (list[p].count > bc) {
                bc = list[p].count;
                bi = p;
            }
        if (bc == 0)
            break;
        for (p = 0; p < nlist; p++) {
            double d = 0;
            if (p == bi || list[p].count == 0)
                continue;
            for (c = 0; c < 3; c++) {
                double t = list[p].sum[c] / list[p].count - list[bi].sum[c] / list[bi].count;
                d += t * t;
            }
            if (sqrt(d) <= CAL_MERGE_DIST) {
                for (c = 0; c < 3; c++)
                    list[bi].sum[c] += list[p].sum[c];
                list[bi].count += list[p].count;
                list[p].count = 0;
            }
        }
        if (list[bi].count >= min_count && nbest < (int)(sizeof best / sizeof best[0]))
            best[nbest++] = list[bi];
        list[bi].count = 0;
    }
    free(list);
    list = NULL;
    /* Merging can make a later area larger than an earlier one. */
    for (i = 1; i < nbest; i++)
        for (j = i; j > 0 && best[j].count > best[j - 1].count; j--) {
            CalArea t = best[j];
            best[j] = best[j - 1];
            best[j - 1] = t;
        }

    for (i = 0; i < nbest && nanchor < areas; i++) {
        int k;
        double own = 0, other = 0;
        for (c = 0; c < 3; c++)
            from[nanchor][c] = best[i].sum[c] / best[i].count;
        k = nearest_perceptual(pal, from[nanchor]);
        to[nanchor] = pal->colors[k];
        shift[nanchor][0] = to[nanchor].r - from[nanchor][0];
        shift[nanchor][1] = to[nanchor].g - from[nanchor][1];
        shift[nanchor][2] = to[nanchor].b - from[nanchor][2];
        /* Optionally only modest changes: a blue sky turning CGA red or
         * magenta is a big one (it can still look right with 4 colours). */
        if (max_shift > 0 &&
            sqrt(shift[nanchor][0] * shift[nanchor][0] + shift[nanchor][1] * shift[nanchor][1] +
                 shift[nanchor][2] * shift[nanchor][2]) > max_shift)
            continue;
        /* Use the area only if it moves the rest of the image less than
         * it moves the area's own colours. A brick wall whose nearest
         * palette colour is grey would drag every brick in the picture
         * towards grey; a pale sky becoming white barely touches anything
         * else. */
        for (p = 0; p < n; p++) {
            const uint8_t *px = img->pixels + p * 4;
            double d2 = 0, f;
            for (c = 0; c < 3; c++) {
                double t = px[c] - from[nanchor][c];
                d2 += t * t;
            }
            /* How much of the move this pixel would follow, given the
             * areas already chosen: colours near one of those (dark
             * leaves near black) stay with it. */
            f = cal_follow(d2);
            if (nanchor > 0 && d2 > 0.25) {
                double den = 1.0 / (CAL_REACH * CAL_REACH) + 1.0 / d2;
                for (j = 0; j < nanchor; j++) {
                    double e2 = 0;
                    for (c = 0; c < 3; c++) {
                        double t = px[c] - from[j][c];
                        e2 += t * t;
                    }
                    den += 1.0 / (e2 > 0.25 ? e2 : 0.25);
                }
                f = (1.0 / d2) / den;
            }
            if (d2 <= CAL_MEAN_DIFF * CAL_MEAN_DIFF)
                own += f;
            else
                other += f;
        }
        if (other >= own)
            continue;
        if (anchors) {
            anchors[nanchor].from.r = (uint8_t)lround(from[nanchor][0]);
            anchors[nanchor].from.g = (uint8_t)lround(from[nanchor][1]);
            anchors[nanchor].from.b = (uint8_t)lround(from[nanchor][2]);
            anchors[nanchor].to = to[nanchor];
            anchors[nanchor].share = (float)((double)best[i].count / n);
        }
        nanchor++;
    }

    /* Each colour moves by a weighted blend of the areas' shifts and "no
     * shift": weights 1/d^2 to each area's colour (so an area's colour
     * moves exactly onto its palette colour) and 1/CAL_REACH^2 for staying
     * put. Distant colours stay; nothing overshoots. */
    if (nanchor > 0) {
        for (p = 0; p < n; p++) {
            uint8_t *px = img->pixels + p * 4;
            double num[3] = { 0, 0, 0 }, den = 1.0 / (CAL_REACH * CAL_REACH);
            int exact = -1;
            for (i = 0; i < nanchor; i++) {
                double d2 = 0, wgt;
                for (c = 0; c < 3; c++) {
                    double t = px[c] - from[i][c];
                    d2 += t * t;
                }
                if (d2 < 0.25) {
                    exact = i;
                    break;
                }
                wgt = 1.0 / d2;
                for (c = 0; c < 3; c++)
                    num[c] += wgt * shift[i][c];
                den += wgt;
            }
            if (exact >= 0) {
                px[0] = to[exact].r;
                px[1] = to[exact].g;
                px[2] = to[exact].b;
            } else {
                for (c = 0; c < 3; c++)
                    px[c] = clamp8((int)lround(px[c] + num[c] / den));
            }
        }
    }
    return nanchor;

fail:
    free(label);
    free(stack);
    free(list);
    return -1;
}

static void put(uint8_t *p, EgtColor c)
{
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = 255;
}

/* Groups pixels into regions of 8-connected equal labels. order[] lists
 * pixels grouped by region; region r is order[start[r] .. start[r+1]-1].
 * region[] gets each pixel's region index. Returns the region count. */
static size_t find_regions(const uint16_t *lab, int w, int h, int32_t *region,
                           int32_t *order, size_t *start)
{
    size_t n = (size_t)w * (size_t)h, p, head, tail = 0, nreg = 0;
    int dx, dy;

    /* Flood fill, using order[] itself as the queue. */
    memset(region, 0xff, n * sizeof(int32_t));
    for (p = 0; p < n; p++) {
        if (region[p] >= 0)
            continue;
        start[nreg] = tail;
        region[p] = (int32_t)nreg;
        order[tail++] = (int32_t)p;
        for (head = start[nreg]; head < tail; head++) {
            int qx = order[head] % w, qy = order[head] / w;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int nx = qx + dx, ny = qy + dy;
                    size_t nb = (size_t)ny * w + nx;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    if (region[nb] < 0 && lab[nb] == lab[p]) {
                        region[nb] = (int32_t)nreg;
                        order[tail++] = (int32_t)nb;
                    }
                }
            }
        }
        nreg++;
    }
    start[nreg] = tail;
    return nreg;
}

/* A small region is only merged if at least this share (percent) of its
 * border touches one label, i.e. it sits inside a surface. Outlines and
 * edges between two areas touch several labels and are kept. */
#define CLEANUP_ENCLOSED_PCT 70

/* Merges regions (8-connected pixels with the same label) smaller than
 * min_region that are enclosed by a single surrounding label into that
 * label. Several passes, so a speck made of two touching colours gets
 * removed once its first part is merged. */
static bool cleanup_regions(uint16_t *lab, int w, int h, int nlabels, int min_region)
{
    size_t n = (size_t)w * (size_t)h;
    int32_t *region = malloc(n * sizeof(int32_t));
    int32_t *order = malloc(n * sizeof(int32_t)); /* pixels grouped by region */
    size_t *start = malloc((n + 1) * sizeof(size_t));
    int *counts = calloc((size_t)nlabels, sizeof(int));
    uint16_t *touched = malloc((size_t)nlabels * sizeof(uint16_t));
    bool ok = region && order && start && counts && touched;
    int pass, dx, dy;

    for (pass = 0; ok && pass < 3; pass++) {
        size_t nreg = find_regions(lab, w, h, region, order, start), r;
        bool changed = false;

        for (r = 0; r < nreg; r++) {
            size_t i;
            int ntouched = 0, best = -1, best_count = 0, total = 0, t;
            uint16_t own;
            if (start[r + 1] - start[r] >= (size_t)min_region)
                continue;
            own = lab[order[start[r]]];
            for (i = start[r]; i < start[r + 1]; i++) {
                int qx = order[i] % w, qy = order[i] / w;
                for (dy = -1; dy <= 1; dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        int nx = qx + dx, ny = qy + dy;
                        uint16_t l;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                            continue;
                        l = lab[(size_t)ny * w + nx];
                        if (l != own && counts[l]++ == 0)
                            touched[ntouched++] = l;
                    }
                }
            }
            for (t = 0; t < ntouched; t++) {
                if (counts[touched[t]] > best_count) {
                    best_count = counts[touched[t]];
                    best = touched[t];
                }
                total += counts[touched[t]];
                counts[touched[t]] = 0;
            }
            if (best >= 0 && best_count * 100 >= total * CLEANUP_ENCLOSED_PCT) {
                for (i = start[r]; i < start[r + 1]; i++)
                    lab[order[i]] = (uint16_t)best;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    free(region);
    free(order);
    free(start);
    free(counts);
    free(touched);
    return ok;
}

bool egt_dither_supports_cleanup(EgtDither d)
{
    return d == EGT_DITHER_EGA_CHECKER || d == EGT_DITHER_NONE;
}

/* None: every pixel becomes its nearest palette colour, then specks under
 * min_region pixels are cleaned up. */
static bool convert_none(const EgtImage *src, EgtImage *dst, const EgtPalette *pal,
                         int min_region)
{
    size_t i, n = (size_t)src->width * (size_t)src->height;
    uint16_t *lab = malloc(n * sizeof(uint16_t));
    if (!lab)
        return false;
    for (i = 0; i < n; i++) {
        const uint8_t *s = src->pixels + i * 4;
        lab[i] = (uint16_t)nearest(pal, s[0], s[1], s[2]);
    }
    if (min_region > 1 && !cleanup_regions(lab, src->width, src->height, pal->count, min_region)) {
        free(lab);
        return false;
    }
    for (i = 0; i < n; i++)
        put(dst->pixels + i * 4, pal->colors[lab[i]]);
    free(lab);
    return true;
}

/* How much dithering a pixel gets, 0..256, from the distance between the
 * source colour and its nearest palette colour: none within snap levels,
 * full from 2x snap, ramping in between so gradients have no hard edge.
 * Without this, error diffusion turns a large near-black area into black
 * sprinkled with bright dots (the leftover error, saved up and spent all
 * at once). *nearest_out gets the nearest colour. */
static int dither_amount(const EgtPalette *pal, const uint8_t *s, int snap, int *nearest_out)
{
    int k = nearest(pal, s[0], s[1], s[2]);
    int dr, dg, db;
    double d;
    *nearest_out = k;
    if (snap <= 0)
        return 256;
    dr = s[0] - pal->colors[k].r;
    dg = s[1] - pal->colors[k].g;
    db = s[2] - pal->colors[k].b;
    d = sqrt((double)(dr * dr + dg * dg + db * db));
    if (d <= snap)
        return 0;
    if (d >= 2.0 * snap)
        return 256;
    return (int)((d - snap) * 256 / snap);
}

/* Ordered: a 4x4 Bayer threshold, scaled to the spacing of the palette's
 * colours, is added to each pixel before the nearest colour is picked.
 * Near a palette colour (Snap) the threshold fades out. */
static void convert_ordered(const EgtImage *src, EgtImage *dst, const EgtPalette *pal, int snap)
{
    static const int BAYER4[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5},
    };
    /* Roughly the distance between neighbouring palette colours. */
    int spread = (int)(255.0 / cbrt((double)pal->count));
    int x, y, k;
    for (y = 0; y < src->height; y++) {
        for (x = 0; x < src->width; x++) {
            size_t o = ((size_t)y * src->width + x) * 4;
            const uint8_t *s = src->pixels + o;
            int amount = dither_amount(pal, s, snap, &k);
            int t = (BAYER4[y & 3][x & 3] * 2 - 15) * spread / 32 * amount / 256;
            if (amount > 0)
                k = nearest(pal, clamp8(s[0] + t), clamp8(s[1] + t), clamp8(s[2] + t));
            put(dst->pixels + o, pal->colors[k]);
        }
    }
}

/* Error diffusion with a rolling buffer of 3 error rows, padded by 2 pixels
 * on each side so taps never need bounds checks. */
static bool convert_diffuse(const EgtImage *src, EgtImage *dst, const EgtPalette *pal,
                            const Kernel *k, int snap)
{
    int w = src->width, h = src->height;
    size_t stride = (size_t)(w + 4) * 3;
    int *err = calloc(stride * 3, sizeof(int));
    int x, y, t;
    if (!err)
        return false;

    for (y = 0; y < h; y++) {
        int *row = err + (size_t)(y % 3) * stride;
        for (x = 0; x < w; x++) {
            size_t o = ((size_t)y * w + x) * 4;
            const uint8_t *s = src->pixels + o;
            int *e = row + (size_t)(x + 2) * 3;
            int ci, amount = dither_amount(pal, s, snap, &ci);
            int r, g, b, er, eg, eb;
            EgtColor c;
            if (amount == 0) {
                /* Close enough: solid colour; error arriving here is dropped. */
                put(dst->pixels + o, pal->colors[ci]);
                continue;
            }
            r = clamp8(s[0] + e[0] / k->divisor * amount / 256);
            g = clamp8(s[1] + e[1] / k->divisor * amount / 256);
            b = clamp8(s[2] + e[2] / k->divisor * amount / 256);
            c = pal->colors[nearest(pal, r, g, b)];
            er = (r - c.r) * amount / 256;
            eg = (g - c.g) * amount / 256;
            eb = (b - c.b) * amount / 256;
            put(dst->pixels + o, c);
            for (t = 0; t < k->ntaps; t++) {
                const KernelTap *tap = &k->taps[t];
                int *te = err + (size_t)((y + tap->dy) % 3) * stride
                              + (size_t)(x + 2 + tap->dx) * 3;
                te[0] += er * tap->w;
                te[1] += eg * tap->w;
                te[2] += eb * tap->w;
            }
        }
        /* This row is done; it becomes the row two below the next one. */
        memset(row, 0, stride * sizeof(int));
    }
    free(err);
    return true;
}

/* Harsh mix calming weight: percent of a pair's "absence" (how far its two
 * colours are from anything around the pixel in the source) added to its
 * match distance. */
#define HARSH_MIX_WEIGHT_PCT 10

/* harsh_mix and the surface tolerance are defined at this resolution and
 * scaled for others (tuned on 640x480 output). */
#define HARSH_MIX_REF_PIXELS (640.0 * 480.0)

int egt_harsh_mix_area(int harsh_mix, int width, int height)
{
    long area;
    if (harsh_mix <= 0)
        return 0;
    if (harsh_mix >= 100)
        return -1;
    /* At 640x480: 1 -> 4 px, 10 -> ~126, 30 -> ~657, 50 -> ~1414 */
    area = lround(4.0 * pow(harsh_mix, 1.5) * ((double)width * height / HARSH_MIX_REF_PIXELS));
    return area < 1 ? 1 : (int)area;
}

/* 50/50 mix of two channel values averaged with the given gamma. */
static int gamma_mix(int a, int b, double g)
{
    double m = (pow(a / 255.0, g) + pow(b / 255.0, g)) / 2;
    return clamp8((int)lround(255.0 * pow(m, 1.0 / g)));
}

typedef struct {
    int r, g, b;  /* the 50/50 mix */
    uint8_t a, b2;
} CheckerPair;

/* Pairs whose score is within this (squared distance) of the best are
 * treated as equally good, and the one with the most lightness contrast in
 * the source's own hue is used (see tie_preference): the EGA look favours
 * contrasty checkerboards, and exact ties (mid grey is black + white, dark
 * + light grey, blue + yellow...) resolve consistently instead of by
 * search order. */
#define PAIR_TIE_TOLERANCE 20

/* For each palette colour, its squared distance to the nearest colour
 * that occurs within output pixel (x, y)'s footprint in detail (any size
 * covering the same picture; up to 2x the output gives sub-pixel detail).
 * Only the pixel's own footprint: a wider window lets the black of an
 * adjacent outline excuse black speckle on a line next to it. */
static void colour_absence(const EgtImage *detail, int out_w, int out_h, int x, int y,
                           const EgtPalette *pal, int *absent)
{
    int x0 = (int)(((int64_t)x * detail->width) / out_w);
    int x1 = (int)(((int64_t)(x + 1) * detail->width) / out_w);
    int y0 = (int)(((int64_t)y * detail->height) / out_h);
    int y1 = (int)(((int64_t)(y + 1) * detail->height) / out_h);
    int k, sx, sy;
    if (x1 <= x0)
        x1 = x0 + 1;
    if (y1 <= y0)
        y1 = y0 + 1;
    for (k = 0; k < pal->count; k++)
        absent[k] = 0x7fffffff;
    for (sy = y0; sy < y1; sy++) {
        const uint8_t *p = detail->pixels + ((size_t)sy * detail->width + x0) * 4;
        for (sx = x0; sx < x1; sx++, p += 4) {
            for (k = 0; k < pal->count; k++) {
                int dr = p[0] - pal->colors[k].r;
                int dg = p[1] - pal->colors[k].g;
                int db = p[2] - pal->colors[k].b;
                int d = dr * dr + dg * dg + db * db;
                if (d < absent[k])
                    absent[k] = d;
            }
        }
    }
}

/* How much a pair is "contrast in the source's own colour": the spread
 * between its two colours along the line from black through the source
 * colour (lightness in that hue), minus the spread across it (hue
 * change). Black + white for grey scores high; blue + yellow, which also
 * averages to grey, scores low. */
static float tie_preference(const uint8_t *s, const EgtPalette *pal, const CheckerPair *pair)
{
    const EgtColor *a = &pal->colors[pair->a], *b = &pal->colors[pair->b2];
    /* Direction of the source colour, nudged towards grey so very dark
     * sources still have a sensible direction. */
    float dir[3] = { s[0] + 20.0f, s[1] + 20.0f, s[2] + 20.0f };
    float diff[3] = { (float)a->r - b->r, (float)a->g - b->g, (float)a->b - b->b };
    float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    float along = fabsf(diff[0] * dir[0] + diff[1] * dir[1] + diff[2] * dir[2]) / len;
    float total2 = diff[0] * diff[0] + diff[1] * diff[1] + diff[2] * diff[2];
    float across = sqrtf(fmaxf(0.0f, total2 - along * along));
    return along - across;
}

bool egt__keep_solid(float solid_dist, float mix_dist, int prefer_solid)
{
    return solid_dist <= mix_dist + PREFER_SOLID_STEP * prefer_solid;
}

/* Label for one pixel: solid colour index, or n + pair index. With absent
 * (from colour_absence), a pair's distance also includes how far its two
 * colours are from anything nearby in the source: a checkerboard of
 * colours that don't occur there (black + white in a grey crease, red +
 * green for an olive blend of yellow and blue) looks like noise even when
 * its average matches, while a pair of colours that do occur (red + white
 * on a flag, yellow + black at a thin rim) reproduces real detail. */
static uint16_t checker_label(const uint8_t *s, const EgtPalette *pal, const CheckerPair *pairs, int npairs, int prefer_solid,
                              const int *absent)
{
    int n = pal->count, p;
    int solid = nearest(pal, s[0], s[1], s[2]);
    int best = 0, best_d = 0x7fffffff;
    int score[EGT_CHECKER_MAX_COLORS * (EGT_CHECKER_MAX_COLORS + 1) / 2];
    for (p = 0; p < npairs; p++) {
        int pr = s[0] - pairs[p].r, pg = s[1] - pairs[p].g, pb = s[2] - pairs[p].b;
        int d = pr * pr + pg * pg + pb * pb;
        if (absent)
            d += (int)(((int64_t)absent[pairs[p].a] + absent[pairs[p].b2])
                       * HARSH_MIX_WEIGHT_PCT / 200);
        score[p] = d;
        if (d < best_d) {
            best_d = d;
            best = p;
        }
    }
    /* Among near-equal pairs, take the most contrasty in the source's hue. */
    {
        int pick = best;
        float pick_pref = tie_preference(s, pal, &pairs[best]);
        for (p = 0; p < npairs; p++) {
            if (p != best && score[p] <= best_d + PAIR_TIE_TOLERANCE) {
                float pref = tie_preference(s, pal, &pairs[p]);
                if (pref > pick_pref) {
                    pick_pref = pref;
                    pick = p;
                }
            }
        }
        best = pick;
    }
    /* Solid or checkerboard (a harsh-mix penalty counts against the pair). */
    {
        int dr = s[0] - pal->colors[solid].r;
        int dg = s[1] - pal->colors[solid].g;
        int db = s[2] - pal->colors[solid].b;
        float solid_dist = sqrtf((float)(dr * dr + dg * dg + db * db));
        return (uint16_t)(egt__keep_solid(solid_dist, sqrtf((float)score[best]), prefer_solid)
                          ? solid : n + best);
    }
}

/* Source areas for harsh_mix: 8-connected pixels whose neighbours differ
 * by at most this much in every channel (at the reference resolution; a
 * gradient changes more per pixel at lower resolutions, so the tolerance
 * grows with the linear scale), so a gently shaded surface is one area
 * while lines, edges and small details are separate small ones. */
#define SURFACE_TOLERANCE 16

/* For each pixel, the size of its source surface (see SURFACE_TOLERANCE),
 * capped at cap + 1 since only "small or not" matters. */
static int32_t *surface_sizes(const EgtImage *src, int cap)
{
    int tol = (int)lround(SURFACE_TOLERANCE *
                          sqrt(HARSH_MIX_REF_PIXELS / ((double)src->width * src->height)));
    int w = src->width, h = src->height, dx, dy, c;
    size_t n = (size_t)w * h, p, head, tail, k;
    int32_t *size = malloc(n * sizeof(int32_t));
    int32_t *queue = malloc(n * sizeof(int32_t));
    uint8_t *seen = calloc(n, 1);
    if (!size || !queue || !seen) {
        free(size);
        free(queue);
        free(seen);
        return NULL;
    }
    for (p = 0; p < n; p++) {
        if (seen[p])
            continue;
        head = tail = 0;
        queue[tail++] = (int32_t)p;
        seen[p] = 1;
        while (head < tail) {
            int qx = queue[head] % w, qy = queue[head] / w;
            const uint8_t *q = src->pixels + (size_t)queue[head] * 4;
            head++;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int nx = qx + dx, ny = qy + dy;
                    size_t nb = (size_t)ny * w + nx;
                    const uint8_t *o;
                    bool near = true;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || seen[nb])
                        continue;
                    o = src->pixels + nb * 4;
                    for (c = 0; c < 3; c++)
                        if (abs(o[c] - q[c]) > tol)
                            near = false;
                    if (near) {
                        seen[nb] = 1;
                        queue[tail++] = (int32_t)nb;
                    }
                }
            }
        }
        for (k = 0; k < tail; k++)
            size[queue[k]] = (int32_t)(tail > (size_t)cap ? (size_t)cap + 1 : tail);
    }
    free(queue);
    free(seen);
    return size;
}

/* Each pixel is either the nearest solid colour or, if a 50/50 mix of two
 * colours looks closer by more than prefer_solid allows (see
 * egt__keep_solid), one half of a checkerboard.
 * With harsh_mix, pixels in small source areas (details, lines, edges) are
 * labelled with the harsh pair penalty, so colour noise there calms down
 * while large surfaces keep their original pairs.
 * Labels 0..n-1 are solid colours, n + i is colour pair i. */
static bool convert_checker(const EgtImage *src, EgtImage *dst, const EgtPalette *pal,
                            const EgtConvertOptions *opt)
{
    int prefer_solid = opt->prefer_solid, harsh_mix = opt->harsh_mix, min_region = opt->min_region;
    const EgtImage *detail = opt->detail;
    int n = pal->count, npairs = n * (n + 1) / 2;
    int w = src->width, h = src->height;
    size_t npix = (size_t)w * h, i;
    int a, b, x, y, p = 0;
    int area = egt_harsh_mix_area(harsh_mix, w, h);
    CheckerPair *pairs;
    uint16_t *lab;

    if (n > EGT_CHECKER_MAX_COLORS)
        return false;
    pairs = malloc((size_t)npairs * sizeof *pairs);
    lab = malloc(npix * sizeof(uint16_t));
    if (!pairs || !lab) {
        free(pairs);
        free(lab);
        return false;
    }
    for (a = 0; a < n; a++) {
        for (b = a; b < n; b++, p++) {
            if (opt->light_mix > 0) {
                const EgtColor *ca = &pal->colors[a], *cb = &pal->colors[b];
                /* Quadratic: fine steps at the low end, where a small change
                 * already decides between near-equal pairs. */
                double t = (opt->light_mix > 100 ? 100 : opt->light_mix) / 100.0;
                double g = 1.0 + 1.2 * t * t;
                pairs[p].r = gamma_mix(ca->r, cb->r, g);
                pairs[p].g = gamma_mix(ca->g, cb->g, g);
                pairs[p].b = gamma_mix(ca->b, cb->b, g);
            } else {
                pairs[p].r = (pal->colors[a].r + pal->colors[b].r) / 2;
                pairs[p].g = (pal->colors[a].g + pal->colors[b].g) / 2;
                pairs[p].b = (pal->colors[a].b + pal->colors[b].b) / 2;
            }
            pairs[p].a = (uint8_t)a;
            pairs[p].b2 = (uint8_t)b;
        }
    }

    {
        int32_t *size = area > 0 ? surface_sizes(src, area) : NULL;
        if (area > 0 && !size) {
            free(pairs);
            free(lab);
            return false;
        }
        int absent[EGT_CHECKER_MAX_COLORS];
        /* Labels of plain (not calmed) pixels depend only on their colour,
         * and images repeat colours a lot: cache them by colour in an
         * open-addressing table (key 0 = empty, so keys are rgb + 1). */
        size_t cap = 1024, mask;
        uint32_t *keys;
        uint16_t *vals;
        while (cap < npix * 2)
            cap *= 2;
        mask = cap - 1;
        keys = calloc(cap, sizeof *keys);
        vals = malloc(cap * sizeof *vals);
        if (!keys || !vals) {
            free(keys);
            free(vals);
            free(size);
            free(pairs);
            free(lab);
            return false;
        }
        if (!detail)
            detail = src;
        for (i = 0; i < npix; i++) {
            const uint8_t *s = src->pixels + i * 4;
            bool calm = area < 0 || (size && size[i] <= area);
            if (calm) {
                colour_absence(detail, w, h, (int)(i % w), (int)(i / w), pal, absent);
                lab[i] = checker_label(s, pal, pairs, npairs, prefer_solid, absent);
            } else {
                uint32_t key = ((uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2]) + 1;
                size_t slot = (key * 2654435761u) & mask;
                while (keys[slot] && keys[slot] != key)
                    slot = (slot + 1) & mask;
                if (!keys[slot]) {
                    keys[slot] = key;
                    vals[slot] = checker_label(s, pal, pairs, npairs, prefer_solid, NULL);
                }
                lab[i] = vals[slot];
            }
        }
        free(keys);
        free(vals);
        free(size);
    }

    if (min_region > 1 && !cleanup_regions(lab, w, h, n + npairs, min_region)) {
        free(pairs);
        free(lab);
        return false;
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int l = lab[(size_t)y * w + x];
            if (l < n) {
                put(dst->pixels + ((size_t)y * w + x) * 4, pal->colors[l]);
            } else {
                const CheckerPair *pr = &pairs[l - n];
                put(dst->pixels + ((size_t)y * w + x) * 4,
                    pal->colors[((x ^ y) & 1) ? pr->b2 : pr->a]);
            }
        }
    }
    free(pairs);
    free(lab);
    return true;
}

bool egt_convert(const EgtImage *src, EgtImage *dst, const EgtPalette *pal,
                 const EgtConvertOptions *opt)
{
    if (pal->count < 1 || src->width != dst->width || src->height != dst->height)
        return false;
    if (opt->limit > EGT_LIMIT_NONE && opt->limit < EGT_LIMIT_COUNT) {
        const Kernel *k = opt->dither == EGT_DITHER_BURKES ? &KERNEL_BURKES
                        : opt->dither == EGT_DITHER_FLOYD_STEINBERG ? &KERNEL_FLOYD
                        : opt->dither == EGT_DITHER_SIERRA ? &KERNEL_SIERRA
                        : opt->dither == EGT_DITHER_STUCKI ? &KERNEL_STUCKI : NULL;
        return egt__convert_limited(src, dst, pal, opt, k);
    }
    switch (opt->dither) {
    case EGT_DITHER_NONE:            return convert_none(src, dst, pal, opt->min_region);
    case EGT_DITHER_ORDERED:         convert_ordered(src, dst, pal, opt->snap); return true;
    case EGT_DITHER_BURKES:          return convert_diffuse(src, dst, pal, &KERNEL_BURKES, opt->snap);
    case EGT_DITHER_FLOYD_STEINBERG: return convert_diffuse(src, dst, pal, &KERNEL_FLOYD, opt->snap);
    case EGT_DITHER_SIERRA:          return convert_diffuse(src, dst, pal, &KERNEL_SIERRA, opt->snap);
    case EGT_DITHER_STUCKI:          return convert_diffuse(src, dst, pal, &KERNEL_STUCKI, opt->snap);
    case EGT_DITHER_EGA_CHECKER:
        return convert_checker(src, dst, pal, opt);
    default:                         return false;
    }
}
