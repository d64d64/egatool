/*
 * Image clipboard: Win32 on Windows; on Linux, wl-paste/wl-copy (Wayland)
 * or xclip (X11) run as separate programs, with PNG data through a pipe.
 * Copyright (c) 2026 d64d64. zlib licence, see LICENSE.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "clipboard.h"
#include "imageio.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

/* Kept apart from raylib.h, whose names clash with windows.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

EgtImage egt_clipboard_get_image(void)
{
    EgtImage img = {0, 0, NULL};
    HGLOBAL h;
    const BITMAPINFOHEADER *bih;
    const uint8_t *bits;
    int w, hgt, x, y, bpp, stride;
    bool bottom_up;

    if (!OpenClipboard(NULL))
        return img;
    h = GetClipboardData(CF_DIB);
    bih = h ? (const BITMAPINFOHEADER *)GlobalLock(h) : NULL;
    if (!bih) {
        CloseClipboard();
        return img;
    }

    bpp = bih->biBitCount;
    w = bih->biWidth;
    hgt = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
    bottom_up = bih->biHeight > 0;
    if ((bpp == 24 || bpp == 32) &&
        (bih->biCompression == BI_RGB || bih->biCompression == BI_BITFIELDS) &&
        w > 0 && hgt > 0) {
        /* A 40-byte header with BI_BITFIELDS is followed by 3 colour masks.
         * We assume the usual BGR(A) layout they describe. */
        bits = (const uint8_t *)bih + bih->biSize;
        if (bih->biSize == sizeof(BITMAPINFOHEADER) && bih->biCompression == BI_BITFIELDS)
            bits += 3 * sizeof(DWORD);
        stride = ((w * bpp + 31) / 32) * 4;
        img = egt_image_alloc(w, hgt);
        for (y = 0; img.pixels && y < hgt; y++) {
            const uint8_t *s = bits + (size_t)(bottom_up ? hgt - 1 - y : y) * stride;
            uint8_t *d = img.pixels + (size_t)y * w * 4;
            for (x = 0; x < w; x++, s += bpp / 8, d += 4) {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = 255;
            }
        }
    }
    GlobalUnlock(h);
    CloseClipboard();
    return img;
}

bool egt_clipboard_set_image(const EgtImage *img)
{
    size_t row = (size_t)img->width * 4;
    size_t size = sizeof(BITMAPINFOHEADER) + row * img->height;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size);
    BITMAPINFOHEADER *bih;
    uint8_t *bits;
    int x, y;

    if (!h)
        return false;
    bih = (BITMAPINFOHEADER *)GlobalLock(h);
    memset(bih, 0, sizeof *bih);
    bih->biSize = sizeof *bih;
    bih->biWidth = img->width;
    bih->biHeight = img->height; /* bottom-up */
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bits = (uint8_t *)(bih + 1);
    for (y = 0; y < img->height; y++) {
        const uint8_t *s = img->pixels + (size_t)(img->height - 1 - y) * row;
        uint8_t *d = bits + (size_t)y * row;
        for (x = 0; x < img->width; x++, s += 4, d += 4) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = 255;
        }
    }
    GlobalUnlock(h);

    if (!OpenClipboard(NULL)) {
        GlobalFree(h);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, h)) {
        CloseClipboard();
        GlobalFree(h);
        return false;
    }
    CloseClipboard();
    return true;
}

#else

#include <stdio.h>

/* Whether a program is installed (on the PATH). */
static bool have_command(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

/* A Wayland session: use wl-paste/wl-copy rather than xclip. */
static bool use_wayland(void)
{
    const char *wl = getenv("WAYLAND_DISPLAY");
    return wl && *wl;
}

EgtImage egt_clipboard_get_image(void)
{
    EgtImage img = {0, 0, NULL};
    const char *cmd = NULL;
    FILE *p;
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0, n;

    if (use_wayland() && have_command("wl-paste"))
        cmd = "wl-paste --no-newline --type image/png 2>/dev/null";
    else if (have_command("xclip"))
        cmd = "xclip -selection clipboard -t image/png -o 2>/dev/null";
    if (!cmd || !(p = popen(cmd, "r")))
        return img;

    for (;;) {
        if (len == cap) {
            uint8_t *nb = realloc(buf, cap = cap ? cap * 2 : 1 << 20);
            if (!nb)
                break;
            buf = nb;
        }
        n = fread(buf + len, 1, cap - len, p);
        if (n == 0)
            break;
        len += n;
    }
    pclose(p);
    if (buf && len > 0)
        img = egt_decode_image(buf, len);
    free(buf);
    return img;
}

/* PNG encoder output, sent to the clipboard program's input. */
static void write_to_pipe(void *ctx, void *data, int size)
{
    fwrite(data, 1, (size_t)size, (FILE *)ctx);
}

bool egt_clipboard_set_image(const EgtImage *img)
{
    const char *cmd = NULL;
    FILE *p;
    bool ok;

    if (use_wayland() && have_command("wl-copy"))
        cmd = "wl-copy --type image/png";
    else if (have_command("xclip"))
        cmd = "xclip -selection clipboard -t image/png -i";
    if (!cmd || !(p = popen(cmd, "w")))
        return false;
    ok = egt_encode_png(img, write_to_pipe, p);
    return pclose(p) == 0 && ok;
}

#endif
