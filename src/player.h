/* player.h - libmpv wrapper rendering into a small software buffer
 *
 * Copyright (C) 2026 Michael Sternberg
 * SPDX-License-Identifier: GPL-2.0-or-later
 * This file is part of wmtube; see COPYING for the full license text.
 */
#ifndef WMTUBE_PLAYER_H
#define WMTUBE_PLAYER_H

#include <stdint.h>
#include <mpv/client.h>
#include <mpv/render.h>

#define VIDEO_W 56
#define VIDEO_H 32

struct player_opts {
	const char *ytdl_format;
	int mute;
	int loop;
};

struct player {
	mpv_handle *mpv;
	mpv_render_context *rc;
	int wake_pipe[2];	/* written from mpv threads, read by main loop */
	uint32_t frame[VIDEO_W * VIDEO_H];	/* 0x00RRGGBB */
};

int player_init(struct player *p, const struct player_opts *o);
void player_destroy(struct player *p);

/* Drain the wakeup pipe; call when wake_pipe[0] is readable. */
void player_ack_wakeup(struct player *p);
/* Render a new frame into p->frame if mpv has one. Returns 1 if rendered. */
int player_render(struct player *p);

void player_load(struct player *p, const char *url);
void player_command(struct player *p, const char **args);
void player_toggle_pause(struct player *p);
void player_seek(struct player *p, double secs, const char *how);
void player_add_volume(struct player *p, double delta);

#endif
