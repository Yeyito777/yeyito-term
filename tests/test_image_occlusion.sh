#!/usr/bin/env bash
# Verify that later terminal-cell content occludes positive-z Kitty images per cell.
# Requires xenv (nested Xephyr+dwm) and python3 with Pillow.
set -euo pipefail

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
st_bin=${ST_BIN:-"$repo/st"}
env_name="st-image-occlusion-$$"
title="st-image-occlusion-$$"
tmpdir=$(mktemp -d /tmp/st-image-occlusion.XXXXXX)

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

wait_file() {
	local path=$1
	for _ in $(seq 1 200); do
		[[ -e "$path" ]] && return 0
		sleep 0.025
	done
	echo "timed out waiting for $path" >&2
	return 1
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

tmp = sys.argv[1]

def emit(data):
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

def ready(name):
    open(os.path.join(tmp, name), "w").close()

def wait_for(name):
    path = os.path.join(tmp, name)
    deadline = time.monotonic() + 10
    while not os.path.exists(path) and time.monotonic() < deadline:
        time.sleep(0.02)
    return os.path.exists(path)

# Every image cell starts over ordinary terminal content. This catches a
# renderer that decides occlusion from only relative cell 0,0 instead of
# remembering every cell in a larger image footprint.
underlay = b"12345678"
emit(b"\x1b[2J\x1b[H\x1b[?25l")
for row in range(5, 9):
    emit(f"\x1b[{row};10H".encode() + underlay)
emit(b"\x1b_Ga=t,t=d,f=32,s=1,v=1,i=424242,q=2;/wD//w==\x1b\\")
emit(b"\x1b[5;10H\x1b_Ga=p,i=424242,c=8,r=4,C=1,z=1478,q=2\x1b\\")
ready("baseline-ready")

if wait_for("show-menu"):
    # Paint only the right half through the image, then mimic a TUI frame that
    # refreshes retained image placements after its text writes.
    for row in range(5, 9):
        emit(f"\x1b[{row};14H".encode() + b"\x1b[38;2;255;255;255;48;2;0;90;230mMENU\x1b[0m")
    emit(b"\x1b[5;10H\x1b_Ga=d,d=z,z=1478,q=2\x1b\\")
    emit(b"\x1b_Ga=p,i=424242,c=8,r=4,C=1,z=1478,q=2\x1b\\")
    ready("right-menu-ready")

if wait_for("move-menu-left"):
    # Moving the cover across relative cell 0,0 must not hide the unchanged
    # right half of the image as one indivisible quad.
    for row in range(5, 9):
        emit(f"\x1b[{row};10H".encode() + underlay)
        emit(f"\x1b[{row};10H".encode() + b"\x1b[38;2;255;255;255;48;2;0;90;230mMENU\x1b[0m")
    emit(b"\x1b[5;10H\x1b_Ga=d,d=z,z=1478,q=2\x1b\\")
    emit(b"\x1b_Ga=p,i=424242,c=8,r=4,C=1,z=1478,q=2\x1b\\")
    ready("left-menu-ready")

if wait_for("restore-image"):
    # Restoring every original cell must reveal the retained image again.
    for row in range(5, 9):
        emit(f"\x1b[{row};10H".encode() + underlay)
    emit(b"\x1b[5;10H\x1b_Ga=d,d=z,z=1478,q=2\x1b\\")
    emit(b"\x1b_Ga=p,i=424242,c=8,r=4,C=1,z=1478,q=2\x1b\\")
    ready("restore-ready")

time.sleep(20)
' "$tmpdir" >"$tmpdir/st.out" 2>"$tmpdir/st.err" &
st_pid=$!

# Wait for both the child output and dwm mapping before taking exact screenshots.
wait_file "$tmpdir/baseline-ready"
for _ in $(seq 1 200); do
	windows=$(xenv windows -e "$env_name" || true)
	printf '%s\n' "$windows" | grep -q " $title$" && break
	sleep 0.025
done
sleep 0.2
xenv screenshot -e "$env_name" -o "$tmpdir/baseline.png" >/dev/null

touch "$tmpdir/show-menu"
wait_file "$tmpdir/right-menu-ready"
sleep 0.15
xenv screenshot -e "$env_name" -o "$tmpdir/right-menu.png" >/dev/null

touch "$tmpdir/move-menu-left"
wait_file "$tmpdir/left-menu-ready"
sleep 0.15
xenv screenshot -e "$env_name" -o "$tmpdir/left-menu.png" >/dev/null

touch "$tmpdir/restore-image"
wait_file "$tmpdir/restore-ready"
sleep 0.15
xenv screenshot -e "$env_name" -o "$tmpdir/restored.png" >/dev/null

python3 - "$tmpdir/baseline.png" "$tmpdir/right-menu.png" "$tmpdir/left-menu.png" "$tmpdir/restored.png" <<'PY'
import sys
from PIL import Image

baseline, right_menu, left_menu, restored = [Image.open(path).convert("RGB") for path in sys.argv[1:5]]

def magenta(pixel):
    r, g, b = pixel
    return r > 235 and g < 35 and b > 235

def blue(pixel):
    r, g, b = pixel
    return r < 45 and 45 < g < 135 and b > 180

def longest_run(image):
    best = None
    for y in range(image.height):
        start = None
        for x in range(image.width + 1):
            hit = x < image.width and magenta(image.getpixel((x, y)))
            if hit and start is None:
                start = x
            elif not hit and start is not None:
                candidate = (x - start, start, x - 1, y)
                if best is None or candidate[0] > best[0]:
                    best = candidate
                start = None
    return best

run = longest_run(baseline)
if run is None or run[0] < 50:
    raise SystemExit(f"baseline inline image was not visible: longest magenta run={run}")

width, x0, x1, center_y = run
rows = []
for y in range(baseline.height):
    hits = sum(magenta(baseline.getpixel((x, y))) for x in range(x0, x1 + 1))
    if hits >= width * 0.8:
        rows.append(y)
if not rows or max(rows) - min(rows) + 1 < 50:
    raise SystemExit("baseline inline image was unexpectedly short or fragmented")
y0, y1 = min(rows), max(rows)

# MENU occupies only the right four columns of the image’s eight-column
# footprint. The left half must remain image pixels throughout.
covered0 = x0 + round(width * 4 / 8)
covered1 = x1
covered_area = max(1, (covered1 - covered0 + 1) * (y1 - y0 + 1))
right_blue = sum(
    blue(right_menu.getpixel((x, y)))
    for y in range(y0, y1 + 1)
    for x in range(covered0, covered1 + 1)
)
right_magenta = sum(
    magenta(right_menu.getpixel((x, y)))
    for y in range(y0, y1 + 1)
    for x in range(covered0, covered1 + 1)
)
right_menu_left_magenta = sum(
    magenta(right_menu.getpixel((x, y)))
    for y in range(y0, y1 + 1)
    for x in range(x0, covered0)
)

if right_blue < covered_area * 0.35:
    raise SystemExit(
        f"terminal MENU did not occlude the image’s right half: "
        f"blue={right_blue}/{covered_area}, magenta={right_magenta}/{covered_area}"
    )
if right_magenta > covered_area * 0.20:
    raise SystemExit(f"image remained over too much of right MENU: {right_magenta}/{covered_area}")
if right_menu_left_magenta < covered_area * 0.80:
    raise SystemExit(
        f"right-half MENU incorrectly hid or exposed the image’s left half: "
        f"left magenta={right_menu_left_magenta}/{covered_area}"
    )

left_blue = sum(
    blue(left_menu.getpixel((x, y)))
    for y in range(y0, y1 + 1)
    for x in range(x0, covered0)
)
left_magenta = sum(
    magenta(left_menu.getpixel((x, y)))
    for y in range(y0, y1 + 1)
    for x in range(x0, covered0)
)
left_menu_right_magenta = sum(
    magenta(left_menu.getpixel((x, y)))
    for y in range(y0, y1 + 1)
    for x in range(covered0, covered1 + 1)
)
if left_blue < covered_area * 0.35:
    raise SystemExit(
        f"terminal MENU did not occlude relative cell 0,0 and the image’s left half: "
        f"blue={left_blue}/{covered_area}, magenta={left_magenta}/{covered_area}"
    )
if left_magenta > covered_area * 0.20:
    raise SystemExit(f"image remained over too much of left MENU: {left_magenta}/{covered_area}")
if left_menu_right_magenta < covered_area * 0.80:
    raise SystemExit(
        f"covering relative cell 0,0 incorrectly hid the image’s right half: "
        f"right magenta={left_menu_right_magenta}/{covered_area}"
    )

restored_run = longest_run(restored)
if restored_run is None or restored_run[0] < width - 2:
    raise SystemExit(
        f"image did not reappear after placeholders were restored: "
        f"baseline={width}px restored={restored_run}"
    )
restored_magenta = sum(
    magenta(restored.getpixel((x, y)))
    for y in range(y0, y1 + 1)
    for x in range(x0, x1 + 1)
)
image_area = width * (y1 - y0 + 1)
if restored_magenta < image_area * 0.95:
    raise SystemExit(
        f"only part of the image reappeared after restoring its cells: "
        f"magenta={restored_magenta}/{image_area}"
    )

print("Image occlusion regression passed: either half is covered independently and restoration reveals the whole image")
PY
