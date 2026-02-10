# st-notify

Send popup notifications to an st terminal window.

## Synopsis

```bash
st-notify [options] <st-pid> "message"
```

## Requirements

- `xdotool`
- `xprop`

## Options

| Option | Short | Argument | Description |
|--------|-------|----------|-------------|
| `--timeout` | `-t` | `<ms>` | Auto-dismiss timeout in milliseconds (default: 5000) |
| `--border` | `-b` | `<hex>` | Border color, e.g. `"#ff0000"` |
| `--background` | `-bg` | `<hex>` | Background color |
| `--foreground` | `-fg` | `<hex>` | Foreground/text color |
| `--textsize` | `-ts` | `<int>` | Font pixel size (window auto-fits to text) |
| `--help` | `-h` | | Show usage help |

## Defaults

When no options are specified, the toast uses the compile-time defaults from `notif.h`:

| Property | Default |
|----------|---------|
| Timeout | 5000ms |
| Border color | `#1d9bf0` (blue) |
| Background | `#00050f` (terminal background) |
| Foreground | `#ffffff` (white) |
| Text size | `usedfontsize * 1.5` (font scale) |

## Examples

### Basic notification

```bash
st-notify $$ "Hello world"
```

### Custom timeout

```bash
# 10-second timeout
st-notify -t 10000 $$ "This stays for 10 seconds"

# Quick 1-second flash
st-notify -t 1000 $$ "Gone!"
```

### Custom colors

```bash
# Red alert
st-notify -b "#ff0000" -bg "#1a0000" -fg "#ff4444" $$ "Error!"

# Green success
st-notify -b "#00ff00" -bg "#001a00" -fg "#88ff88" $$ "Build passed"

# Only override border, keep other defaults
st-notify -b "#ffaa00" $$ "Warning"
```

### Custom text size

```bash
# Large text (28px) — window auto-sizes to fit
st-notify -ts 28 $$ "Big notification"

# Small text (10px)
st-notify -ts 10 $$ "Subtle message"
```

### Combining options

```bash
st-notify -t 10000 -b "#ff0000" -bg "#1a0a2e" -fg "#00ff88" -ts 28 $$ "All options"
```

### Multi-line messages

```bash
st-notify $$ "Line one
Line two
Line three"
```

The toast width adapts to the widest line, height adapts to the number of lines.

### Combining multi-line with options

```bash
st-notify -t 8000 -b "#ff4444" -ts 20 $$ "Build failed
src/main.c:42: error
src/util.c:17: warning"
```

## Targeting a specific terminal

`st-notify` uses the PID to find the X window via `xdotool search --pid`. Common patterns:

```bash
# Current shell's terminal
st-notify $$ "message"

# A known PID
st-notify 12345 "message"

# From a script that spawned st
st &
st_pid=$!
st-notify $st_pid "Ready"
```

## Using from other programs

### Direct xprop (no options)

```bash
wid=$(xdotool search --pid "$pid" | head -1)
xprop -id "$wid" -f _ST_NOTIFY 8u -set _ST_NOTIFY "message"
```

### Direct xprop with options

Options are encoded as a metadata header using ASCII control characters:
- `\x1f` (Unit Separator) separates key=value pairs
- `\x1e` (Record Separator) separates metadata from message

```bash
wid=$(xdotool search --pid "$pid" | head -1)
val=$(printf 't=3000\x1ffg=#ff0000\x1f\x1eError message')
xprop -id "$wid" -f _ST_NOTIFY 8u -set _ST_NOTIFY "$val"
```

### Metadata keys

| Key | Description |
|-----|-------------|
| `t` | Timeout in ms |
| `b` | Border color (hex) |
| `bg` | Background color (hex) |
| `fg` | Foreground color (hex) |
| `ts` | Font pixel size |

### From C / X11 client

```c
XChangeProperty(dpy, win, XInternAtom(dpy, "_ST_NOTIFY", False),
    XInternAtom(dpy, "UTF8_STRING", False), 8, PropModeReplace,
    (unsigned char *)"Hello", 5);
```

## Behavior

- New notifications stack at the top-right corner, pushing existing ones down
- Each toast auto-dismisses independently after its timeout
- Maximum 8 simultaneous toasts; new ones evict the oldest if full
- The `_ST_NOTIFY` property is deleted after reading (no stale state)
- Window auto-sizes to text content plus padding (8px) and border (2px)
