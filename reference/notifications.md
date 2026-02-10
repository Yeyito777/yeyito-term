# Notification Overlay System

The notification system allows external programs to display popup messages on an st terminal window. The mechanism uses X11 window properties: an external process sets the `_ST_NOTIFY` property on the st window, st detects the change via `PropertyNotify`, renders a styled overlay popup in the top-right corner, and auto-dismisses it after a configurable timeout (default 5 seconds).

This is a purely external interface — unlike the SSH indicator (which uses OSC escape sequences from the shell), notifications are triggered by setting an X11 property from any process that knows the window ID. No shell coordination or OSC sequences are involved.

## Usage

### Via the `st-notify` script

```bash
st-notify <st-pid> "Your message here"
```

The script finds the X window belonging to the given PID using `xdotool search --pid`, then sets the `_ST_NOTIFY` property using `xprop`. Requires `xdotool` and `xprop` to be installed.

### Via `xprop` directly

```bash
xprop -id <window-id> -f _ST_NOTIFY 8u -set _ST_NOTIFY "Your message here"
```

### Via any X11 client

Any program with access to the display can call `XChangeProperty()` to set `_ST_NOTIFY` on the st window. st reads and deletes the property atomically (the `XGetWindowProperty` call uses `delete=True`), so there is no stale state.

## Architecture

### Property-based trigger (not OSC)

The notification system does **not** use OSC escape sequences. It uses an X11 window property (`_ST_NOTIFY`) which is set externally and detected via the `PropertyNotify` X event. This design means:

- Any process can send a notification, not just the shell running inside st
- No terminal I/O or pseudo-terminal involvement
- The sender only needs the window ID (discoverable via `_NET_WM_PID` + `xdotool`)
- The property is deleted after reading, preventing stale state

### Overlay rendering

The notification popup is a child X window of the main st window, using double-buffered Xft rendering. It creates its own `GC`, `XftDraw`, `Pixmap`, and `XftFont` — completely independent of the main terminal's drawing context. This isolation means the notification overlay cannot interfere with terminal rendering and vice versa.

### Positioning

The popup appears in the top-right corner of the terminal, offset by `notif_margin` (10px) from the edges. If the SSH indicator (`sshind`) is active, the notification shifts down below it by `sshind_height() + notif_margin` pixels to avoid overlap.

### Auto-dismiss timer

On show, `clock_gettime(CLOCK_MONOTONIC)` records the timestamp. The main event loop in `run()` checks `notif_check_timeout()` each iteration. When the elapsed time exceeds `notif_display_ms` (5000ms), `notif_hide()` is called and all X resources are freed. The timeout also participates in the `pselect()` timeout calculation so st wakes up precisely when dismissal is due, rather than polling.

### Replacement behavior

If a new notification arrives while one is already displayed, `notif_show()` calls `notif_hide()` first to tear down the existing overlay (destroying the window, pixmap, font, GC, and colors), then creates a fresh overlay for the new message. The timeout resets to the full `notif_display_ms` from the new show time.

## Configuration

All configuration is in `notif.h` as `static const` variables (same pattern as `sshind.h`):

| Variable | Default | Description |
|----------|---------|-------------|
| `notif_border_color` | `"#5fafd7"` | Border color (steel blue) |
| `notif_bg_color` | `"#001520"` | Background color (very dark blue) |
| `notif_fg_color` | `"#ffffff"` | Text color (white) |
| `notif_border_width` | `2` | Border thickness in pixels |
| `notif_margin` | `10` | Margin from parent window edge in pixels |
| `notif_padding` | `8` | Internal padding between border and text in pixels |
| `notif_font_scale` | `1.5` | Font size multiplier relative to `usedfontsize` |
| `notif_display_ms` | `5000` | Auto-dismiss timeout in milliseconds |

## Debug Mode

When st is started with `-d` (debug mode), the notification system logs to stderr:

```
notif: showing "Your message here"
notif: hiding "Your message here"
```

This is useful for verifying that notifications are being received and dismissed correctly without needing to see the visual overlay.

## Relevant Files and Functions

### notif.h

| Item | Line | Description |
|------|------|-------------|
| `TIMEDIFF` macro | 10 | Local definition (guarded by `#ifndef`) for use in `notif_check_timeout()` without requiring `st.h` |
| Configuration statics | 18-25 | All visual and timing parameters |
| `notif_show()` | 28 | Show a notification with the given message |
| `notif_hide()` | 29 | Dismiss the current notification and free all X resources |
| `notif_draw()` | 30 | Redraw the notification overlay (called on Expose events) |
| `notif_resize()` | 31 | Reposition the overlay after terminal resize |
| `notif_active()` | 32 | Returns 1 if a notification is currently displayed |
| `notif_check_timeout()` | 33 | Returns remaining ms until dismissal (negative/zero = expired) |

### notif.c

| Function | Line | Description |
|----------|------|-------------|
| `notif_active()` | 83 | Returns `notif.active` flag |
| `notif_check_timeout()` | 89 | Computes `notif_display_ms - elapsed` using `TIMEDIFF`. Returns remaining ms, or -1 if inactive |
| `notif_show()` | 101 | Full lifecycle: hide existing if active, store message, load scaled font via Fontconfig/Xft, measure text, allocate colors, compute position (offset below sshind if active), create child window with `CWColormap`, create pixmap + XftDraw + GC, map window, record show time, draw |
| `notif_hide()` | 211 | Destroys all X resources in order: XftDraw, Pixmap, Window, XftFont, GC, then frees XftColors. Clears active flag and message |
| `notif_draw()` | 251 | Clears background with `XftDrawRect`, draws text with `XftDrawStringUtf8`, copies pixmap to window with `XCopyArea` using the notification's own GC |
| `notif_resize()` | 274 | Recalculates top-right position (accounting for sshind), calls `XMoveWindow` |
| Static state struct | 69-80 | `active`, `msg[512]`, `win`, `buf`, `draw`, `font`, `fg/bg/border`, `width/height`, `gc`, `show_time` |

### x.c

| Location | Line | Description |
|----------|------|-------------|
| `#include "notif.h"` | 67 | Header inclusion, after `sshind.h` |
| `xw.stnotify` atom in `XWindow` struct | 100 | Atom field for the `_ST_NOTIFY` property |
| `PropertyChangeMask` in event mask | 1188 | Always-on in `xinit()`, enables `PropertyNotify` events for both clipboard INCR transfers and `_ST_NOTIFY` detection |
| `xw.stnotify` atom registration in `xinit()` | 1251 | `XInternAtom(xw.dpy, "_ST_NOTIFY", False)` |
| `propnotify()` handler | 523-534 | On `PropertyNewValue` for `xw.stnotify`: calls `XGetWindowProperty` with `delete=True` to atomically read and remove the property, then calls `notif_show()` with the data. Reads up to 256 long-words (1024 bytes) as `UTF8_STRING` |
| `expose()` hook | 1796 | Calls `notif_draw()` after `sshind_draw()` |
| `resize()` hook | 2112 | Calls `notif_resize()` after `sshind_resize()` |
| Timer logic in `run()` | 2212-2220 | After the blink timeout block: checks `notif_active()`, calls `notif_check_timeout(&now)`. If expired, calls `notif_hide()`. If not expired, adjusts `pselect()` timeout so the loop wakes up at exactly the right time for dismissal |
| INCR transfer cleanup | 566-572 | `PropertyChangeMask` is no longer toggled off after INCR clipboard transfers complete, since it must stay on for `_ST_NOTIFY` monitoring |

### st.h

| Item | Line | Description |
|------|------|-------------|
| `notif_show()` | 134 | Public declaration |
| `notif_hide()` | 135 | Public declaration |
| `notif_draw()` | 136 | Public declaration |
| `notif_resize()` | 137 | Public declaration |
| `notif_active()` | 138 | Public declaration |
| `notif_check_timeout()` | 139 | Public declaration |

### sshind.h / sshind.c

| Item | File | Line | Description |
|------|------|------|-------------|
| `sshind_height()` | sshind.h | 25 | Declaration added for notif.c to query sshind overlay height |
| `sshind_height()` | sshind.c | 84 | Returns `sshind.height + 2 * sshind_border_width` if active, 0 otherwise |

### scripts/st-notify

| Line | Description |
|------|-------------|
| 5-6 | Parses `pid` and `msg` arguments |
| 8-11 | Usage error if either argument missing |
| 14 | `xdotool search --pid "$pid"` to find the X window ID for the given PID |
| 22 | `xprop -id "$wid" -f _ST_NOTIFY 8u -set _ST_NOTIFY "$msg"` to set the property as a UTF-8 string |

### Makefile

| Target | Description |
|--------|-------------|
| `notif.o` | Compiled from `notif.c`, depends on `sshind.h notif.h` |
| `x.o` | Now also depends on `notif.h` |
| `test_notif` | Compiles `tests/test_notif.c` (self-contained, includes `notif.c` directly with X11 mocks) |
| `install` | Copies `scripts/st-notify` to `$(DESTDIR)$(PREFIX)/bin/` alongside `st` |

### tests/test_notif.c

| Test | Line | Description |
|------|------|-------------|
| `notif_active_initial_state` | 331 | Inactive by default on startup |
| `notif_show_activates` | 340 | `notif_active()` returns 1 after `notif_show()` |
| `notif_hide_deactivates` | 352 | `notif_active()` returns 0 after `notif_hide()` |
| `notif_creates_own_gc` | 364 | `XCreateGC` is called exactly once during `notif_show()` |
| `notif_draw_uses_own_gc` | 377 | `XCopyArea` uses the notification's GC, not `dc.gc` |
| `notif_hide_frees_gc` | 399 | `XFreeGC` is called during `notif_hide()` |
| `notif_check_timeout_not_expired` | 413 | Returns positive remaining time when show_time is 1 second ago |
| `notif_check_timeout_expired` | 435 | Returns zero or negative when show_time is 6 seconds ago |
| `notif_replaces_existing` | 456 | Calling `notif_show()` twice frees old GC and creates new one |

## Flow

### External program sends notification

```
External process (e.g. build script, timer, monitoring tool)
  → Finds st's window ID via xdotool search --pid or _NET_WM_PID
  → Calls xprop to set _ST_NOTIFY property on the window
    (or XChangeProperty directly from C/Python/etc.)

st's X event loop (run() in x.c)
  → XNextEvent delivers PropertyNotify
  → handler[PropertyNotify] → propnotify()
  → Checks xpev->atom == xw.stnotify && xpev->state == PropertyNewValue
  → XGetWindowProperty with delete=True reads data, removes property
  → Calls notif_show(data)
    → Tears down any existing notification
    → Loads scaled font, measures text, allocates colors
    → Creates child window (XCreateWindow with CWColormap)
    → Creates pixmap, XftDraw, GC for double buffering
    → XMapRaised to show the overlay
    → clock_gettime records show_time
    → notif_draw() renders text to pixmap, copies to window
  → XFree(data)
```

### Timer-based auto-dismiss

```
run() main loop iteration:
  → clock_gettime(CLOCK_MONOTONIC, &now)
  → Process X events, tty reads
  → After blink timeout block:
  → notif_active() returns 1
  → notif_check_timeout(&now) computes remaining ms
  → If remaining <= 0:
      → notif_hide() destroys all X resources
  → If remaining > 0 and remaining < current timeout:
      → timeout = remaining (ensures pselect wakes at dismissal time)
  → draw(), XFlush
```

### Window resize while notification is visible

```
ConfigureNotify event
  → resize() in x.c
  → cresize() updates win.w, win.h
  → sshind_resize() repositions SSH indicator
  → notif_resize()
    → Recalculates x = win.w - notif.width - notif_margin
    → Recalculates y = notif_margin (+ sshind offset if active)
    → XMoveWindow repositions the overlay
```

### Expose event redraws overlay

```
Expose event
  → expose() in x.c
  → redraw() redraws terminal content
  → sshind_draw() redraws SSH indicator
  → notif_draw()
    → XftDrawRect clears background
    → XftDrawStringUtf8 renders message text
    → XCopyArea copies pixmap to overlay window
```

## X11 Resource Lifecycle

Each `notif_show()` creates these X resources, all destroyed by `notif_hide()`:

| Resource | Created by | Destroyed by |
|----------|-----------|--------------|
| `XftFont` | `XftFontOpenPattern` | `XftFontClose` |
| `XftColor` (x3: fg, bg, border) | `XftColorAllocName` | `XftColorFree` |
| Child `Window` | `XCreateWindow` | `XDestroyWindow` |
| `Pixmap` (double buffer) | `XCreatePixmap` | `XFreePixmap` |
| `XftDraw` | `XftDrawCreate` | `XftDrawDestroy` |
| `GC` | `XCreateGC` | `XFreeGC` |

The child window is created with `override_redirect = True` so the window manager does not attempt to manage, decorate, or reposition it. The `CWColormap` flag is passed explicitly to avoid `BadMatch` errors when the parent window's colormap differs from the default.

## Struct Layout Constraint

Both `notif.c` and `sshind.c` contain their own local `XWindow` struct definition (a subset of the real one in `x.c`) to avoid pulling in all of `x.c`'s includes. **These struct definitions must match the field layout in `x.c` exactly**, particularly the `Atom` fields. If atoms are added to the `XWindow` struct in `x.c`, the local definitions in `notif.c` and `sshind.c` must be updated to match, or all fields after the atoms will be read at incorrect offsets (causing garbage values for `vis`, `scr`, `cmap`, etc., and likely `BadMatch` or `BadValue` X errors).
