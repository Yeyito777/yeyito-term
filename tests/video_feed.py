#!/usr/bin/env python3
"""Stream a clip to the terminal on stdout with st video (docs/video.md).

A test feeder for backend work, not a player: st-play is the real sender.
ffmpeg transcodes the input to constant-frame-rate H.264 (no B-frames, an
AUD before every access unit) and ADTS AAC in a scratch directory; the
feeder then writes open, place, packets paced to the wall clock, end, and
finally deletes the video.  Replies from the terminal are not read.

  tests/video_feed.py clip.mp4 --cols 60 --rows 20 --seconds 5
  tests/video_feed.py --test-source --seconds 3   # ffmpeg testsrc2 + sine
"""
import argparse
import base64
import os
import struct
import subprocess
import sys
import tempfile
import time

FPS = 30
RATE = 48000


def apc(controls, payload=b""):
    data = base64.b64encode(payload).decode() if payload else ""
    out = []
    if len(data) <= 4096:
        out.append("\033_G%s%s%s\033\\" % (controls, ";" if data else "", data))
    else:
        for i in range(0, len(data), 4096):
            more = 1 if i + 4096 < len(data) else 0
            head = "%s,m=%d" % (controls, more) if i == 0 else "m=%d" % more
            out.append("\033_G%s;%s\033\\" % (head, data[i:i + 4096]))
    return "".join(out).encode()


def packet(stream, key, pts_us, data):
    return struct.pack(">BBq", stream, 1 if key else 0, pts_us) + data


def access_units(h264):
    """Split Annex B at access unit delimiters (NAL type 9)."""
    starts = []
    i = 0
    while True:
        i = h264.find(b"\x00\x00\x01", i)
        if i < 0:
            break
        if i + 3 < len(h264) and h264[i + 3] & 0x1f == 9:
            starts.append(i - 1 if i > 0 and h264[i - 1] == 0 else i)
        i += 3
    starts.append(len(h264))
    for a, b in zip(starts, starts[1:]):
        unit = h264[a:b]
        types = set()
        j = 0
        while True:
            j = unit.find(b"\x00\x00\x01", j)
            if j < 0 or j + 3 >= len(unit):
                break
            types.add(unit[j + 3] & 0x1f)
            j += 3
        yield unit, 5 in types


def adts_frames(aac):
    i = 0
    while i + 7 <= len(aac):
        if aac[i] != 0xff or aac[i + 1] & 0xf0 != 0xf0:
            raise SystemExit("bad ADTS sync at %d" % i)
        protection_absent = aac[i + 1] & 1
        length = ((aac[i + 3] & 3) << 11) | (aac[i + 4] << 3) | (aac[i + 5] >> 5)
        header = 7 if protection_absent else 9
        yield aac[i + header:i + length]
        i += length


def transcode(args, scratch):
    video = os.path.join(scratch, "v.h264")
    audio = os.path.join(scratch, "a.aac")
    if args.test_source:
        inputs = ["-f", "lavfi", "-i", "testsrc2=size=%dx%d:rate=%d" %
                  (args.width, args.height, FPS),
                  "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=%d" % RATE]
        vmap, amap = ["-map", "0:v"], ["-map", "1:a"]
    else:
        inputs = ["-i", args.input]
        vmap, amap = ["-map", "0:v:0"], ["-map", "0:a:0?"]
    common = ["ffmpeg", "-v", "error", "-y"] + inputs + ["-t", str(args.seconds)]
    subprocess.run(common + vmap + [
        "-vf", "scale=%d:%d,fps=%d" % (args.width, args.height, FPS),
        "-c:v", "libx264", "-preset", "veryfast", "-bf", "0",
        "-g", str(FPS * 2), "-b:v", args.bitrate, "-pix_fmt", "yuv420p",
        "-x264-params", "aud=1:repeat-headers=1", "-f", "h264", video],
        check=True)
    have_audio = not args.no_audio
    if have_audio:
        subprocess.run(common + amap + [
            "-c:a", "aac", "-b:a", "128k", "-ar", str(RATE), "-ac", "2",
            "-f", "adts", audio], check=True)
        have_audio = os.path.getsize(audio) > 0
    with open(video, "rb") as f:
        units = list(access_units(f.read()))
    frames = []
    if have_audio:
        with open(audio, "rb") as f:
            frames = list(adts_frames(f.read()))
    return units, frames


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", nargs="?")
    parser.add_argument("--test-source", action="store_true")
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--bitrate", default="1500k")
    parser.add_argument("--cols", type=int, default=64)
    parser.add_argument("--rows", type=int, default=0)
    parser.add_argument("--id", type=int, default=4242)
    parser.add_argument("--ahead", type=float, default=1.0,
                        help="seconds of media sent ahead of the wall clock")
    parser.add_argument("--no-audio", action="store_true")
    parser.add_argument("--volume", default="1")
    parser.add_argument("--keep", action="store_true",
                        help="leave the last frame instead of deleting it")
    args = parser.parse_args()
    if not args.input and not args.test_source:
        parser.error("give an input or --test-source")

    with tempfile.TemporaryDirectory(prefix="st-video-feed-") as scratch:
        units, frames = transcode(args, scratch)
    events = [(i * 1000000 // FPS, 0, key, unit)
              for i, (unit, key) in enumerate(units)]
    events += [(i * 1024 * 1000000 // RATE, 1, False, frame)
               for i, frame in enumerate(frames)]
    events.sort(key=lambda e: (e[0], e[1]))

    out = sys.stdout.buffer
    config = "video=h264 width=%d height=%d volume=%s" % (
        args.width, args.height, args.volume)
    if frames:
        config += " audio=aac rate=%d channels=2" % RATE
    out.write(apc("a=v,i=%d,k=o,q=2" % args.id, config.encode()))
    place = "a=p,i=%d,p=1,c=%d,C=1,q=2" % (args.id, args.cols)
    if args.rows:
        place += ",r=%d" % args.rows
    out.write(apc(place))
    out.flush()
    start = time.monotonic()
    for pts, stream, key, data in events:
        wait = pts / 1e6 - args.ahead - (time.monotonic() - start)
        if wait > 0:
            out.flush()
            time.sleep(wait)
        out.write(apc("a=v,i=%d,k=d" % args.id, packet(stream, key, pts, data)))
    out.write(apc("a=v,i=%d,k=e,q=2" % args.id))
    out.flush()
    last = max(e[0] for e in events) / 1e6 if events else 0
    remaining = last + 0.5 - (time.monotonic() - start)
    if remaining > 0:
        time.sleep(remaining)
    if not args.keep:
        out.write(apc("a=d,d=I,i=%d,q=2" % args.id))
    out.flush()


if __name__ == "__main__":
    main()
