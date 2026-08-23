#!/usr/bin/env bash
# Verify that repainting a row does not blend the glyph beneath the GPU cursor twice.
# Requires xenv (nested Xephyr+dwm) and python3 with Pillow.
set -euo pipefail

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
st_bin=${ST_BIN:-"$repo/st"}
env_name="st-cursor-rendering-$$"
title="st-cursor-rendering-$$"
tmpdir=$(mktemp -d /tmp/st-cursor-rendering.XXXXXX)

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

size_path, ready_path = sys.argv[1:]
with open(size_path, "w", encoding="ascii") as size_file:
    size_file.write("%d %d" % os.get_terminal_size())

# Use identical medium-gray glyphs and a steady underline cursor. The second
# write dirties the row while leaving the cursor over the middle M. A buggy
# renderer appends that M to the base glyph batch twice.
initial = "\x1b[2J\x1b[H\x1b[38;2;96;96;96mMMMMM\x1b[1;3H\x1b[4 q"
sys.stdout.write(initial)
sys.stdout.flush()
time.sleep(0.4)
sys.stdout.write("\x1b[1;1HMMMMM\x1b[1;3H")
sys.stdout.flush()
with open(ready_path, "w", encoding="ascii"):
    pass
time.sleep(10)
' "$tmpdir/size" "$tmpdir/ready" >"$tmpdir/st.out" 2>"$tmpdir/st.err" &
st_pid=$!

for _ in $(seq 1 200); do
	xenv windows -e "$env_name" >"$tmpdir/windows"
	if [[ -e "$tmpdir/ready" ]] && grep -q " $title$" "$tmpdir/windows"; then
		break
	fi
	sleep 0.025
done
[[ -e "$tmpdir/ready" ]] || { echo "cursor test application did not become ready" >&2; exit 1; }
grep -q " $title$" "$tmpdir/windows" || { echo "cursor test terminal did not map" >&2; exit 1; }
sleep 0.15
xenv screenshot -e "$env_name" -o "$tmpdir/screenshot.png" >/dev/null

python3 - "$tmpdir/screenshot.png" "$tmpdir/windows" "$tmpdir/size" "$title" <<'PY'
import re
import sys
from PIL import Image

shot_path, windows_path, size_path, title = sys.argv[1:]
image = Image.open(shot_path).convert("RGB")
columns, rows = map(int, open(size_path, encoding="ascii").read().split())
geometry = None
with open(windows_path, encoding="utf-8") as windows:
    for line in windows:
        match = re.match(r"\s*\S+\s+(\d+)x(\d+)\s+(-?\d+),(-?\d+)\s+(.*)$", line.rstrip())
        if match and match.group(5) == title:
            width, height, x, y = map(int, match.group(1, 2, 3, 4))
            geometry = x, y, width, height
            break
if geometry is None:
    raise SystemExit("could not locate cursor test terminal geometry")

x, y, width, height = geometry
border = 2
cell_width = (width - 2 * border) / columns
cell_height = (height - 2 * border) / rows

def glyph_ink(column):
    x0 = round(x + border + column * cell_width)
    x1 = round(x + border + (column + 1) * cell_width)
    y0 = round(y + border)
    y1 = round(y + border + cell_height)
    # Exclude the left edge and bottom of each cell so the underline cursor
    # itself cannot affect the comparison. Count neutral-gray antialias pixels.
    values = []
    for py in range(y0, y1 - 3):
        for px in range(x0 + 3, x1):
            red, green, blue = image.getpixel((px, py))
            if abs(red - green) < 3 and abs(green - blue) < 3 and red > 10:
                values.append(red)
    return len(values), sum(values)

left = glyph_ink(1)
cursor = glyph_ink(2)
right = glyph_ink(3)
reference_count = max(left[0], right[0])
reference_sum = max(left[1], right[1])
if cursor[0] > reference_count + 2 or cursor[1] > reference_sum * 1.12:
    raise SystemExit(
        "glyph beneath cursor was blended twice: "
        f"left={left}, cursor={cursor}, right={right}"
    )

print(f"GPU cursor glyph regression passed: left={left}, cursor={cursor}, right={right}")
PY
