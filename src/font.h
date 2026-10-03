/* font.h - 5x7 LED-style pixel font for wmtube
 *
 * Copyright (C) 2026 Michael Sternberg
 * SPDX-License-Identifier: GPL-2.0-or-later
 * This file is part of wmtube; see COPYING for the full license text.
 */
#ifndef WMTUBE_FONT_H
#define WMTUBE_FONT_H

#include <stdint.h>

#define FONT_HEIGHT 7

/* Width in pixels of the rendered string (no trailing gap). */
int font_text_width(const char *s);

/*
 * Draw string s into a 32-bit framebuffer (fb_w pixels wide) with its top-left
 * corner at (x, y). Pixels outside [clip_x0, clip_x1) are not touched.
 */
void font_draw(uint32_t *fb, int fb_w, int x, int y, const char *s,
               uint32_t color, int clip_x0, int clip_x1);

/*
 * Normalise arbitrary (UTF-8) text into something the font can draw:
 * upper-case ASCII, unsupported characters dropped, whitespace collapsed.
 */
void font_normalize(const char *in, char *out, int outlen);

#endif
