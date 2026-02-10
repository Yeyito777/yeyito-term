# Notification Overlay System

The notification system allows external programs to display popup messages on an st terminal window. It uses a **stacked toast** design: each new notification appears at the top-right corner, pushing existing toasts downward. Each toast auto-dismisses independently after a configurable timeout (default 5 seconds). The mechanism uses X11 window properties: an external process sets the `_ST_NOTIFY` property on the st window, st detects the change via `PropertyNotify`, renders a styled overlay popup, and auto-dismisses it after the timeout.

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

### Multi-line notifications

Messages containing newline characters are rendered as multi-line toasts. The toast width adapts to the widest line and the height adapts to the number of lines.

```bash
xprop -id <window-id> -f _ST_NOTIFY 8u -set _ST_NOTIFY "Line one
Line two
Line three"
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

### Stacked toast design

New notifications do **not** replace the current one. Instead:

- New toast appears at the top-right corner (index 0 in the stack)
- Existing toasts are pushed down by the height of the new toast plus `notif_toast_gap`
- Each toast has its own independent timer starting from when it was shown
- When any toast expires, it is removed and toasts below it shift upward
- Maximum `NOTIF_MAX_TOASTS` (8) toasts can be stacked simultaneously
- If a new notification arrives at max capacity, the oldest toast is evicted

### Shared resources

The font and colors are loaded once when the first toast appears and freed when the last toast disappears. Each individual toast has its own X window, pixmap, XftDraw, and GC — completely independent of the main terminal's drawing context and of other toasts.

### Multi-line rendering

Each notification message is split on newline characters (`\n`). The toast dimensions adapt:
- **Width**: based on the widest line (plus padding and border)
- **Height**: based on the number of lines times the line height (plus padding and border)

Line data is stored as offsets into the message buffer (not pointers), making the toast structs safe to copy during array shifts.

### Positioning

Toasts stack vertically in the top-right corner. The Y position of each toast is calculated as:
- `notif_margin` (10px) from the top edge
- Plus `sshind_height() + notif_margin` if the SSH indicator is active
- Plus the cumulative height of all toasts above it (each toast's height + 2 * border_width + toast_gap)

### Auto-dismiss timer

On show, `clock_gettime(CLOCK_MONOTONIC)` records the timestamp for each toast. The main event loop in `run()` checks `notif_check_timeout()` each iteration. This function iterates all active toasts, removes any that have exceeded `notif_display_ms`, and returns the minimum remaining time across all toasts. The timeout participates in the `pselect()` calculation so st wakes up precisely when the next dismissal is due.

## Configuration

All configuration is in `notif.h` as `static const` variables (same pattern as `sshind.h`):

| Variable | Default | Description |
|----------|---------|-------------|
| `notif_border_color` | `"#00ff88"` | Border color (bright green) |
| `notif_bg_color` | `"#000f08"` | Background color (dark green, blends with #00050f) |
| `notif_fg_color` | `"#ffffff"` | Text color (white) |
| `notif_border_width` | `2` | Border thickness in pixels |
| `notif_margin` | `10` | Margin from parent window edge in pixels |
| `notif_padding` | `8` | Internal padding between border and text in pixels |
| `notif_font_scale` | `1.5` | Font size multiplier relative to `usedfontsize` |
| `notif_display_ms` | `5000` | Auto-dismiss timeout in milliseconds |
| `notif_toast_gap` | `6` | Vertical gap between stacked toasts in pixels |
| `NOTIF_MAX_TOASTS` | `8` | Maximum simultaneous toasts (define) |
| `NOTIF_MAX_LINES` | `16` | Maximum lines per toast message (define) |

## Debug Mode

When st is started with `-d` (debug mode), the notification system logs detailed state to stderr:

```
notif: [toast 0] showing "Your message" (254x49, 1 lines, stack: 2)
notif: --- stack dump (after show) ---
notif:   count=2, shared_loaded=1
notif:   [0] "Your message" (254x49, 1 lines, 4999ms remaining, y=10)
notif:   [1] "Previous message" (280x49, 1 lines, 3500ms remaining, y=69)
notif: --- end dump ---
notif: [toast 1] hiding "Previous message" (lived 5019ms, stack: 2 -> 1)
```

This shows:
- Per-toast show/hide events with dimensions, line count, and stack size
- Full stack dumps after each new toast showing all positions and remaining timeouts
- Toast lifetime when hiding (how long it was displayed)

## Relevant Files and Functions

### notif.h

| Item | Description |
|------|-------------|
| `TIMEDIFF` macro | Local definition (guarded by `#ifndef`) for timeout calculation |
| Configuration statics | All visual and timing parameters |
| `NOTIF_MAX_TOASTS` | Maximum simultaneous toasts (8) |
| `NOTIF_MAX_LINES` | Maximum lines per toast message (16) |
| `notif_show()` | Add a new toast with the given message |
| `notif_hide()` | Dismiss all toasts and free all resources |
| `notif_draw()` | Redraw all toast overlays (called on Expose events) |
| `notif_resize()` | Reposition all toasts after terminal resize |
| `notif_active()` | Returns 1 if any toast is currently displayed |
| `notif_check_timeout()` | Removes expired toasts, returns min remaining ms |

### notif.c

| Item | Description |
|------|-------------|
| `NotifToast` typedef | Per-toast state: active, msg, line offsets/lengths, X resources, dimensions, show_time |
| `notif` static struct | Stack state: array of toasts, count, shared font and colors |
| `notif_toast_y()` | Calculates Y position for toast at given stack index |
| `notif_parse_lines()` | Splits message on newlines, stores offsets and lengths |
| `notif_load_shared()` | Loads font and colors on first toast |
| `notif_free_shared()` | Frees font and colors when last toast dismissed |
| `notif_destroy_toast()` | Destroys per-toast X resources (window, pixmap, draw, GC) |
| `notif_draw_toast()` | Renders a single toast: clears bg, draws each line, copies to window |
| `notif_remove_toast()` | Destroys toast at index, shifts array, repositions remaining |
| `notif_debug_dump()` | Logs full stack state (positions, remaining times) in debug mode |
| `notif_active()` | Returns `notif.count > 0` |
| `notif_check_timeout()` | Iterates from end to start, removes expired, returns min remaining |
| `notif_show()` | Loads shared resources, evicts oldest if full, shifts stack, creates new toast at index 0, repositions existing toasts |
| `notif_hide()` | Destroys all toasts, frees shared resources |
| `notif_draw()` | Iterates all toasts, calls `notif_draw_toast()` |
| `notif_resize()` | Repositions all toasts based on new window dimensions |

### x.c

| Location | Description |
|----------|-------------|
| `propnotify()` | On `PropertyNewValue` for `xw.stnotify`: reads and deletes property, calls `notif_show()` |
| `expose()` | Calls `notif_draw()` after `sshind_draw()` |
| `resize()` | Calls `notif_resize()` after `sshind_resize()` |
| Timer logic in `run()` | Checks `notif_active()`, calls `notif_check_timeout()` which handles removal internally, adjusts `pselect()` timeout for next dismissal |

### scripts/st-notify

| Line | Description |
|------|-------------|
| 5-6 | Parses `pid` and `msg` arguments |
| 8-11 | Usage error if either argument missing |
| 14 | `xdotool search --pid "$pid"` to find the X window ID for the given PID |
| 22 | `xprop -id "$wid" -f _ST_NOTIFY 8u -set _ST_NOTIFY "$msg"` to set the property as a UTF-8 string |

### tests/test_notif.c

| Test | Description |
|------|-------------|
| `notif_active_initial_state` | Inactive by default on startup |
| `notif_show_activates` | `notif_active()` returns 1 after `notif_show()` |
| `notif_hide_deactivates` | `notif_active()` returns 0 after `notif_hide()` |
| `notif_creates_own_gc` | `XCreateGC` called once per toast |
| `notif_draw_uses_own_gc` | `XCopyArea` uses toast's GC, not `dc.gc` |
| `notif_hide_frees_gc` | `XFreeGC` called during `notif_hide()` |
| `notif_check_timeout_not_expired` | Returns positive remaining time for recent toast |
| `notif_check_timeout_removes_expired` | Removes expired toast, returns -1 when empty |
| `notif_stacks_multiple` | Two shows create two toasts, newest at index 0 |
| `notif_shared_font_loaded_once` | Font loaded once for multiple toasts, freed once on hide |
| `notif_max_evicts_oldest` | At max capacity, oldest toast evicted |
| `notif_multiline_height` | Multi-line message gets correct line count and height |
| `notif_expired_middle_reposition` | Middle toast expired, remaining repositioned |

## Flow

### External program sends notification

```
External process
  → Sets _ST_NOTIFY property on st window

st's X event loop (run() in x.c)
  → PropertyNotify for xw.stnotify
  → propnotify() reads and deletes property
  → notif_show(msg)
    → notif_load_shared() loads font/colors if first toast
    → If at max capacity, notif_remove_toast(count-1) evicts oldest
    → Shifts existing toasts down by one index
    → Parses message into lines (splits on '\n')
    → Measures widest line, calculates toast dimensions
    → Creates X window, pixmap, XftDraw, GC at top-right position
    → Records show_time, marks active
    → Repositions all existing toasts (pushed down)
    → Draws the new toast
    → Debug: logs show event and full stack dump
```

### Timer-based auto-dismiss

```
run() main loop iteration:
  → clock_gettime(CLOCK_MONOTONIC, &now)
  → notif_active() returns 1 (count > 0)
  → notif_check_timeout(&now):
    → Iterates toasts from end to start
    → For each: computes remaining = notif_display_ms - elapsed
    → If remaining <= 0: notif_remove_toast(i)
      → Destroys toast's X resources
      → Shifts array, repositions remaining toasts
      → If count becomes 0: notif_free_shared()
    → Returns minimum remaining time across active toasts
  → If remaining > 0: adjusts pselect() timeout
```

### Window resize while toasts are visible

```
ConfigureNotify event
  → resize() in x.c
  → cresize() updates win.w, win.h
  → sshind_resize()
  → notif_resize()
    → For each toast: recalculates x (right-aligned) and y (stacked)
    → XMoveWindow repositions each toast
```

## X11 Resource Lifecycle

### Shared resources (loaded once, freed when last toast dismissed)

| Resource | Created by | Destroyed by |
|----------|-----------|--------------|
| `XftFont` | `XftFontOpenPattern` in `notif_load_shared()` | `XftFontClose` in `notif_free_shared()` |
| `XftColor` (x3: fg, bg, border) | `XftColorAllocName` in `notif_load_shared()` | `XftColorFree` in `notif_free_shared()` |

### Per-toast resources (created per show, destroyed per dismiss)

| Resource | Created by | Destroyed by |
|----------|-----------|--------------|
| Child `Window` | `XCreateWindow` in `notif_show()` | `XDestroyWindow` in `notif_destroy_toast()` |
| `Pixmap` (double buffer) | `XCreatePixmap` in `notif_show()` | `XFreePixmap` in `notif_destroy_toast()` |
| `XftDraw` | `XftDrawCreate` in `notif_show()` | `XftDrawDestroy` in `notif_destroy_toast()` |
| `GC` | `XCreateGC` in `notif_show()` | `XFreeGC` in `notif_destroy_toast()` |

Each toast window is created with `override_redirect = True` so the window manager does not manage it. The `CWColormap` flag is passed explicitly to avoid `BadMatch` errors.

## Struct Layout Constraint

Both `notif.c` and `sshind.c` contain their own local `XWindow` struct definition (a subset of the real one in `x.c`) to avoid pulling in all of `x.c`'s includes. **These struct definitions must match the field layout in `x.c` exactly**, particularly the `Atom` fields. If atoms are added to the `XWindow` struct in `x.c`, the local definitions in `notif.c` and `sshind.c` must be updated to match, or all fields after the atoms will be read at incorrect offsets (causing garbage values for `vis`, `scr`, `cmap`, etc., and likely `BadMatch` or `BadValue` X errors).
