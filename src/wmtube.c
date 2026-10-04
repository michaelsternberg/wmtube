/* wmtube - a Window Maker dockapp that plays YouTube / PeerTube videos
 *
 * Copyright (C) 2026 Michael Sternberg
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "display.h"
#include "font.h"
#include "player.h"
#include "selection.h"

#define VERSION "0.1"

#define TICK_MS        66	/* marquee speed: ~15 px/s */
#define MARQUEE_GAP    18	/* pixels of blank space before the title repeats */
#define DBLCLICK_MS    300
#define VOLUME_SHOW_MS 1500
#define ROW_PAD        3	/* gap between a text row and the video */
#define MAX_TIME       (1000.0 * 3600 - 1)	/* clamp stream times to 999:59:59 */

#define HELP_TOP    "MIDDLE-CLICK TO PLAY URL"
#define HELP_BOTTOM "LEFT: PAUSE  *  DOUBLE: FULL SIZE  *  RIGHT: TEXT ROWS  *  " \
                    "SHIFT+LEFT: -10S  *  SHIFT+RIGHT: +10S  *  " \
                    "CTRL+LEFT: PREV TRACK  *  CTRL+RIGHT: NEXT TRACK  *  WHEEL: VOLUME  *  " \
                    "LEFT NOW: HIDE HELP"

enum mode { MODE_BOTH, MODE_TITLE, MODE_TIME, MODE_NONE, MODE_COUNT };
static const char *mode_names[] = { "both", "title", "time", "none" };

static struct {
	struct display d;
	struct player p;
	enum mode mode;
	uint32_t fg, dim, bg;

	char url[4096];
	char title[512];	/* normalised media-title */
	char status[256];	/* overrides the title when non-empty */
	char ytdl_err[256];	/* last yt-dlp error, shown instead of mpv's */
	double tpos, dur, volume;
	int paused, have_video, loading;
	int vo_ok;		/* mpv has a video output (file has video) */
	int seekable;		/* false for live streams / internet radio */
	int show_help;		/* help text while idle; left click toggles */

	int marquee_off;	/* title scroll position */
	int time_marquee_off;	/* bottom row scroll position (long times, help) */
	double volume_until;	/* monotonic seconds */
	Time last_click;
	int dirty;
	volatile sig_atomic_t quit;
} S;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void set_status(const char *s)
{
	font_normalize(s, S.status, sizeof(S.status));
	S.marquee_off = 0;
	S.dirty = 1;
}

/* No video loaded or loading: startup, after an error, or an empty paste. */
static int idle(void)
{
	return !S.have_video && !S.loading;
}

static int in_path(const char *prog)
{
	const char *path = getenv("PATH");
	char buf[4096];

	if (!path)
		return 0;
	while (*path) {
		const char *e = strchr(path, ':');
		int n = e ? (int)(e - path) : (int)strlen(path);

		snprintf(buf, sizeof(buf), "%.*s/%s", n, n ? path : ".", prog);
		if (access(buf, X_OK) == 0)
			return 1;
		path += n + (e ? 1 : 0);
	}
	return 0;
}

/* ---- drawing ---------------------------------------------------------- */

static void fmt_time(double t, int hours, int wide_min, char *out, size_t len)
{
	int s = t < 0 ? 0 : (int)t;

	if (hours)
		snprintf(out, len, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else if (wide_min)
		snprintf(out, len, "%02d:%02d", s / 60, s % 60);
	else
		snprintf(out, len, "%d:%02d", s / 60, s % 60);
}

static const char *title_text(void)
{
	if (S.status[0])
		return S.status;
	if (idle() && S.show_help)
		return HELP_TOP;
	if (S.title[0])
		return S.title;
	return "WMTUBE";
}

/* Draw t centred if it fits, otherwise as a seamless scrolling marquee. */
static void draw_marquee(int y, const char *t, uint32_t color, int offset)
{
	int w = font_text_width(t);
	int x0 = INNER_X, x1 = INNER_X + INNER_W;
	int period, off;

	if (w <= INNER_W - 2) {
		font_draw(S.d.fb, TILE, x0 + (INNER_W - w) / 2, y, t, color, x0, x1);
		return;
	}
	/* draw twice so the wrap-around is seamless */
	period = w + MARQUEE_GAP;
	off = offset % period;
	font_draw(S.d.fb, TILE, x0 + 1 - off, y, t, color, x0, x1);
	font_draw(S.d.fb, TILE, x0 + 1 - off + period, y, t, color, x0, x1);
}

static void draw_title_row(int y)
{
	draw_marquee(y, title_text(), S.fg, S.marquee_off);
}

static void draw_volume(int y)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "VOL %d", (int)(S.volume + 0.5));
	font_draw(S.d.fb, TILE, INNER_X + (INNER_W - font_text_width(buf)) / 2, y,
	          buf, S.fg, INNER_X, INNER_X + INNER_W);
}

/*
 * Format the time row. Returns 1 if elapsed and total fit side by side
 * (a = elapsed, b = total), 0 if only a is meaningful (live stream), or 2 if
 * they do not fit and a holds "elapsed / total" for the marquee.
 */
static int time_text(char *a, size_t alen, char *b, size_t blen)
{
	int hours = S.dur >= 3600 || S.tpos >= 3600;
	int wide = S.dur >= 600 || S.tpos >= 600;
	char e[16];

	fmt_time(S.tpos, hours, wide, a, alen);
	/* live stream / radio: mpv's "duration" there is just elapsed time
	 * plus the buffer, so show elapsed only */
	if (S.dur <= 0 || !S.seekable)
		return 0;
	fmt_time(S.dur, hours, wide, b, blen);
	if (font_text_width(a) + font_text_width(b) + 2 <= INNER_W - 2)
		return 1;
	snprintf(e, sizeof(e), "%s", a);
	snprintf(a, alen, "%s / %s", e, b);
	return 2;
}

static int help_visible(void)
{
	return idle() && S.show_help;
}

static int time_marquee_active(void)
{
	char a[40], b[16];

	if (now() < S.volume_until)
		return 0;
	if (help_visible())	/* help overrides the display mode */
		return 1;
	return (S.mode == MODE_BOTH || S.mode == MODE_TIME) && S.have_video &&
	       time_text(a, sizeof(a), b, sizeof(b)) == 2;
}

static void draw_time_row(int y)
{
	char a[40], b[16];
	uint32_t c = S.paused ? S.dim : S.fg;
	int x0 = INNER_X + 1, x1 = INNER_X + INNER_W - 1;

	if (now() < S.volume_until) {
		draw_volume(y);
		return;
	}
	if (help_visible()) {
		draw_marquee(y, HELP_BOTTOM, S.fg, S.time_marquee_off);
		return;
	}
	if (!S.have_video)
		return;
	switch (time_text(a, sizeof(a), b, sizeof(b))) {
	case 0:
		font_draw(S.d.fb, TILE, x0, y, a, c, INNER_X, INNER_X + INNER_W);
		break;
	case 1:
		font_draw(S.d.fb, TILE, x0, y, a, c, INNER_X, INNER_X + INNER_W);
		font_draw(S.d.fb, TILE, x1 - font_text_width(b), y, b, c,
		          INNER_X, INNER_X + INNER_W);
		break;
	case 2:
		draw_marquee(y, a, c, S.time_marquee_off);
		break;
	}
}

static void redraw(void)
{
	/* fixed layout: the video never moves, hidden rows are left blank
	 * pad 2 | title 7 | gap 3 | video 32 | gap 3 | time 7 | pad 2 */
	const int title_y = INNER_Y + (INNER_H - VIDEO_H) / 2 - ROW_PAD - FONT_HEIGHT;
	const int video_y = title_y + FONT_HEIGHT + ROW_PAD;
	const int time_y = video_y + VIDEO_H + ROW_PAD;
	int show_title = S.mode == MODE_BOTH || S.mode == MODE_TITLE || help_visible();
	int show_time = S.mode == MODE_BOTH || S.mode == MODE_TIME || help_visible();

	display_clear(&S.d, S.bg);
	if (show_title)
		draw_title_row(title_y);
	display_blit(&S.d, INNER_X, video_y, VIDEO_W, VIDEO_H, S.p.frame);
	if (show_time)
		draw_time_row(time_y);
	else if (now() < S.volume_until)
		draw_volume(time_y);
	display_flush(&S.d);
	S.dirty = 0;
}

static int marquee_active(void)
{
	return (S.mode == MODE_BOTH || S.mode == MODE_TITLE || help_visible()) &&
	       font_text_width(title_text()) > INNER_W - 2;
}

/* ---- actions ---------------------------------------------------------- */

/* Back to the idle tile: blank video, no title or time. Whether the help
 * text shows is left to S.show_help, i.e. as the user last set it. */
static void unload(void)
{
	S.title[0] = '\0';
	S.tpos = S.dur = -1;
	S.have_video = 0;
	S.loading = 0;
	S.marquee_off = S.time_marquee_off = 0;
	memset(S.p.frame, 0, sizeof(S.p.frame));
	set_status("");
}

static void load(const char *url)
{
	snprintf(S.url, sizeof(S.url), "%s", url);
	S.title[0] = '\0';
	S.tpos = S.dur = -1;
	S.time_marquee_off = 0;
	S.have_video = 0;
	S.loading = 1;
	memset(S.p.frame, 0, sizeof(S.p.frame));
	set_status("LOADING");
	player_load(&S.p, url);
}

/* Open the current video full-size in a separate mpv. */
static void open_external(void)
{
	char start[32];
	pid_t pid;

	/* only a URL that mpv has actually loaded is handed on */
	if (!S.url[0] || !S.have_video)
		return;
	snprintf(start, sizeof(start), "--start=%.0f", S.tpos > 0 ? S.tpos : 0);
	/* double fork so the child is reparented to init and never a zombie;
	 * SIGCHLD must stay at its default so mpv can reap yt-dlp itself */
	pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			/* "--": the URL must never be parsed as an mpv option */
			execlp("mpv", "mpv", "--force-window=immediate", start, "--",
			       S.url, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (pid > 0) {
		waitpid(pid, NULL, 0);
		mpv_set_property_string(S.p.mpv, "pause", "yes");
	}
}

static void handle_button(XButtonEvent *ev)
{
	int shift = ev->state & ShiftMask;
	int ctrl = ev->state & ControlMask;

	switch (ev->button) {
	case Button1:
		if (ctrl) {
			player_playlist_step(&S.p, -1);
			break;
		}
		if (shift) {
			player_seek(&S.p, -10, "relative");
			break;
		}
		if (idle()) {
			/* nothing to pause: toggle the help text instead, and
			 * clear any error so the tile can sit quietly */
			S.show_help = !S.show_help;
			S.status[0] = '\0';
			S.marquee_off = S.time_marquee_off = 0;
			S.dirty = 1;
			break;
		}
		if (ev->time - S.last_click < DBLCLICK_MS) {
			S.last_click = 0;
			open_external();
			break;
		}
		S.last_click = ev->time;
		player_toggle_pause(&S.p);
		break;
	case Button2:
		selection_request(S.d.dpy, S.d.win, ev->time);
		break;
	case Button3:
		if (ctrl) {
			player_playlist_step(&S.p, 1);
			break;
		}
		if (shift) {
			player_seek(&S.p, 10, "relative");
			break;
		}
		S.mode = (S.mode + 1) % MODE_COUNT;
		S.dirty = 1;
		break;
	case Button4:
		player_add_volume(&S.p, 5);
		S.volume_until = now() + VOLUME_SHOW_MS / 1000.0;
		break;
	case Button5:
		player_add_volume(&S.p, -5);
		S.volume_until = now() + VOLUME_SHOW_MS / 1000.0;
		break;
	}
}

static void handle_x_events(void)
{
	XEvent ev;
	char url[sizeof(S.url)];

	while (XPending(S.d.dpy)) {
		XNextEvent(S.d.dpy, &ev);
		switch (ev.type) {
		case Expose:
			S.dirty = 1;
			break;
		case ButtonPress:
			handle_button(&ev.xbutton);
			break;
		case SelectionNotify:
			switch (selection_notify(S.d.dpy, &ev.xselection, url, sizeof(url))) {
			case 1:
				load(url);
				break;
			case -1:
				set_status("NO URL IN SELECTION");
				break;
			}
			break;
		case DestroyNotify:
			S.quit = 1;
			break;
		}
	}
}

/* Times come from the stream: keep them finite and within int range. */
static double sane_time(double t)
{
	if (!isfinite(t) || t < 0)
		return -1;
	return t > MAX_TIME ? MAX_TIME : t;
}

static void handle_property(mpv_event_property *pr)
{
	if (!strcmp(pr->name, "media-title")) {
		if (pr->format == MPV_FORMAT_STRING)
			font_normalize(*(char **)pr->data, S.title, sizeof(S.title));
		else
			S.title[0] = '\0';
		S.marquee_off = 0;
	} else if (!strcmp(pr->name, "time-pos")) {
		double t = pr->format == MPV_FORMAT_DOUBLE ? sane_time(*(double *)pr->data) : -1;

		int same = (int)t == (int)S.tpos;

		S.tpos = t;
		if (same)
			return;	/* only redraw on whole seconds */
	} else if (!strcmp(pr->name, "duration")) {
		S.dur = pr->format == MPV_FORMAT_DOUBLE ? sane_time(*(double *)pr->data) : -1;
	} else if (!strcmp(pr->name, "pause")) {
		S.paused = pr->format == MPV_FORMAT_FLAG && *(int *)pr->data;
	} else if (!strcmp(pr->name, "volume")) {
		if (pr->format == MPV_FORMAT_DOUBLE)
			S.volume = *(double *)pr->data;
	} else if (!strcmp(pr->name, "seekable")) {
		S.seekable = pr->format == MPV_FORMAT_FLAG && *(int *)pr->data;
	} else if (!strcmp(pr->name, "vo-configured")) {
		S.vo_ok = pr->format == MPV_FORMAT_FLAG && *(int *)pr->data;
		/* switching to audio-only: don't leave the last picture behind */
		if (!S.vo_ok)
			memset(S.p.frame, 0, sizeof(S.p.frame));
	}
	S.dirty = 1;
}

/* Print an mpv log line with control characters (terminal escapes from
 * remote content) replaced. */
static void log_safe(const char *prefix, const char *text)
{
	char buf[1024];
	size_t n = 0;

	for (; *text && n < sizeof(buf) - 1; text++) {
		unsigned char ch = (unsigned char)*text;

		buf[n++] = (ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7f ? '?' : (char)ch;
	}
	buf[n] = '\0';
	fprintf(stderr, "wmtube: [%s] %s", prefix, buf);
}

static void handle_mpv_events(void)
{
	char msg[300];

	for (;;) {
		mpv_event *e = mpv_wait_event(S.p.mpv, 0);

		switch (e->event_id) {
		case MPV_EVENT_NONE:
			return;
		case MPV_EVENT_SHUTDOWN:
			S.quit = 1;
			return;
		case MPV_EVENT_PROPERTY_CHANGE:
			handle_property(e->data);
			break;
		case MPV_EVENT_START_FILE:
			S.ytdl_err[0] = '\0';
			set_status("LOADING");
			break;
		case MPV_EVENT_FILE_LOADED:
			S.have_video = 1;
			S.loading = 0;
			set_status("");
			break;
		case MPV_EVENT_END_FILE: {
			mpv_event_end_file *ef = e->data;

			if (ef->reason == MPV_END_FILE_REASON_EOF) {
				unload();	/* played to the end (never with -loop) */
			} else if (ef->reason == MPV_END_FILE_REASON_ERROR) {
				S.have_video = 0;
				S.loading = 0;
				if (S.ytdl_err[0])
					snprintf(msg, sizeof(msg), "ERROR: %s", S.ytdl_err);
				else
					snprintf(msg, sizeof(msg), "ERROR: %s",
					         mpv_error_string(ef->error));
				set_status(msg);
			}
			break;
		}
		case MPV_EVENT_LOG_MESSAGE: {
			mpv_event_log_message *lm = e->data;

			log_safe(lm->prefix, lm->text);
			/* "ERROR: [youtube] <id>: This video is unavailable" */
			if (!strcmp(lm->prefix, "ytdl_hook") &&
			    !strncmp(lm->text, "ERROR: ", 7)) {
				const char *r = strrchr(lm->text, ':');

				snprintf(S.ytdl_err, sizeof(S.ytdl_err), "%s", r ? r + 1 : lm->text + 7);
			}
			break;
		}
		default:
			break;
		}
	}
}

/* ---- main ------------------------------------------------------------- */

static void usage(void)
{
	printf("wmtube " VERSION " - YouTube / PeerTube player dockapp for Window Maker\n\n"
	       "usage: wmtube [options] [URL]\n\n"
	       "  -display DISPLAY    X display to use\n"
	       "  -mode MODE          text rows to show: both, title, time, none (default both)\n"
	       "  -fg COLOR           text colour (default #20B2AE)\n"
	       "  -bg COLOR           background colour (default #202020)\n"
	       "  -mute               start muted\n"
	       "  -loop               loop the video\n"
	       "  -ytdl-format FMT    yt-dlp format (default: low resolution)\n"
	       "  -h, -help           show this help\n\n"
	       "mouse: left = pause (no video: toggle help), double-left = open in mpv,\n"
	       "       right = cycle text rows, shift+left = -10s, shift+right = +10s,\n"
	       "       ctrl+left = previous track, ctrl+right = next track,\n"
	       "       middle = play URL from selection,\n"
	       "       wheel = volume\n");
}

static void on_signal(int sig)
{
	(void)sig;
	S.quit = 1;
}

int main(int argc, char **argv)
{
	struct player_opts po = {
		/* prefer H.264: far cheaper to decode than AV1/VP9. YouTube has
		 * separate video/audio streams; PeerTube has combined ones. */
		.ytdl_format = "bestvideo[height<=144][vcodec^=avc1]+bestaudio/"
		               "bestvideo[height<=144]+bestaudio/"
		               "best[height<=144]/best[height<=240]/worst",
	};
	const char *dpyname = NULL, *fg = "#20B2AE", *bg = "#202020", *url = NULL;
	double next_tick;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		int more = i + 1 < argc;

		if (!strcmp(a, "-display") && more)
			dpyname = argv[++i];
		else if (!strcmp(a, "-mode") && more) {
			const char *m = argv[++i];
			int k;

			for (k = 0; k < MODE_COUNT && strcmp(m, mode_names[k]); k++)
				;
			if (k == MODE_COUNT) {
				fprintf(stderr, "wmtube: unknown mode '%s'\n", m);
				return 1;
			}
			S.mode = k;
		} else if (!strcmp(a, "-fg") && more)
			fg = argv[++i];
		else if (!strcmp(a, "-bg") && more)
			bg = argv[++i];
		else if (!strcmp(a, "-mute"))
			po.mute = 1;
		else if (!strcmp(a, "-loop"))
			po.loop = 1;
		else if (!strcmp(a, "-ytdl-format") && more)
			po.ytdl_format = argv[++i];
		else if (!strcmp(a, "-h") || !strcmp(a, "-help") || !strcmp(a, "--help")) {
			usage();
			return 0;
		} else if (a[0] != '-' && !url)
			url = a;
		else {
			fprintf(stderr, "wmtube: bad option '%s' (try -help)\n", a);
			return 1;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	if (display_open(&S.d, dpyname, argc, argv) < 0)
		return 1;
	if (display_parse_color(&S.d, fg, &S.fg) < 0 ||
	    display_parse_color(&S.d, bg, &S.bg) < 0) {
		fprintf(stderr, "wmtube: bad colour\n");
		return 1;
	}
	/* dimmed text (paused): halfway between fg and bg */
	S.dim = (((S.fg >> 1) & 0x7f7f7f) + ((S.bg >> 1) & 0x7f7f7f));

	if (player_init(&S.p, &po) < 0)
		return 1;
	S.tpos = S.dur = -1;
	S.show_help = 1;

	if (!in_path("yt-dlp") && !in_path("youtube-dl"))
		set_status("NO YT-DLP FOUND");
	else if (url)
		load(url);
	redraw();

	next_tick = now() + TICK_MS / 1000.0;
	while (!S.quit) {
		struct pollfd fds[2] = {
			{ ConnectionNumber(S.d.dpy), POLLIN, 0 },
			{ S.p.wake_pipe[0], POLLIN, 0 },
		};
		int timeout = (int)((next_tick - now()) * 1000);

		handle_x_events();
		if (S.dirty)
			redraw();
		if (timeout < 0)
			timeout = 0;
		if (poll(fds, 2, timeout) < 0 && errno != EINTR)
			break;

		if (fds[1].revents & POLLIN) {
			player_ack_wakeup(&S.p);
			handle_mpv_events();
			if (player_render(&S.p))
				S.dirty = 1;
		}
		if (now() >= next_tick) {
			next_tick += TICK_MS / 1000.0;
			if (next_tick < now())
				next_tick = now() + TICK_MS / 1000.0;
			if (marquee_active()) {
				S.marquee_off++;
				S.dirty = 1;
			}
			if (time_marquee_active()) {
				S.time_marquee_off++;
				S.dirty = 1;
			}
			/* expire volume readout */
			if (S.volume_until && now() >= S.volume_until) {
				S.volume_until = 0;
				S.dirty = 1;
			}
		}
	}

	player_destroy(&S.p);
	display_close(&S.d);
	return 0;
}
