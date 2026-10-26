#include "imageio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb defines many helpers we never call. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* Takes ownership of stb's buffer, copying it into our own allocation so
 * egt_image_free() can be used. Alpha is dropped. */
static EgtImage from_stbi(uint8_t *data, int w, int h)
{
    EgtImage img = {0, 0, NULL};
    if (!data)
        return img;
    img = egt_image_alloc(w, h);
    if (img.pixels) {
        size_t i, count = (size_t)w * (size_t)h;
        memcpy(img.pixels, data, count * 4);
        for (i = 0; i < count; i++)
            img.pixels[i * 4 + 3] = 255;
    }
    stbi_image_free(data);
    return img;
}

EgtImage egt_load_image(const char *path)
{
    int w = 0, h = 0, n;
    uint8_t *data = stbi_load(path, &w, &h, &n, 4);
    return from_stbi(data, w, h);
}

EgtImage egt_decode_image(const uint8_t *data, size_t len)
{
    int w = 0, h = 0, n;
    uint8_t *pixels = len > 0x7fffffff ? NULL
                    : stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
    return from_stbi(pixels, w, h);
}

bool egt_encode_png(const EgtImage *img, EgtWriteFunc *write, void *ctx)
{
    return stbi_write_png_to_func(write, ctx, img->width, img->height, 4,
                                  img->pixels, img->width * 4) != 0;
}

const char *egt_load_error(void)
{
    return stbi_failure_reason();
}

static bool has_ext(const char *path, const char *ext)
{
    size_t lp = strlen(path), le = strlen(ext), i;
    if (lp < le)
        return false;
    for (i = 0; i < le; i++) {
        char c = path[lp - le + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != ext[i])
            return false;
    }
    return true;
}

bool egt_save_image(const char *path, const EgtImage *img)
{
    if (has_ext(path, ".bmp"))
        return stbi_write_bmp(path, img->width, img->height, 4, img->pixels) != 0;
    if (has_ext(path, ".tga"))
        return stbi_write_tga(path, img->width, img->height, 4, img->pixels) != 0;
    return stbi_write_png(path, img->width, img->height, 4, img->pixels, img->width * 4) != 0;
}

/* Appends one PNG text chunk at *pos (tEXt for ASCII, else iTXt, which is
 * UTF-8). Returns the new position. */
static uint8_t *put_text_chunk(uint8_t *pos, const char *key, const char *text)
{
    size_t klen = strlen(key), tlen = strlen(text), i, len;
    bool ascii = true;
    uint8_t *data;
    unsigned crc;
    for (i = 0; i < tlen; i++)
        if ((uint8_t)text[i] >= 0x80)
            ascii = false;
    /* iTXt: keyword, 0, compression flag and method, empty language tag
     * and translated keyword (each 0-terminated), then the text. */
    len = klen + 1 + (ascii ? 0 : 4) + tlen;
    pos[0] = (uint8_t)(len >> 24);
    pos[1] = (uint8_t)(len >> 16);
    pos[2] = (uint8_t)(len >> 8);
    pos[3] = (uint8_t)len;
    memcpy(pos + 4, ascii ? "tEXt" : "iTXt", 4);
    data = pos + 8;
    memcpy(data, key, klen + 1);
    if (!ascii)
        memset(data + klen + 1, 0, 4);
    memcpy(data + len - tlen, text, tlen);
    crc = stbiw__crc32(pos + 4, (int)(len + 4));
    data[len] = (uint8_t)(crc >> 24);
    data[len + 1] = (uint8_t)(crc >> 16);
    data[len + 2] = (uint8_t)(crc >> 8);
    data[len + 3] = (uint8_t)crc;
    return data + len + 4;
}

bool egt_save_image_comment(const char *path, const EgtImage *img, const char *comment)
{
    /* PNG signature (8) + IHDR chunk (4 + 4 + 13 + 4): text goes after it. */
    enum { HEAD = 33 };
    static const char *SOFTWARE = "egatool";
    int png_len = 0;
    unsigned char *png;
    uint8_t *out, *pos;
    size_t extra, total;
    FILE *f;
    bool ok;

    if (!comment || has_ext(path, ".bmp") || has_ext(path, ".tga"))
        return egt_save_image(path, img);
    if (strlen(comment) > 0x7fff0000u)
        return false;
    png = stbi_write_png_to_mem(img->pixels, img->width * 4, img->width, img->height, 4, &png_len);
    if (!png || png_len < HEAD) {
        free(png);
        return false;
    }
    extra = 2 * 12 + strlen("Software") + 1 + strlen(SOFTWARE)
          + strlen("Comment") + 1 + 4 + strlen(comment);
    total = (size_t)png_len + extra;
    out = malloc(total);
    if (!out) {
        free(png);
        return false;
    }
    memcpy(out, png, HEAD);
    pos = put_text_chunk(out + HEAD, "Software", SOFTWARE);
    pos = put_text_chunk(pos, "Comment", comment);
    memcpy(pos, png + HEAD, (size_t)png_len - HEAD);
    total = (size_t)(pos - out) + (size_t)png_len - HEAD;
    free(png);

    f = stbiw__fopen(path, "wb");
    ok = f && fwrite(out, 1, total, f) == total;
    if (f && fclose(f) != 0)
        ok = false;
    free(out);
    return ok;
}
