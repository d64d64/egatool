/*
 * Image clipboard. Native on Windows; on Linux it uses wl-paste/wl-copy or
 * xclip when installed.
 */
#ifndef EGATOOL_CLIPBOARD_H
#define EGATOOL_CLIPBOARD_H

#include <stdbool.h>
#include "engine.h"

/* Returns an image with NULL pixels if the clipboard holds no image. */
EgtImage egt_clipboard_get_image(void);
bool egt_clipboard_set_image(const EgtImage *img);

#endif
