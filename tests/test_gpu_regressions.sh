#!/usr/bin/env bash
# Regression tests for GPU renderer/window-manager integration.
# Requires xenv (nested Xephyr+dwm) and python3 with Pillow.
set -euo pipefail

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
st_bin=${ST_BIN:-"$repo/st"}
env_name="st-gpu-regress-$$"
title="st-gpu-regress-$$"
tmpdir=$(mktemp -d /tmp/st-gpu-regress.XXXXXX)

cleanup() {
	set +e
	if [[ -n "${st_pid:-}" ]]; then
		kill "$st_pid" 2>/dev/null || true
		wait "$st_pid" 2>/dev/null || true
	fi
	xenv stop "$env_name" >/dev/null 2>&1 || true
	rm -rf "$tmpdir"
}
trap cleanup EXIT

require() {
	command -v "$1" >/dev/null 2>&1 || {
		echo "missing required command: $1" >&2
		exit 77
	}
}

require xenv
require python3
python3 - <<'PY'
import PIL.Image  # noqa: F401
PY

xenv start "$env_name" >/dev/null
display=$(xenv display -e "$env_name")

DISPLAY=$display "$st_bin" -T "$title" -e python3 -c '
import os, sys, time
for i in range(36):
    print(f"GPUREGRESS-ROW-{i:02d} The quick brown fox lambda漢 emoji-ish text", flush=True)
time.sleep(0.7)
print("GPUREGRESS-LATE redraw sentinel", flush=True)
deadline = time.monotonic() + 10
while not os.path.exists(sys.argv[1]) and time.monotonic() < deadline:
    time.sleep(0.02)
if os.path.exists(sys.argv[1]):
    # Put an opaque magenta image on the final terminal row. C=1 leaves
    # the terminal cursor there so forced Ctrl+V can exercise cursor layering.
    sys.stdout.buffer.write(
        b"\x1b[999;1H\x1b_Ga=T,f=32,s=1,v=1,c=8,r=1,C=1,q=2;/wD//w==\x1b\\"
    )
    sys.stdout.buffer.flush()
time.sleep(30)
' "$tmpdir/image-trigger" >"$tmpdir/st.out" 2>"$tmpdir/st.err" &
st_pid=$!

# Wait until dwm has mapped the terminal at its initial tiled size.
for _ in $(seq 1 100); do
	windows=$(xenv windows -e "$env_name" || true)
	if printf '%s\n' "$windows" | grep -q " $title$"; then
		break
	fi
	sleep 0.05
done
windows=$(xenv windows -e "$env_name")
printf '%s\n' "$windows" >"$tmpdir/windows.txt"

python3 - "$title" "$tmpdir/windows.txt" <<'PY'
import re, sys

title, path = sys.argv[1:3]
root_w = root_h = bar_h = None
term_w = term_h = None
with open(path, encoding='utf-8') as f:
    for line in f:
        m = re.match(r"\s*\S+\s+(\d+)x(\d+)\s+(-?\d+),(-?\d+)\s+(.*)$", line.rstrip())
        if not m:
            continue
        w, h, x, y, name = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5)
        if name == '(unnamed)' and (root_w is None or w * h > root_w * root_h):
            root_w, root_h = w, h
        if name == '(unnamed)' and h <= 64:
            bar_h = max(bar_h or 0, h)
        if name == title:
            term_w, term_h = w, h

if None in (root_w, root_h, bar_h, term_w, term_h):
    raise SystemExit(f"could not parse xenv geometry: root={root_w}x{root_h} bar={bar_h} term={term_w}x{term_h}")

expected_w = root_w - 2          # dwm's one-pixel border on each side
expected_h = root_h - bar_h - 2  # bar plus one-pixel border on each side
if abs(term_w - expected_w) > 1 or abs(term_h - expected_h) > 1:
    raise SystemExit(
        f"initial GPU window was snapped away from full tile size: "
        f"got {term_w}x{term_h}, expected about {expected_w}x{expected_h} "
        f"(root {root_w}x{root_h}, bar {bar_h})"
    )
PY

# Wait for the later redraw.  If double-buffer contents are not preserved and
# clean rows are not redrawn, the earlier rows disappear/turn black here.
sleep 1.4
shot="$tmpdir/screenshot.png"
xenv screenshot -e "$env_name" -o "$shot" >/dev/null

python3 - "$shot" "$tmpdir/windows.txt" <<'PY'
import re, sys
from PIL import Image

shot, winpath = sys.argv[1:3]
bar_h = 0
with open(winpath, encoding='utf-8') as f:
    for line in f:
        m = re.match(r"\s*\S+\s+(\d+)x(\d+)\s+(-?\d+),(-?\d+)\s+(.*)$", line.rstrip())
        if not m:
            continue
        h = int(m.group(2)); name = m.group(5)
        if name == '(unnamed)' and h <= 64:
            bar_h = max(bar_h, h)

im = Image.open(shot).convert('RGB')
w, h = im.size
# Count bright/colored glyph pixels in the initial rows, excluding dwm's bar.
y1 = min(h, bar_h + 8)
y2 = min(h, bar_h + 520)
x2 = min(w, 900)
textish = 0
for y in range(y1, y2):
    for x in range(0, x2):
        r, g, b = im.getpixel((x, y))
        if r + g + b > 270 or (r > 120 and g < 100 and b < 100) or (g > 120 and r < 120):
            textish += 1

# A healthy 36-line screen has thousands of glyph pixels in this region.  When
# the stale/black back-buffer bug happens, only the late line/cursor remains and
# this drops close to zero.
if textish < 1800:
    raise SystemExit(f"initial rows vanished or rendered black after later redraw: only {textish} text-like pixels")
PY

# Image quads use scaled cell geometry. If their scissor still uses the smaller
# nominal integer grid, a tiled window's leftover pixels clip most of the final
# terminal row and expose the background below the image.
touch "$tmpdir/image-trigger"
sleep 0.4
imageshot="$tmpdir/image-bottom.png"
xenv screenshot -e "$env_name" -o "$imageshot" >/dev/null

python3 - "$imageshot" <<'PY'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert('RGB')
ys = []
for y in range(im.height):
    count = 0
    for x in range(min(160, im.width)):
        r, g, b = im.getpixel((x, y))
        if r > 240 and g < 25 and b > 240:
            count += 1
    if count >= 30:
        ys.append(y)

if not ys:
    raise SystemExit("bottom-row image did not render")
height = max(ys) - min(ys) + 1
if height < 14:
    raise SystemExit(f"bottom-row image was clipped to {height}px")
PY

# Forced nav uses a coral-red block cursor. Ctrl+V selects the cursor cell, so
# the GPU cursor draw must ignore selection highlighting rather than replacing
# its explicit red background with the gray selection color.
xenv -e "$env_name" key Shift+Escape ctrl+v >/dev/null
sleep 0.2
blockshot="$tmpdir/block-cursor.png"
xenv screenshot -e "$env_name" -o "$blockshot" >/dev/null

python3 - "$blockshot" <<'PY'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert('RGB')
coral = 0
for y in range(im.height):
    for x in range(min(160, im.width)):
        r, g, b = im.getpixel((x, y))
        if r > 235 and 75 < g < 145 and 75 < b < 145:
            coral += 1
if coral < 100:
    raise SystemExit(f"forced Ctrl+V cursor was not visibly coral: {coral}px")
PY

# Leave forced block mode so the existing cmdline test can enter forced mode
# from a clean state.
xenv -e "$env_name" key Shift+Escape >/dev/null

# The vim-style command line is an Xft child overlay even when the grid is drawn
# by the GPU.  It should align with the actual rendered bottom row, including
# descenders, rather than stale integer-grid geometry.
xenv -e "$env_name" key Shift+Escape >/dev/null
sleep 0.1
xenv -e "$env_name" type ':' >/dev/null
sleep 0.1
xenv -e "$env_name" type 'echo glyphs gjpqy' >/dev/null
sleep 0.2
cmdshot="$tmpdir/cmdline.png"
xenv screenshot -e "$env_name" -o "$cmdshot" >/dev/null

python3 - "$cmdshot" <<'PY'
import sys
from PIL import Image

shot = sys.argv[1]
im = Image.open(shot).convert('RGB')
w, h = im.size

def textish_in_band(y1, y2):
    count = 0
    for y in range(max(0, y1), min(h, y2)):
        for x in range(8, min(w, 520)):
            r, g, b = im.getpixel((x, y))
            if r + g + b > 300 or (r > 120 and g < 100 and b < 100) or (g > 120 and r < 120):
                count += 1
    return count

bottom42 = textish_in_band(h - 42, h)
bottom14 = textish_in_band(h - 14, h)

if bottom42 < 250:
    raise SystemExit(f"cmdline text was not visible at the bottom: only {bottom42} text-like pixels")
if bottom14 < 20:
    raise SystemExit(f"cmdline descenders/bottom strokes look clipped: only {bottom14} low pixels")
PY

echo "GPU regression tests passed: full-size mapping, preserved redraws, bottom-row images, forced block cursor, and cmdline alignment"
