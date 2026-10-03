/* display.h - dockapp window and 64x64 framebuffer
 *
 * Copyright (C) 2026 Michael Sternberg
 * SPDX-License-Identifier: GPL-2.0-or-later
 * This file is part of wmyt; see COPYING for the full license text.
 */
#ifndef WMYT_DISPLAY_H
#define WMYT_DISPLAY_H

#include <stdint.h>
#include <X11/Xlib.h>

#define TILE 64
/* Drawable area inside the sunken bevel. */
#define INNER_X 4
#define INNER_Y 4
#define INNER_W 56
#define INNER_H 56

struct display {
	Display *dpy;
	Window win, iconwin;
	XImage *img;
	GC gc;
	uint32_t fb[TILE * TILE];	/* 0x00RRGGBB */
};

int display_open(struct display *d, const char *dpyname, int argc, char **argv);
void display_close(struct display *d);

/* Convert an X colour spec ("#20b2ae", "seagreen", ...) to 0x00RRGGBB. */
int display_parse_color(struct display *d, const char *spec, uint32_t *rgb);

/* Clear the tile to the bevelled empty background. */
void display_clear(struct display *d, uint32_t bg);
void display_fill(struct display *d, int x, int y, int w, int h, uint32_t c);
/* Copy a w*h 0x00RRGGBB buffer into the framebuffer at (x, y). */
void display_blit(struct display *d, int x, int y, int w, int h, const uint32_t *src);
/* Push the framebuffer to the screen. */
void display_flush(struct display *d);

#endif
