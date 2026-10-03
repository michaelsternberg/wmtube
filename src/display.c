/* display.c - dockapp window and 64x64 framebuffer
 *
 * Copyright (C) 2026 Michael Sternberg
 * SPDX-License-Identifier: GPL-2.0-or-later
 * This file is part of wmyt; see COPYING for the full license text.
 *
 * Classic wmgeneral-style setup: a withdrawn main window plus an icon
 * window that Window Maker swallows into the dock/clip, shaped so only the
 * bevelled square is visible.
 */
#include <stdio.h>
#include <string.h>

#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include "display.h"

#define BEVEL_DARK  0x000000
#define BEVEL_LIGHT 0xC7C5C8

int display_open(struct display *d, const char *dpyname, int argc, char **argv)
{
	XSizeHints sh;
	XWMHints wmh;
	XClassHint ch;
	Pixmap mask;
	GC mgc;
	Window root;
	Visual *vis;
	int scr, depth;
	long ev = ButtonPressMask | ExposureMask | StructureNotifyMask;

	memset(d, 0, sizeof(*d));
	d->dpy = XOpenDisplay(dpyname);
	if (!d->dpy) {
		fprintf(stderr, "wmyt: cannot open display %s\n", XDisplayName(dpyname));
		return -1;
	}
	scr = DefaultScreen(d->dpy);
	root = RootWindow(d->dpy, scr);
	vis = DefaultVisual(d->dpy, scr);
	depth = DefaultDepth(d->dpy, scr);
	if (depth < 24 || vis->red_mask != 0xff0000 || vis->green_mask != 0xff00 ||
	    vis->blue_mask != 0xff) {
		fprintf(stderr, "wmyt: need a 24/32-bit RGB TrueColor visual\n");
		XCloseDisplay(d->dpy);
		return -1;
	}

	d->win = XCreateSimpleWindow(d->dpy, root, 0, 0, TILE, TILE, 0, 0, 0);
	d->iconwin = XCreateSimpleWindow(d->dpy, d->win, 0, 0, TILE, TILE, 0, 0, 0);
	XSetWindowBackgroundPixmap(d->dpy, d->win, None);
	XSetWindowBackgroundPixmap(d->dpy, d->iconwin, None);

	sh.flags = USSize | USPosition;
	sh.x = sh.y = 0;
	sh.width = sh.height = TILE;
	XSetWMNormalHints(d->dpy, d->win, &sh);

	ch.res_name = "wmyt";
	ch.res_class = "DockApp";
	XSetClassHint(d->dpy, d->win, &ch);
	XStoreName(d->dpy, d->win, "wmyt");
	XSetIconName(d->dpy, d->win, "wmyt");

	wmh.flags = StateHint | IconWindowHint | IconPositionHint | WindowGroupHint;
	wmh.initial_state = WithdrawnState;
	wmh.icon_window = d->iconwin;
	wmh.icon_x = wmh.icon_y = 0;
	wmh.window_group = d->win;
	XSetWMHints(d->dpy, d->win, &wmh);
	XSetCommand(d->dpy, d->win, argv, argc);

	XSelectInput(d->dpy, d->win, ev | PropertyChangeMask);
	XSelectInput(d->dpy, d->iconwin, ev);

	/* Shape: only the bevelled square (3..60) is visible. */
	mask = XCreatePixmap(d->dpy, root, TILE, TILE, 1);
	mgc = XCreateGC(d->dpy, mask, 0, NULL);
	XSetForeground(d->dpy, mgc, 0);
	XFillRectangle(d->dpy, mask, mgc, 0, 0, TILE, TILE);
	XSetForeground(d->dpy, mgc, 1);
	XFillRectangle(d->dpy, mask, mgc, INNER_X - 1, INNER_Y - 1, INNER_W + 2, INNER_H + 2);
	XShapeCombineMask(d->dpy, d->win, ShapeBounding, 0, 0, mask, ShapeSet);
	XShapeCombineMask(d->dpy, d->iconwin, ShapeBounding, 0, 0, mask, ShapeSet);
	XFreeGC(d->dpy, mgc);
	XFreePixmap(d->dpy, mask);

	d->gc = XCreateGC(d->dpy, d->win, 0, NULL);
	d->img = XCreateImage(d->dpy, vis, depth, ZPixmap, 0, (char *)d->fb,
	                      TILE, TILE, 32, TILE * 4);
	if (!d->img || d->img->bits_per_pixel != 32) {
		fprintf(stderr, "wmyt: unsupported image format\n");
		return -1;
	}

	XMapWindow(d->dpy, d->win);
	XFlush(d->dpy);
	return 0;
}

void display_close(struct display *d)
{
	if (!d->dpy)
		return;
	if (d->img) {
		d->img->data = NULL;	/* fb is not malloc'd */
		XDestroyImage(d->img);
	}
	XFreeGC(d->dpy, d->gc);
	XCloseDisplay(d->dpy);
	d->dpy = NULL;
}

int display_parse_color(struct display *d, const char *spec, uint32_t *rgb)
{
	XColor c;

	if (!XParseColor(d->dpy, DefaultColormap(d->dpy, DefaultScreen(d->dpy)), spec, &c))
		return -1;
	*rgb = ((uint32_t)(c.red >> 8) << 16) | ((uint32_t)(c.green >> 8) << 8) | (c.blue >> 8);
	return 0;
}

void display_fill(struct display *d, int x, int y, int w, int h, uint32_t c)
{
	int i, j;

	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++)
			d->fb[j * TILE + i] = c;
}

void display_clear(struct display *d, uint32_t bg)
{
	display_fill(d, 0, 0, TILE, TILE, 0);
	/* sunken bevel: dark top/left, light bottom/right */
	display_fill(d, INNER_X - 1, INNER_Y - 1, INNER_W + 1, 1, BEVEL_DARK);
	display_fill(d, INNER_X - 1, INNER_Y - 1, 1, INNER_H + 1, BEVEL_DARK);
	display_fill(d, INNER_X, INNER_Y + INNER_H, INNER_W + 1, 1, BEVEL_LIGHT);
	display_fill(d, INNER_X + INNER_W, INNER_Y, 1, INNER_H + 1, BEVEL_LIGHT);
	display_fill(d, INNER_X, INNER_Y, INNER_W, INNER_H, bg);
}

void display_blit(struct display *d, int x, int y, int w, int h, const uint32_t *src)
{
	int j;

	for (j = 0; j < h; j++)
		memcpy(&d->fb[(y + j) * TILE + x], &src[j * w], (size_t)w * 4);
}

void display_flush(struct display *d)
{
	XPutImage(d->dpy, d->iconwin, d->gc, d->img, 0, 0, 0, 0, TILE, TILE);
	XPutImage(d->dpy, d->win, d->gc, d->img, 0, 0, 0, 0, TILE, TILE);
	XFlush(d->dpy);
}
