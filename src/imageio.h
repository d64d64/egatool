/*
 * Image file loading/saving via stb_image (static, so it cannot clash with
 * the copy inside raylib).
 */
#ifndef EGATOOL_IMAGEIO_H
#define EGATOOL_IMAGEIO_H

#include <stdbool.h>
#include <stddef.h>
#include "engine.h"

/* Loads PNG/JPG/BMP/GIF/TGA/PSD as RGBA8. Returns an image with NULL pixels
 * on failure. Free with egt_image_free(). */
EgtImage egt_load_image(const char *path);

/* Saves by extension: .png, .bmp or .tga (anything else is saved as PNG). */
bool egt_save_image(const char *path, const EgtImage *img);

/* Like egt_save_image, and PNGs also get comment as a "Comment" text
 * chunk (with "Software" = egatool); BMP and TGA have no place for it.
 * comment may be NULL. */
bool egt_save_image_comment(const char *path, const EgtImage *img, const char *comment);

/* Decodes an in-memory image file (PNG/JPG/...). */
EgtImage egt_decode_image(const uint8_t *data, size_t len);

typedef void EgtWriteFunc(void *ctx, void *data, int size);

/* Encodes as PNG, streaming the bytes to write(). */
bool egt_encode_png(const EgtImage *img, EgtWriteFunc *write, void *ctx);

const char *egt_load_error(void);

#endif
