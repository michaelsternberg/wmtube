/* player.c - libmpv wrapper rendering into a small software buffer
 *
 * Copyright (C) 2026 Michael Sternberg
 * SPDX-License-Identifier: GPL-2.0-or-later
 * This file is part of wmtube; see COPYING for the full license text.
 *
 * mpv runs with vo=libmpv and the software render API, so every frame is
 * scaled by mpv straight into our 56x32 "bgr0" buffer (which on a
 * little-endian machine is exactly a 0x00RRGGBB uint32_t array).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "player.h"

static void wakeup(void *ctx)
{
	struct player *p = ctx;
	char c = 1;

	/* non-blocking; if the pipe is full the main loop is awake anyway */
	if (write(p->wake_pipe[1], &c, 1) < 0 && errno != EAGAIN)
		perror("wmtube: wakeup pipe");
}

static void set_opt(mpv_handle *mpv, const char *name, const char *val)
{
	int err = mpv_set_option_string(mpv, name, val);

	if (err < 0)
		fprintf(stderr, "wmtube: mpv option %s=%s: %s\n", name, val, mpv_error_string(err));
}

int player_init(struct player *p, const struct player_opts *o)
{
	int err, i;

	memset(p, 0, sizeof(*p));
	if (pipe(p->wake_pipe) < 0) {
		perror("wmtube: pipe");
		return -1;
	}
	for (i = 0; i < 2; i++) {
		fcntl(p->wake_pipe[i], F_SETFL, O_NONBLOCK);
		fcntl(p->wake_pipe[i], F_SETFD, FD_CLOEXEC);
	}

	p->mpv = mpv_create();
	if (!p->mpv) {
		fprintf(stderr, "wmtube: mpv_create failed\n");
		return -1;
	}
	set_opt(p->mpv, "vo", "libmpv");
	set_opt(p->mpv, "idle", "yes");
	set_opt(p->mpv, "input-default-bindings", "no");
	set_opt(p->mpv, "input-vo-keyboard", "no");
	set_opt(p->mpv, "osc", "no");
	set_opt(p->mpv, "ytdl", "yes");
	set_opt(p->mpv, "ytdl-format", o->ytdl_format);
	set_opt(p->mpv, "audio-client-name", "wmtube");
	set_opt(p->mpv, "background-color", "#000000");
	if (o->mute)
		set_opt(p->mpv, "mute", "yes");
	if (o->loop)
		set_opt(p->mpv, "loop-file", "inf");

	err = mpv_initialize(p->mpv);
	if (err < 0) {
		fprintf(stderr, "wmtube: mpv_initialize: %s\n", mpv_error_string(err));
		return -1;
	}
	mpv_request_log_messages(p->mpv, "error");

	{
		mpv_render_param params[] = {
			{ MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW },
			{ MPV_RENDER_PARAM_INVALID, NULL },
		};
		err = mpv_render_context_create(&p->rc, p->mpv, params);
		if (err < 0) {
			fprintf(stderr, "wmtube: mpv render context: %s\n", mpv_error_string(err));
			return -1;
		}
	}

	mpv_observe_property(p->mpv, 0, "media-title", MPV_FORMAT_STRING);
	mpv_observe_property(p->mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
	mpv_observe_property(p->mpv, 0, "duration", MPV_FORMAT_DOUBLE);
	mpv_observe_property(p->mpv, 0, "pause", MPV_FORMAT_FLAG);
	mpv_observe_property(p->mpv, 0, "volume", MPV_FORMAT_DOUBLE);
	mpv_observe_property(p->mpv, 0, "vo-configured", MPV_FORMAT_FLAG);
	mpv_observe_property(p->mpv, 0, "seekable", MPV_FORMAT_FLAG);
	mpv_observe_property(p->mpv, 0, "playlist-pos-1", MPV_FORMAT_INT64);
	mpv_observe_property(p->mpv, 0, "playlist-count", MPV_FORMAT_INT64);

	mpv_set_wakeup_callback(p->mpv, wakeup, p);
	mpv_render_context_set_update_callback(p->rc, wakeup, p);
	return 0;
}

void player_destroy(struct player *p)
{
	if (p->rc)
		mpv_render_context_free(p->rc);
	if (p->mpv)
		mpv_terminate_destroy(p->mpv);
	p->rc = NULL;
	p->mpv = NULL;
}

void player_ack_wakeup(struct player *p)
{
	char buf[64];

	while (read(p->wake_pipe[0], buf, sizeof(buf)) > 0)
		;
}

int player_render(struct player *p)
{
	int size[2] = { VIDEO_W, VIDEO_H };
	size_t stride = VIDEO_W * 4;
	mpv_render_param params[] = {
		{ MPV_RENDER_PARAM_SW_SIZE, size },
		{ MPV_RENDER_PARAM_SW_FORMAT, "bgr0" },
		{ MPV_RENDER_PARAM_SW_STRIDE, &stride },
		{ MPV_RENDER_PARAM_SW_POINTER, p->frame },
		{ MPV_RENDER_PARAM_INVALID, NULL },
	};

	if (!(mpv_render_context_update(p->rc) & MPV_RENDER_UPDATE_FRAME))
		return 0;
	return mpv_render_context_render(p->rc, params) >= 0;
}

void player_command(struct player *p, const char **args)
{
	int err = mpv_command_async(p->mpv, 0, args);

	if (err < 0)
		fprintf(stderr, "wmtube: mpv command %s: %s\n", args[0], mpv_error_string(err));
}

void player_load(struct player *p, const char *url)
{
	const char *cmd[] = { "loadfile", url, "replace", NULL };

	player_command(p, cmd);
	mpv_set_property_string(p->mpv, "pause", "no");
}

/* Stop playback and clear the playlist; mpv goes idle. */
void player_stop(struct player *p)
{
	const char *cmd[] = { "stop", NULL };

	player_command(p, cmd);
}

void player_toggle_pause(struct player *p)
{
	const char *cmd[] = { "cycle", "pause", NULL };

	player_command(p, cmd);
}

void player_seek(struct player *p, double secs, const char *how)
{
	char s[32];
	const char *cmd[] = { "seek", s, how, NULL };

	snprintf(s, sizeof(s), "%.3f", secs);
	player_command(p, cmd);
}

/* Move through a playlist: dir < 0 previous, dir > 0 next. Does nothing
 * at either end (mpv's default "weak" behaviour) or for a single video. */
void player_playlist_step(struct player *p, int dir)
{
	const char *cmd[] = { dir < 0 ? "playlist-prev" : "playlist-next", NULL };

	player_command(p, cmd);
}

void player_add_volume(struct player *p, double delta)
{
	char s[32];
	const char *cmd[] = { "add", "volume", s, NULL };

	snprintf(s, sizeof(s), "%g", delta);
	player_command(p, cmd);
}
