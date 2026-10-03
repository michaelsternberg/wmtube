/* selection.c - fetch a URL from the X PRIMARY / CLIPBOARD selections */
#include <ctype.h>
#include <string.h>

#include <X11/Xatom.h>

#include "selection.h"

static Atom prop_atom(Display *dpy)
{
	return XInternAtom(dpy, "WMYT_SELECTION", False);
}

static void request(Display *dpy, Window win, Atom sel, Time t)
{
	XConvertSelection(dpy, sel, XInternAtom(dpy, "UTF8_STRING", False),
	                  prop_atom(dpy), win, t);
}

void selection_request(Display *dpy, Window win, Time t)
{
	request(dpy, win, XA_PRIMARY, t);
}

/* Trim whitespace and accept things that look like a URL or a video id. */
static int usable(const char *in, char *url, int urllen)
{
	const char *s = in, *e;
	int n;

	while (*s && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		e--;
	n = (int)(e - s);
	if (n <= 0 || n >= urllen || memchr(s, '\n', (size_t)n))
		return 0;
	if (memmem(s, (size_t)n, "://", 3) || memmem(s, (size_t)n, "youtu", 5)) {
		memcpy(url, s, (size_t)n);
		url[n] = '\0';
		return 1;
	}
	return 0;
}

int selection_notify(Display *dpy, XSelectionEvent *ev, char *url, int urllen)
{
	Atom type;
	int format, ok = 0;
	unsigned long nitems, after;
	unsigned char *data = NULL;

	if (ev->property != None &&
	    XGetWindowProperty(dpy, ev->requestor, ev->property, 0, 4096, True,
	                       AnyPropertyType, &type, &format, &nitems, &after,
	                       &data) == Success && data) {
		if (format == 8)
			ok = usable((const char *)data, url, urllen);
		XFree(data);
	}
	if (ok)
		return 1;
	if (ev->selection == XA_PRIMARY) {
		request(dpy, ev->requestor, XInternAtom(dpy, "CLIPBOARD", False), ev->time);
		return 0;
	}
	return -1;
}
