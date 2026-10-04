# Changelog

## Unreleased

- Clear the video area when switching to an audio-only stream, so the
  previous video's last frame no longer stays on screen.
- Ctrl+left / Ctrl+right: previous / next track in a playlist.
- Live streams and internet radio show elapsed time only (mpv's
  "duration" for them is just elapsed time plus the buffer).

## 0.1 (2026-10-03)

- Renamed from wmyt to wmtube: neutral name now that PeerTube is
  supported, and no YouTube abbreviation in the program name.
- First version: plays YouTube and PeerTube (via libmpv + yt-dlp) in a
  64x64 Window Maker dockapp, with a scrolling title and play time in the
  wmtop LED style.
- Display modes (both / title / time / none) with a fixed video position.
- Play time scrolls as a marquee for videos of an hour or more.
- Scrolling help text when no video is loaded; left click toggles it.
- Mouse: left pause, double-left open in mpv, right cycle text rows,
  shift+left / shift+right seek -10 s / +10 s, middle play URL from
  selection, wheel volume.
- When a video ends the tile returns to its idle state (blank video,
  help text shown or hidden as last chosen).
- Prefers low-resolution streams (144p, H.264 when available).
- Hardened URL handling: selection limited to http(s) URLs, no option
  injection into the external mpv, sanitised log output.
