/* selection.h - fetch a URL from the X PRIMARY / CLIPBOARD selections
 *
 * Copyright (C) 2026 Michael Sternberg
 * SPDX-License-Identifier: GPL-2.0-or-later
 * This file is part of wmyt; see COPYING for the full license text.
 */
#ifndef WMYT_SELECTION_H
#define WMYT_SELECTION_H

#include <X11/Xlib.h>

/* Start an asynchronous fetch: PRIMARY first, then CLIPBOARD. */
void selection_request(Display *dpy, Window win, Time t);

/*
 * Handle a SelectionNotify event. Returns 1 and fills url when a usable
 * URL was found, 0 while still trying, -1 when both selections failed.
 */
int selection_notify(Display *dpy, XSelectionEvent *ev, char *url, int urllen);

#endif
