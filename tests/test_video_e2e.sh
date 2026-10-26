#!/usr/bin/env bash
# End-to-end st video (a=v) on the Linux GPU renderer: a clip streamed
# through the pty plays to the end, frames advance on screen inside the
# placement only, and pause freezes the picture.  Sound is kept off the
# speakers by pointing PulseAudio at a socket that does not exist (st then
# keeps time with its silent output).
# Requires xenv (nested Xephyr+dwm), ffmpeg with libx264, python3 with Pillow.
set -euo pipefail

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
st_bin=${ST_BIN:-"$repo/st"}
env_name="st-video-e2e-$$"
title="st-video-e2e-$$"
tmpdir=$(mktemp -d /tmp/st-video-e2e.XXXXXX)

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
	for _ in $(seq 1 600); do
		[[ -e "$path" ]] && return 0
		sleep 0.025
	done
	echo "timed out waiting for $path" >&2
	return 1
}

require xenv
require ffmpeg
require python3
python3 - <<'PY'
import PIL.Image  # noqa: F401
PY
encoders=$(ffmpeg -hide_banner -encoders 2>/dev/null || true)
[[ "$encoders" == *libx264* ]] || {
	echo "ffmpeg without libx264" >&2
	exit 77
}

xenv start "$env_name" >/dev/null
display=$(xenv display -e "$env_name")

PULSE_SERVER=unix:/nonexistent DISPLAY=$display "$st_bin" -T "$title" -e python3 -c '
import os, select, sys, termios, time, tty

sys.dont_write_bytecode = True
sys.path.insert(0, sys.argv[2])
import video_feed as vf

tmp = sys.argv[1]
fd = sys.stdin.fileno()
tty.setraw(fd)
pending = b""

def emit(data):
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

def ready(name, text=""):
    with open(os.path.join(tmp, name), "w") as f:
        f.write(text)

def wait_for(name):
    path = os.path.join(tmp, name)
    deadline = time.monotonic() + 15
    while not os.path.exists(path) and time.monotonic() < deadline:
        time.sleep(0.02)

def status(seq):
    global pending
    emit(vf.apc("a=v,i=31,k=q,q=1", b"seq=%d" % seq))
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            pending += os.read(fd, 4096)
        key = b"seq=%d " % seq
        i = pending.find(key)
        if i >= 0 and pending.find(b"\x1b\\", i) >= 0:
            j = pending.find(b"\x1b\\", i)
            reply = pending[i:j].decode()
            pending = pending[j:]
            return dict(x.split("=", 1) for x in reply.split())
    return {}

class A:
    test_source = True; input = None; seconds = 3; width = 320; height = 180
    bitrate = "500k"; no_audio = False
units, frames = vf.transcode(A, tmp)
events = [(i * 1000000 // vf.FPS, 0, key, unit) for i, (unit, key) in enumerate(units)]
events += [(i * 1024 * 1000000 // vf.RATE, 1, False, f) for i, f in enumerate(frames)]
events.sort(key=lambda e: (e[0], e[1]))

emit(b"\x1b[2J\x1b[H\x1b[?25l\x1b[3;5H")
emit(vf.apc("a=v,i=31,k=o,q=2", b"video=h264 width=320 height=180 audio=aac rate=48000 channels=2"))
emit(vf.apc("a=p,i=31,p=1,c=24,r=8,C=1,q=2"))
start = time.monotonic()
seq = 0
for pts, stream, key, data in events:
    wait = pts / 1e6 - 0.5 - (time.monotonic() - start)
    if wait > 0:
        time.sleep(wait)
    emit(vf.apc("a=v,i=31,k=d", vf.packet(stream, key, pts, data)))
    if stream == 0 and pts >= 1000000 and seq == 0:
        seq = 1
        ready("playing", repr(status(seq)))
emit(vf.apc("a=v,i=31,k=e,q=2"))
wait_for("shot-1")
ready("first-done")
wait_for("shot-2")
emit(vf.apc("a=v,i=31,k=s,q=2", b"pause=1"))
time.sleep(0.3)
ready("paused")
wait_for("shot-3")
emit(vf.apc("a=v,i=31,k=s,q=2", b"pause=0"))
deadline = time.monotonic() + 10
st = {}
while time.monotonic() < deadline:
    seq += 1
    st = status(seq)
    if st.get("state") == "ended":
        break
    time.sleep(0.1)
ready("ended", repr(st))
time.sleep(30)
' "$tmpdir" "$repo/tests" &
st_pid=$!

wait_file "$tmpdir/playing"
for _ in $(seq 1 200); do
	windows=$(xenv windows -e "$env_name" || true)
	[[ "$windows" == *"$title"* ]] && break
	sleep 0.025
done
# Park the pointer in the corner the pixel checks ignore.
xenv move -e "$env_name" 5000 5000 >/dev/null 2>&1 || true
xenv screenshot -e "$env_name" -o "$tmpdir/a.png" >/dev/null
sleep 0.4
xenv screenshot -e "$env_name" -o "$tmpdir/b.png" >/dev/null
touch "$tmpdir/shot-1"
wait_file "$tmpdir/first-done"
touch "$tmpdir/shot-2"
wait_file "$tmpdir/paused"
xenv screenshot -e "$env_name" -o "$tmpdir/p1.png" >/dev/null
sleep 0.4
xenv screenshot -e "$env_name" -o "$tmpdir/p2.png" >/dev/null
touch "$tmpdir/shot-3"
wait_file "$tmpdir/ended"

python3 - "$tmpdir" <<'PY'
import ast, os, sys
from PIL import Image, ImageChops

tmp = sys.argv[1]
playing = ast.literal_eval(open(os.path.join(tmp, "playing")).read())
ended = ast.literal_eval(open(os.path.join(tmp, "ended")).read())
a, b, p1, p2 = (Image.open(os.path.join(tmp, n)).convert("RGB")
                for n in ("a.png", "b.png", "p1.png", "p2.png"))

def box(image):
    """Bounding box of non-background pixels inside the window borders."""
    bg = image.getpixel((image.width - 50, image.height - 50))
    diff = ImageChops.difference(image, Image.new("RGB", image.size, bg))
    diff = diff.crop((4, 25, image.width - 60, image.height - 60))
    bbox = diff.convert("L").point(lambda v: 255 if v > 40 else 0).getbbox()
    return None if bbox is None else (bbox[0] + 4, bbox[1] + 25,
                                      bbox[2] + 4, bbox[3] + 25)

assert playing.get("state") == "playing", playing
assert ended.get("state") == "ended", ended
assert int(ended["shown"]) >= 80 and int(ended["dropped"]) <= 5, ended
assert "error" not in ended, ended
frame = box(a)
assert frame is not None, "no video pixels on screen"
width, height = frame[2] - frame[0], frame[3] - frame[1]
# c=24,r=8 cells at row 3, column 5: a block in the top left, nothing
# else drawn.
assert frame[0] > 4 and frame[1] > 25 and width > 100 and height > 60, frame
assert frame[2] < a.width // 2 and frame[3] < a.height // 2, frame
assert box(b) == frame, (box(b), frame)
moved = ImageChops.difference(a.crop(frame), b.crop(frame)).getbbox()
assert moved is not None, "frames did not advance"
still = ImageChops.difference(p1.crop(frame), p2.crop(frame)).getbbox()
assert still is None, "paused video kept changing"
if os.environ.get("ST_VIDEO_E2E_KEEP"):
    a.save(os.environ["ST_VIDEO_E2E_KEEP"])
print("Video e2e passed: plays to the end (shown=%s dropped=%s), frames "
      "advance inside a %dx%d placement, pause freezes the picture" %
      (ended["shown"], ended["dropped"], width, height))
PY
