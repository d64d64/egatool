/*
 * EGAtool pre-processing: area resize, smoothing and outline inking.
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#include "engine.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char *SMOOTH_NAMES[EGT_SMOOTH_COUNT] = { "None", "Median", "Kuwahara", "Surface" };

static const char *OUTLINE_NAMES[EGT_OUTLINE_COUNT] = { "Basic", "Hi-res" };

const char *egt_smooth_name(EgtSmooth s)
{
    return (s >= 0 && s < EGT_SMOOTH_COUNT) ? SMOOTH_NAMES[s] : "?";
}

const char *egt_outline_mode_name(EgtOutlineMode m)
{
    return (m >= 0 && m < EGT_OUTLINE_COUNT) ? OUTLINE_NAMES[m] : "?";
}

/* Taps of a 1-D box filter from src_n to dst_n samples: output i averages
 * the source interval [i, i + 1) * src_n / dst_n, each source sample
 * weighted by how much of it lies inside (partly covered samples at the
 * ends count partly). An enlarged axis takes the nearest sample instead.
 * Returns false on allocation failure. */
typedef struct {
    int *first, *count; /* per output: first source index, number of taps */
    float *weight;      /* per output: max_taps weights, summing to 1 */
    int max_taps;
} BoxTaps;

static void box_taps_free(BoxTaps *t)
{
    free(t->first);
    free(t->count);
    free(t->weight);
    t->first = t->count = NULL;
    t->weight = NULL;
}

/* For each output position along one axis: the source pixels it covers
 * and their weights, for area-average resizing. */
static bool box_taps(BoxTaps *t, int src_n, int dst_n)
{
    double scale = (double)src_n / dst_n;
    int i, j;
    t->max_taps = scale <= 1.0 ? 1 : (int)ceil(scale) + 1;
    t->first = malloc((size_t)dst_n * sizeof *t->first);
    t->count = malloc((size_t)dst_n * sizeof *t->count);
    t->weight = malloc((size_t)dst_n * t->max_taps * sizeof *t->weight);
    if (!t->first || !t->count || !t->weight) {
        box_taps_free(t);
        return false;
    }
    for (i = 0; i < dst_n; i++) {
        float *w = t->weight + (size_t)i * t->max_taps;
        if (scale <= 1.0) {
            int n = (int)((i + 0.5) * scale);
            t->first[i] = n < src_n ? n : src_n - 1;
            t->count[i] = 1;
            w[0] = 1.0f;
            continue;
        }
        {
            double a = i * scale, b = (i + 1) * scale;
            int j0 = (int)floor(a), j1 = (int)ceil(b);
            if (j1 > src_n)
                j1 = src_n;
            if (j1 - j0 > t->max_taps) /* rounding safety */
                j1 = j0 + t->max_taps;
            t->first[i] = j0;
            t->count[i] = j1 - j0;
            for (j = j0; j < j1; j++)
                w[j - j0] = (float)((fmin(b, j + 1) - fmax(a, j)) / scale);
        }
    }
    return true;
}

EgtImage egt_resize_area(const EgtImage *src, int width, int height)
{
    EgtImage dst = egt_image_alloc(width, height);
    BoxTaps tx = {0}, ty = {0};
    float *tmp = NULL; /* horizontally filtered: width x src->height, RGB */
    int x, y, k;
    if (!dst.pixels)
        return dst;
    tmp = malloc((size_t)width * src->height * 3 * sizeof *tmp);
    if (!tmp || !box_taps(&tx, src->width, width) || !box_taps(&ty, src->height, height)) {
        free(tmp);
        box_taps_free(&tx);
        box_taps_free(&ty);
        egt_image_free(&dst);
        return dst;
    }
    for (y = 0; y < src->height; y++) {
        const uint8_t *row = src->pixels + (size_t)y * src->width * 4;
        float *t = tmp + (size_t)y * width * 3;
        for (x = 0; x < width; x++, t += 3) {
            const uint8_t *s = row + (size_t)tx.first[x] * 4;
            const float *w = tx.weight + (size_t)x * tx.max_taps;
            float r = 0, g = 0, b = 0;
            for (k = 0; k < tx.count[x]; k++, s += 4) {
                r += w[k] * s[0];
                g += w[k] * s[1];
                b += w[k] * s[2];
            }
            t[0] = r;
            t[1] = g;
            t[2] = b;
        }
    }
    for (y = 0; y < height; y++) {
        const float *w = ty.weight + (size_t)y * ty.max_taps;
        uint8_t *d = dst.pixels + (size_t)y * width * 4;
        for (x = 0; x < width; x++, d += 4) {
            const float *t = tmp + ((size_t)ty.first[y] * width + x) * 3;
            float r = 0, g = 0, b = 0;
            for (k = 0; k < ty.count[y]; k++, t += (size_t)width * 3) {
                r += w[k] * t[0];
                g += w[k] * t[1];
                b += w[k] * t[2];
            }
            d[0] = (uint8_t)fminf(r + 0.5f, 255.0f);
            d[1] = (uint8_t)fminf(g + 0.5f, 255.0f);
            d[2] = (uint8_t)fminf(b + 0.5f, 255.0f);
            d[3] = 255;
        }
    }
    free(tmp);
    box_taps_free(&tx);
    box_taps_free(&ty);
    return dst;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Per-channel median over a (2r+1)^2 window (clipped at the borders), with
 * a sliding histogram per row (Huang): O(r) per pixel instead of O(r^2). */
static void smooth_median(const EgtImage *src, EgtImage *dst, int r)
{
    int w = src->width, h = src->height, x, y, yy, c;
    for (y = 0; y < h; y++) {
        int hist[3][256];
        int med[3] = {0, 0, 0}, below[3] = {0, 0, 0};
        int y0 = y - r < 0 ? 0 : y - r, y1 = y + r >= h ? h - 1 : y + r;
        int n = 0;
        memset(hist, 0, sizeof hist);
        for (x = 0; x < w; x++) {
            /* Slide: add the column entering on the right, drop the one
             * leaving on the left (the first pixel adds columns 0..r). */
            int add0 = x == 0 ? 0 : x + r, add1 = x + r >= w ? w - 1 : x + r;
            int drop = x - r - 1, xa, k;
            uint8_t *d = dst->pixels + ((size_t)y * w + x) * 4;
            for (xa = add0; xa <= add1; xa++) {
                for (yy = y0; yy <= y1; yy++) {
                    const uint8_t *p = src->pixels + ((size_t)yy * w + xa) * 4;
                    for (c = 0; c < 3; c++) {
                        hist[c][p[c]]++;
                        below[c] += p[c] < med[c];
                    }
                    n++;
                }
            }
            if (drop >= 0) {
                for (yy = y0; yy <= y1; yy++) {
                    const uint8_t *p = src->pixels + ((size_t)yy * w + drop) * 4;
                    for (c = 0; c < 3; c++) {
                        hist[c][p[c]]--;
                        below[c] -= p[c] < med[c];
                    }
                    n--;
                }
            }
            /* Move each median to the k-th smallest value: below counts
             * values < med, and med holds indices below..below+hist-1. */
            k = n / 2;
            for (c = 0; c < 3; c++) {
                while (below[c] > k)
                    below[c] -= hist[c][--med[c]];
                while (below[c] + hist[c][med[c]] <= k)
                    below[c] += hist[c][med[c]++];
                d[c] = (uint8_t)med[c];
            }
            d[3] = 255;
        }
    }
}

/* Kuwahara: of the four (r+1)x(r+1) squares that have the pixel as a
 * corner, output the mean of the one with the lowest colour variance.
 * Flattens areas while keeping edges sharp. Uses summed-area tables. */
static bool smooth_kuwahara(const EgtImage *src, EgtImage *dst, int r)
{
    int w = src->width, h = src->height, x, y, q, c;
    size_t stride = (size_t)w + 1;
    int64_t *sum = calloc(stride * (h + 1) * 4, sizeof(int64_t));
    if (!sum)
        return false;

    /* sum[(y*stride+x)*4 + c]: c = 0..2 channel sums, 3 = sum of squares */
    for (y = 0; y < h; y++) {
        int64_t row[4] = {0, 0, 0, 0};
        for (x = 0; x < w; x++) {
            const uint8_t *s = src->pixels + ((size_t)y * w + x) * 4;
            int64_t *above = sum + ((size_t)y * stride + x + 1) * 4;
            int64_t *cur = sum + ((size_t)(y + 1) * stride + x + 1) * 4;
            row[0] += s[0];
            row[1] += s[1];
            row[2] += s[2];
            row[3] += s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
            for (c = 0; c < 4; c++)
                cur[c] = above[c] + row[c];
        }
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            static const int QX[4] = {-1, 0, -1, 0}, QY[4] = {-1, -1, 0, 0};
            double best_var = 1e30;
            int64_t best[3] = {0, 0, 0};
            int64_t best_n = 1;
            uint8_t *d = dst->pixels + ((size_t)y * w + x) * 4;
            for (q = 0; q < 4; q++) {
                /* Quadrant spans r pixels towards QX/QY, clamped. */
                int x0 = clampi(QX[q] ? x - r : x, 0, w - 1);
                int x1 = clampi(QX[q] ? x : x + r, 0, w - 1) + 1;
                int y0 = clampi(QY[q] ? y - r : y, 0, h - 1);
                int y1 = clampi(QY[q] ? y : y + r, 0, h - 1) + 1;
                int64_t n = (int64_t)(x1 - x0) * (y1 - y0), s[4];
                double mean_sq = 0, var;
                for (c = 0; c < 4; c++) {
                    s[c] = sum[((size_t)y1 * stride + x1) * 4 + c]
                         - sum[((size_t)y0 * stride + x1) * 4 + c]
                         - sum[((size_t)y1 * stride + x0) * 4 + c]
                         + sum[((size_t)y0 * stride + x0) * 4 + c];
                }
                for (c = 0; c < 3; c++)
                    mean_sq += ((double)s[c] / n) * ((double)s[c] / n);
                var = (double)s[3] / n - mean_sq;
                if (var < best_var) {
                    best_var = var;
                    best[0] = s[0];
                    best[1] = s[1];
                    best[2] = s[2];
                    best_n = n;
                }
            }
            for (c = 0; c < 3; c++)
                d[c] = (uint8_t)((best[c] + best_n / 2) / best_n);
            d[3] = 255;
        }
    }
    free(sum);
    return true;
}

/* Surface blur (like Photoshop's): each pixel becomes the weighted mean
 * of the pixels within radius r whose colour differs from it by less than
 * 2.5 * threshold (Euclidean RGB), weighted by 1 - (difference / (2.5 *
 * threshold))^2. A thin line of a different colour only averages with
 * itself, so it keeps its colour and width, while gentle shading and noise
 * within a surface is smoothed away. */
static void smooth_surface(const EgtImage *src, EgtImage *dst, int r, int threshold)
{
    int w = src->width, h = src->height, x, y, dy;
    int limit = (int)(2.5 * clampi(threshold, 1, 100) + 0.5);
    int32_t limit2 = limit * limit;
    /* weight = (limit2 - e2) >> shift, at most 1024 (sums fit in 32 bits) */
    int shift = 0;
    int *span = malloc((size_t)(2 * r + 1) * sizeof *span); /* round window half-widths */
    if (!span) {
        memcpy(dst->pixels, src->pixels, (size_t)w * h * 4);
        return;
    }
    while ((limit2 >> shift) > 1024)
        shift++;
    for (dy = -r; dy <= r; dy++)
        span[dy + r] = (int)floor(sqrt((double)(r * r + r - dy * dy)));
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *c = src->pixels + ((size_t)y * w + x) * 4;
            uint8_t *d = dst->pixels + ((size_t)y * w + x) * 4;
            int32_t cr = c[0], cg = c[1], cb = c[2];
            uint32_t sr = 0, sg = 0, sb = 0, sw = 0;
            for (dy = -r; dy <= r; dy++) {
                int py = y + dy, x0, x1, i, n, step;
                const uint8_t *p;
                if (py < 0 || py >= h)
                    continue;
                x0 = x - span[dy + r] < 0 ? 0 : x - span[dy + r];
                x1 = x + span[dy + r] >= w ? w - 1 : x + span[dy + r];
                /* Large windows: full rows near the centre, beyond that a
                 * checkerboard of pixels (half the cost; the mean of a
                 * surface barely changes, and each radius step still adds
                 * pixels). */
                step = r > 4 && (dy > 2 || dy < -2) ? 2 : 1;
                if (step == 2 && ((x0 - x + dy) & 1))
                    x0++;
                p = src->pixels + ((size_t)py * w + x0) * 4;
                n = x1 < x0 ? 0 : (x1 - x0) / step + 1;
                for (i = 0; i < n; i++, p += step * 4) {
                    int32_t er = p[0] - cr, eg = p[1] - cg, eb = p[2] - cb;
                    int32_t e = limit2 - (er * er + eg * eg + eb * eb);
                    uint32_t wt = (uint32_t)((e > 0 ? e : 0) >> shift);
                    sr += wt * p[0];
                    sg += wt * p[1];
                    sb += wt * p[2];
                    sw += wt;
                }
            }
            /* the centre pixel always has weight limit2 >> shift > 0 */
            d[0] = (uint8_t)((sr + sw / 2) / sw);
            d[1] = (uint8_t)((sg + sw / 2) / sw);
            d[2] = (uint8_t)((sb + sw / 2) / sw);
            d[3] = 255;
        }
    }
    free(span);
}

/* Outline sensitivity 1..100 as a colour step height (0..255) an edge must
 * exceed: 1 -> 250 (only near black/white contrasts), 50 -> ~67, 100 -> 4.
 * Quadratic, for finer control at the high-sensitivity end. Both outline
 * modes normalise their gradients to step height, so they share the scale. */
static double step_threshold(int sensitivity)
{
    double inv = (100 - clampi(sensitivity, 1, 100)) / 100.0;
    return 4 + 251 * inv * inv;
}

/* Removes 8-connected outline fragments shorter than min_len pixels. */
static bool prune_mask(uint8_t *mask, int w, int h, int min_len)
{
    size_t n = (size_t)w * h, p, head, tail, i;
    int32_t *queue;
    uint8_t *seen;
    if (min_len <= 1)
        return true;
    queue = malloc(n * sizeof(int32_t));
    seen = calloc(n, 1);
    if (!queue || !seen) {
        free(queue);
        free(seen);
        return false;
    }
    for (p = 0; p < n; p++) {
        if (!mask[p] || seen[p])
            continue;
        head = tail = 0;
        queue[tail++] = (int32_t)p;
        seen[p] = 1;
        while (head < tail) {
            int qx = queue[head] % w, qy = queue[head] / w, dx, dy;
            head++;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int nx = qx + dx, ny = qy + dy;
                    size_t nb = (size_t)ny * w + nx;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || !mask[nb] || seen[nb])
                        continue;
                    seen[nb] = 1;
                    queue[tail++] = (int32_t)nb;
                }
            }
        }
        if (tail < (size_t)min_len)
            for (i = 0; i < tail; i++)
                mask[queue[i]] = 0;
    }
    free(queue);
    free(seen);
    return true;
}

EgtImage egt_detail_image(const EgtImage *source, int width, int height)
{
    if (source->width > width * 2 || source->height > height * 2)
        return egt_resize_area(source, width * 2, height * 2);
    return egt_image_copy(source);
}

/* Darkens the masked pixels; darkness 1 makes them black. */
static void apply_mask(EgtImage *img, const uint8_t *mask, float darkness)
{
    size_t i, n = (size_t)img->width * img->height;
    int c, keep = (int)((1.0f - (darkness < 0 ? 0 : darkness > 1 ? 1 : darkness)) * 256);
    for (i = 0; i < n; i++) {
        if (mask[i]) {
            uint8_t *p = img->pixels + i * 4;
            for (c = 0; c < 3; c++)
                p[c] = (uint8_t)((p[c] * keep) >> 8);
        }
    }
}

/* Darkens pixels on the dark side of strong edges, giving 1 pixel wide
 * ink-style outlines that the palette mapping then turns into dark colours. */
static bool ink_outlines(EgtImage *img, int sensitivity, float darkness, int min_len)
{
    int w = img->width, h = img->height, x, y, dx, dy, c;
    /* A hard step of height D gives a Sobel magnitude of 4D. */
    double t = 4 * step_threshold(sensitivity);
    int threshold_sq = (int)(t * t);
    int *luma = malloc((size_t)w * h * sizeof(int));
    uint8_t *mask = calloc((size_t)w * h, 1);

    if (!luma || !mask) {
        free(luma);
        free(mask);
        return false;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *p = img->pixels + ((size_t)y * w + x) * 4;
            luma[(size_t)y * w + x] = p[0] * 77 + p[1] * 150 + p[2] * 29;
        }
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int mag = 0, lsum = 0;
            const uint8_t *n[3][3];
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int xx = clampi(x + dx, 0, w - 1), yy = clampi(y + dy, 0, h - 1);
                    n[dy + 1][dx + 1] = img->pixels + ((size_t)yy * w + xx) * 4;
                    lsum += luma[(size_t)yy * w + xx];
                }
            }
            for (c = 0; c < 3; c++) {
                int gx = (n[0][2][c] + 2 * n[1][2][c] + n[2][2][c])
                       - (n[0][0][c] + 2 * n[1][0][c] + n[2][0][c]);
                int gy = (n[2][0][c] + 2 * n[2][1][c] + n[2][2][c])
                       - (n[0][0][c] + 2 * n[0][1][c] + n[0][2][c]);
                int m = gx * gx + gy * gy;
                if (m > mag)
                    mag = m;
            }
            /* Only the darker side of the edge, so lines stay 1 pixel wide. */
            if (mag > threshold_sq && luma[(size_t)y * w + x] * 9 < lsum)
                mask[(size_t)y * w + x] = 1;
        }
    }

    if (!prune_mask(mask, w, h, min_len)) {
        free(luma);
        free(mask);
        return false;
    }
    apply_mask(img, mask, darkness);
    free(luma);
    free(mask);
    return true;
}

/* Separable Gaussian blur of the RGB channels into dst (RGB, 3 bytes per
 * pixel). Integer weights summing to 1 << 12. Returns the kernel's response
 * to a unit step as seen by a Sobel operator, for threshold normalisation. */
static bool gaussian_rgb(const EgtImage *src, uint8_t *dst, double sigma, double *step_gain)
{
    int w = src->width, h = src->height, x, y, k, c;
    int radius = (int)ceil(sigma * 3.0);
    int *kern = malloc((size_t)(2 * radius + 1) * sizeof(int));
    uint16_t *tmp = malloc((size_t)w * h * 3 * sizeof(uint16_t));
    double total = 0, best = 0, cum = 0;
    int isum = 0;

    if (!kern || !tmp) {
        free(kern);
        free(tmp);
        return false;
    }
    for (k = -radius; k <= radius; k++)
        total += exp(-(double)(k * k) / (2 * sigma * sigma));
    for (k = -radius; k <= radius; k++) {
        kern[k + radius] = (int)lround(exp(-(double)(k * k) / (2 * sigma * sigma)) / total * 4096);
        isum += kern[k + radius];
    }
    kern[radius] += 4096 - isum; /* exact normalisation */

    /* Sobel (1,2,1 smoothing, central difference) of a blurred unit step
     * peaks at 4 * (cdf(k+1) - cdf(k-1)); take the maximum over k. */
    {
        double *cdf = malloc((size_t)(2 * radius + 3) * sizeof(double));
        if (!cdf) {
            free(kern);
            free(tmp);
            return false;
        }
        cdf[0] = 0;
        for (k = 0; k < 2 * radius + 1; k++) {
            cum += kern[k] / 4096.0;
            cdf[k + 1] = cum;
        }
        cdf[2 * radius + 2] = 1;
        for (k = 1; k < 2 * radius + 2; k++) {
            double d = 4 * (cdf[k + 1] - cdf[k - 1]);
            if (d > best)
                best = d;
        }
        free(cdf);
        *step_gain = best;
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int acc[3] = {0, 0, 0};
            for (k = -radius; k <= radius; k++) {
                const uint8_t *p = src->pixels + ((size_t)y * w + clampi(x + k, 0, w - 1)) * 4;
                for (c = 0; c < 3; c++)
                    acc[c] += p[c] * kern[k + radius];
            }
            for (c = 0; c < 3; c++)
                tmp[((size_t)y * w + x) * 3 + c] = (uint16_t)((acc[c] + 8) >> 4); /* 8.8 fixed */
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int acc[3] = {0, 0, 0};
            for (k = -radius; k <= radius; k++) {
                const uint16_t *p = tmp + ((size_t)clampi(y + k, 0, h - 1) * w + x) * 3;
                for (c = 0; c < 3; c++)
                    acc[c] += p[c] * kern[k + radius];
            }
            for (c = 0; c < 3; c++)
                dst[((size_t)y * w + x) * 3 + c] = (uint8_t)clampi((acc[c] + (1 << 19)) >> 20, 0, 255);
        }
    }
    free(kern);
    free(tmp);
    return true;
}

/* Hi-res outlines are detected at no more than this multiple of the target
 * size: enough for well-placed lines, and much faster on huge sources. */
#define HIRES_MAX_FACTOR 4

static uint8_t *outline_mask_canny(const EgtImage *source, int width, int height,
                                   int sensitivity);

uint8_t *egt_outline_mask_hires(const EgtImage *source, int width, int height,
                                int sensitivity)
{
    EgtImage reduced;
    uint8_t *mask;
    if (source->width <= width * HIRES_MAX_FACTOR && source->height <= height * HIRES_MAX_FACTOR)
        return outline_mask_canny(source, width, height, sensitivity);
    reduced = egt_resize_area(source, width * HIRES_MAX_FACTOR, height * HIRES_MAX_FACTOR);
    if (!reduced.pixels)
        return NULL;
    mask = outline_mask_canny(&reduced, width, height, sensitivity);
    egt_image_free(&reduced);
    return mask;
}

/* Canny edge detection on source: blur, gradient, thinning to one-pixel
 * ridges, and hysteresis (weak edges kept only where they continue
 * strong ones). The edges are then reduced to a width x height mask. */
static uint8_t *outline_mask_canny(const EgtImage *source, int width, int height,
                                   int sensitivity)
{
    int w = source->width, h = source->height, x, y, c;
    size_t n = (size_t)w * h, i, head, tail;
    double scale = 0.5 * ((double)w / width + (double)h / height);
    double sigma = scale * 0.5 < 1.0 ? 1.0 : scale * 0.5, gain;
    /* The low threshold continues lines started above the high one. */
    double high = step_threshold(sensitivity), low = high * 0.5;
    uint8_t *blur = malloc(n * 3);
    float *mag = malloc(n * sizeof(float));
    uint8_t *dir = malloc(n);
    uint8_t *edge = calloc(n, 1); /* 0 none, 1 candidate (weak), 2 edge */
    int32_t *queue = malloc(n * sizeof(int32_t));
    uint8_t *mask = calloc((size_t)width * height, 1);
    bool ok = blur && mag && dir && edge && queue && mask;

    if (ok)
        ok = gaussian_rgb(source, blur, sigma, &gain);
    if (!ok) {
        free(mask);
        mask = NULL;
        goto done;
    }

    /* Gradient: per pixel, the channel with the strongest Sobel response,
     * normalised so a clean step of height H gives about H. */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int xm = x > 0 ? x - 1 : 0, xp = x < w - 1 ? x + 1 : x;
            int ym = y > 0 ? y - 1 : 0, yp = y < h - 1 ? y + 1 : y;
            int best = 0, bgx = 0, bgy = 0;
#define B(xx, yy) blur[((size_t)(yy) * w + (xx)) * 3 + c]
            for (c = 0; c < 3; c++) {
                int gx = (B(xp, ym) + 2 * B(xp, y) + B(xp, yp)) - (B(xm, ym) + 2 * B(xm, y) + B(xm, yp));
                int gy = (B(xm, yp) + 2 * B(x, yp) + B(xp, yp)) - (B(xm, ym) + 2 * B(x, ym) + B(xp, ym));
                int m = gx * gx + gy * gy;
                if (m > best) {
                    best = m;
                    bgx = gx;
                    bgy = gy;
                }
            }
#undef B
            i = (size_t)y * w + x;
            mag[i] = (float)(sqrt((double)best) / gain);
            {
                int ax = abs(bgx), ay = abs(bgy);
                /* 0: horizontal gradient, 1: vertical, 2: down-right, 3: down-left */
                if (ay * 1000 <= ax * 414)
                    dir[i] = 0;
                else if (ay * 1000 >= ax * 2414)
                    dir[i] = 1;
                else
                    dir[i] = (bgx > 0) == (bgy > 0) ? 2 : 3;
            }
        }
    }

    /* Non-maximum suppression: keep pixels that peak across the edge. Ties
     * are broken one way so plateaus stay 1 pixel wide. */
    for (y = 1; y < h - 1; y++) {
        for (x = 1; x < w - 1; x++) {
            static const int DX[4] = {1, 0, 1, 1}, DY[4] = {0, 1, 1, -1};
            float m, a, b;
            i = (size_t)y * w + x;
            m = mag[i];
            if (m < low)
                continue;
            a = mag[(size_t)(y + DY[dir[i]]) * w + x + DX[dir[i]]];
            b = mag[(size_t)(y - DY[dir[i]]) * w + x - DX[dir[i]]];
            if (m >= a && m > b)
                edge[i] = m >= high ? 2 : 1;
        }
    }

    /* Hysteresis: weak pixels become edges if connected to a strong one. */
    tail = 0;
    for (i = 0; i < n; i++)
        if (edge[i] == 2)
            queue[tail++] = (int32_t)i;
    for (head = 0; head < tail; head++) {
        int qx = queue[head] % w, qy = queue[head] / w, dx, dy;
        for (dy = -1; dy <= 1; dy++) {
            for (dx = -1; dx <= 1; dx++) {
                int nx = qx + dx, ny = qy + dy;
                size_t nb = (size_t)ny * w + nx;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h || edge[nb] != 1)
                    continue;
                edge[nb] = 2;
                queue[tail++] = (int32_t)nb;
            }
        }
    }

    /* Reduce: a target pixel is outline if the edge runs through at least
     * half of its source box (a crossing line covers about box-size pixels). */
    for (y = 0; y < height; y++) {
        int y0 = (int)(((int64_t)y * h) / height), y1 = (int)(((int64_t)(y + 1) * h) / height);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (x = 0; x < width; x++) {
            int x0 = (int)(((int64_t)x * w) / width), x1 = (int)(((int64_t)(x + 1) * w) / width);
            int count = 0, need, sx, sy, side;
            if (x1 <= x0)
                x1 = x0 + 1;
            side = (x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0);
            need = (side + 1) / 2;
            for (sy = y0; sy < y1; sy++)
                for (sx = x0; sx < x1; sx++)
                    count += edge[(size_t)sy * w + sx] == 2;
            mask[(size_t)y * width + x] = count >= need;
        }
    }

done:
    free(blur);
    free(mag);
    free(dir);
    free(edge);
    free(queue);
    return mask;
}

float egt_snap_radius(float radius)
{
    if (radius < 0.25f)
        return 0.25f;
    if (radius < 1.0f)
        return roundf(radius * 4) / 4;
    if (radius > 4.0f)
        return 4.0f;
    return roundf(radius * 2) / 2;
}

EgtImage egt_resize_smoothed(const EgtImage *source, int width, int height,
                             bool nearest, const EgtFilters *f)
{
    float r = egt_snap_radius(f->smooth_radius);
    /* Work at 4x the target below radius 1 so small radii become whole
     * pixels, else at 2x so half steps do; never beyond the source
     * resolution (that adds cost, not detail). The same scale for every
     * radius of a range keeps the steps even (1x for whole radii and 2x for
     * half ones made 2.5 look unlike both 2 and 3). */
    float want = r < 1.0f ? 4.0f : 2.0f;
    float avail = fminf((float)source->width / width, (float)source->height / height);
    float scale = fmaxf(1.0f, fminf(want, avail));
    int work_w = (int)lroundf(width * scale), work_h = (int)lroundf(height * scale);
    int work_r = (int)lroundf(r * scale);
    EgtImage work, smoothed, out;
    bool ok;

    if (f->smooth == EGT_SMOOTH_NONE) {
        return nearest ? egt_resize_nearest(source, width, height)
                       : egt_resize_area(source, width, height);
    }
    if (work_r < 1)
        work_r = 1;
    if (nearest && work_w == width && work_h == height)
        work = egt_resize_nearest(source, width, height);
    else
        work = egt_resize_area(source, work_w, work_h);
    smoothed = egt_image_alloc(work.width, work.height);
    ok = work.pixels && smoothed.pixels;
    if (ok && f->smooth == EGT_SMOOTH_MEDIAN)
        smooth_median(&work, &smoothed, work_r);
    else if (ok && f->smooth == EGT_SMOOTH_SURFACE)
        smooth_surface(&work, &smoothed, work_r, f->smooth_threshold);
    else if (ok)
        ok = smooth_kuwahara(&work, &smoothed, work_r);
    egt_image_free(&work);
    if (!ok) {
        egt_image_free(&smoothed);
        return smoothed;
    }
    if (smoothed.width == width && smoothed.height == height)
        return smoothed;
    out = nearest ? egt_resize_nearest(&smoothed, width, height)
                  : egt_resize_area(&smoothed, width, height);
    egt_image_free(&smoothed);
    return out;
}

bool egt_filter(EgtImage *img, const EgtFilters *f)
{
    if (f->outline <= 0)
        return true;
    if (f->outline_mode == EGT_OUTLINE_HIRES) {
        size_t n = (size_t)img->width * img->height;
        uint8_t *mask;
        if (!f->outline_mask)
            return true;
        /* Prune a copy: the caller may cache the mask. */
        mask = malloc(n);
        if (!mask)
            return false;
        memcpy(mask, f->outline_mask, n);
        if (!prune_mask(mask, img->width, img->height, f->outline_min_length)) {
            free(mask);
            return false;
        }
        apply_mask(img, mask, f->outline_darkness);
        free(mask);
        return true;
    }
    return ink_outlines(img, f->outline, f->outline_darkness, f->outline_min_length);
}
