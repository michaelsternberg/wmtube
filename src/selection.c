/* selection.c - fetch a URL from the X PRIMARY / CLIPBOARD selections */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

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

/*
 * Selections can hold arbitrary (even hostile) text, so only plain web URLs
 * are accepted: http:// or https://, or a scheme-less YouTube address which
 * gets https:// prepended. Anything else mpv understands (local paths,
 * file://, av://, edl://, memory://, ...) is refused, as is any text with
 * whitespace, control or non-ASCII characters inside it.
 */
static int usable(const char *in, char *url, int urllen)
{
	static const char *const hosts[] = {
		"youtube.com/", "www.youtube.com/", "m.youtube.com/",
		"music.youtube.com/", "youtu.be/",
	};
	const char *s = in, *e, *p, *prefix = NULL;
	size_t i;
	int n;

	while (*s && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		e--;
	n = (int)(e - s);
	if (n <= 0)
		return 0;
	/* printable ASCII only (browsers percent-encode everything else) */
	for (p = s; p < e; p++)
		if ((unsigned char)*p <= ' ' || (unsigned char)*p >= 0x7f)
			return 0;

	if (!strncasecmp(s, "http://", 7) || !strncasecmp(s, "https://", 8)) {
		prefix = "";
	} else {
		for (i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++)
			if (!strncasecmp(s, hosts[i], strlen(hosts[i])))
				prefix = "https://";
	}
	if (!prefix || (int)strlen(prefix) + n >= urllen)
		return 0;
	snprintf(url, (size_t)urllen, "%s%.*s", prefix, n, s);
	return 1;
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
