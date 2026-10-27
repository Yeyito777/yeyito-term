# Video (st extension to Kitty graphics)

`st` plays compressed video and audio that an application streams through the
terminal itself. The application sends H.264 and AAC packets inside Kitty
graphics APC strings. `st` decodes them (VideoToolbox on macOS, libavcodec
on Linux), plays the sound on the machine running the terminal, and shows the
frames through ordinary image placements. As with direct image transmission,
nothing names a file, socket or shared-memory object on the terminal's host,
so a player on the far side of an SSH connection works unchanged, and a
compressed stream fits on links where raw frames (`mpv --vo=kitty`) can't.

`st-play` (`play/st-play.c`) is the reference sender, a terminal video player
that transcodes any input libavformat can open.

A video is a Kitty image whose pixels change over time. Everything that
applies to images applies to it: placements (`a=p`) with cell or natural
sizing, source crops, z-index, scrollback anchoring, image clip regions,
cell occlusion, and deletion. Only how its pixels arrive is new.

## Wire format

Every command is a Kitty graphics APC with the st-specific action `a=v`:

```
ESC _ G a=v,i=<id>,k=<command>[,q=<quiet>][,m=<more>] ; <base64 payload> ESC \
```

- `i` is the image id of the video and is required (nonzero). `I` is not
  accepted.
- `k` selects the command below.
- `m=1` chunking works as for image data: a long payload may be split over
  several APCs, the continuation ones carrying only `m` (and optionally `q`).
  No other graphics command may come between the chunks.
- The payload is always base64 (padded or unpadded). Commands without data
  may omit it.

### Commands

| `k` | Command | Payload |
| --- | --- | --- |
| `c` | Capability query | none |
| `o` | Open (create or replace) a video | text configuration |
| `d` | Media packet | binary packet |
| `f` | Flush (seek) | text, optional `start=<us>` |
| `s` | Set playback state | text, `pause=0\|1` and/or `volume=<0..1>` |
| `e` | End of stream | none |
| `q` | Status query / ping | text, optional `seq=<n>` |

Text payloads are `key=value` pairs separated by spaces or newlines (at most
1,024 bytes). Unknown keys are ignored. Numbers are decimal.

#### `k=c`: capability query

```
ESC _ G a=v,i=31,k=c ESC \
```

A terminal with video support answers

```
ESC _ G i=31;OK video=h264 audio=aac max=4096x4096 ESC \
```

and `ENOSYS:...` when the renderer can't play video. As with the Kitty image
query, send DA1 (`ESC [ c`) right after it: a terminal that answers DA1
without answering the query has no video support. The id is only used to
match the reply; no image is created.

#### `k=o`: open

```
video=h264 width=1280 height=720 audio=aac rate=48000 channels=2 config=1190
```

| Key | Meaning |
| --- | --- |
| `video` | `h264`. Omit for an audio-only stream. |
| `width`, `height` | Display size of the image in pixels (1..4096). Required with `video`. Placements size and crop in these units whatever the coded size of the frames, so the sender may change resolution mid-stream. |
| `audio` | `aac` (AAC-LC). Omit for a silent video. |
| `rate`, `channels` | Audio sample rate (8000..96000) and channel count (1..2). Required with `audio`. |
| `config` | AudioSpecificConfig as hex (optional; derived from `rate` and `channels` for AAC-LC when absent). |
| `prebuffer` | Milliseconds of media to buffer before playback starts or resumes after running dry (default 300, at most 10000). |
| `pause` | `1` opens the video paused. |
| `volume` | Initial volume, 0..1 (default 1). |

Opening an id that is already in use replaces that image or video together
with its placements, like retransmitting an image. At most four videos may be
open at once (`ENOSPC`). The reply is `OK` unless `q` suppresses it.

The new video has no placement; show it with an ordinary placement, for
example `ESC _ G a=p,i=31,p=1,c=80,r=24,C=1,q=2 ESC \`. Until the first frame
is decoded a placement draws nothing.

#### `k=d`: media packet

The decoded payload is a 10-byte header followed by codec data:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | Stream: `0` video, `1` audio |
| 1 | 1 | Flags: bit 0 = keyframe (an IDR access unit) |
| 2 | 8 | Presentation time in microseconds, signed 64-bit big-endian, within ±2^62 |
| 10 | n | Codec data |

- Video data is one H.264 access unit in Annex B form (start codes). The
  first video packet after an open or flush must be a keyframe carrying its
  SPS and PPS in-band. A keyframe may carry new parameter sets, for example a
  new resolution.
- Audio data is one raw AAC frame (no ADTS header).
- Packets are decoded in the order they arrive. Video timestamps may be out
  of order (B-frames). Both streams share one timeline.

Packets never produce a reply, so they can't flood an application that
doesn't read its input. A problem is kept and reported by a `k=q` as
`error=`: a full buffer by the next one, undecodable data once st has
decoded that packet (decoding runs beside the parser, so a query sent
right after a bad packet may not report it yet).

#### `k=f`: flush

Discards every queued, decoded and playing packet so that the next packets
start a new segment of the timeline (a seek). With `start=<us>`, frames
earlier than `start` are decoded but not shown and audio before it is
dropped, which allows exact seeks into a stream that only has keyframes every
few seconds. Playback then rebuffers (`prebuffer`) and continues unless the
video is paused. Flush also clears the end-of-stream state and the reported
error.

#### `k=s`: set

`pause=1` stops the clock: the current frame stays and audio pauses.
`pause=0` resumes. `volume=` sets the volume (0 mutes).

#### `k=e`: end of stream

No more packets follow until the next flush. `st` plays what it has; then the
state becomes `ended` and the last frame stays on screen.

#### `k=q`: status query

```
ESC _ G a=v,i=31,k=q;<base64 "seq=7"> ESC \
```

is answered, unless `q=2`, with

```
ESC _ G i=31;OK seq=7 state=playing pos=1234567 vbuf=3400000 abuf=3412000 shown=36 dropped=0 rx=482113 ESC \
```

| Field | Meaning |
| --- | --- |
| `seq` | Echo of the query's `seq` (omitted when it had none). |
| `state` | `buffering`, `playing`, `paused` or `ended`. |
| `pos` | Presentation time being played, in microseconds (`-1` before the first frame). |
| `vbuf`, `abuf` | Timestamp of the newest queued video / audio packet (`-1` if none). |
| `shown`, `dropped` | Frames presented, and frames decoded but skipped because they were late or before a flush `start`. |
| `rx` | Decoded payload bytes received since the open. |
| `error` | The first error since the open or last flush, as `CODE` (for example `EBADMSG`), if any. |

An unknown id is answered with `ENOENT:video not found`.

Commands are handled in the order they arrive. The reply to a query is
written only once every byte before it has been processed, so it doubles as
an acknowledgement (see [Flow control](#flow-control-for-senders)).

#### Closing

Delete the image: `ESC _ G a=d,d=I,i=31 ESC \` stops playback at once and
frees the decoder. Deleting only its placements (`d=i`) leaves it playing,
with the sound still on. A terminal reset (`ESC c`) closes every video.

## Playback model

After an open or flush the video is `buffering`. It starts `playing` once it
has a decoded frame (when it has video) and `prebuffer` milliseconds of media
(or once end of stream arrives). When a stream runs dry before end of stream,
the clock stops (`buffering`) until `prebuffer` milliseconds are queued again.

The clock follows the audio output when there is audio and the host clock
otherwise. Each redraw shows the newest decoded frame whose timestamp is not
after the clock. Frames that were never shown because a newer one was due
count as `dropped`. Frames keep decoding while a placement is off screen or
the window is hidden, so the sound and the picture stay in step.

Every placement of a video shows the same current frame. A video image has no
retained pixels, so selecting it copies nothing as an image.

## Flow control for senders

Bytes written to a PTY can't be taken back. Over SSH, several seconds of
data can sit in socket and channel buffers. A sender that writes as fast as
the pipe accepts will buffer the whole file into the terminal's memory and
make every pause or seek wait behind it. `st-play` therefore:

1. Sends `k=q` with an increasing `seq` every ~32 KiB written (and at least
   every 100 ms while sending, 250 ms otherwise), with at most 16 queries
   unanswered so their replies never fill the terminal's tty buffer. When
   the reply for `seq` arrives, every byte written before that query has
   reached the terminal.
2. Keeps the bytes written after the newest acknowledged query below a
   window of one round trip plus ~80 ms (at least 200 ms) at the delivery
   rate, plus the bytes one burst of replies acknowledges, at least 64 KiB.
   The window never plans for more than twice the rate actually seen in the
   last half second. Pause, seek and quit commands go ahead of queued
   packets and reach the terminal within about that time.
3. Keeps at most a few seconds of media ahead of the reported `pos` (5 s
   over a link, 2 s locally).
4. Measures the delivery rate as acknowledged bytes per second over the
   last 3 s, counting only intervals in which it always had media to send
   (not app-limited), because replies arrive in bursts and short spans
   overstate it. A rate well below the estimate for two round trips, with
   a window big enough to hold the link at that rate, lowers it at once;
   one 30% higher over 1.5 s raises it. While the media budget is full it
   presumes the link 20% a second faster (window-limited acks correct it
   once the encoder outruns the link) and probes for a second with a
   larger window. The encoder's bitrate follows
   the rate (base64 adds a third to every payload byte; APC and packet
   headers ~42 bytes a packet) and uses less of it the less media the
   terminal holds (`vbuf`/`abuf`); the resolution steps through a ladder
   that never exceeds the source or the placement, up after 3 s, down
   after 1 s.

## st-play

```
st-play [--inline ROWS] [--start S] [--copy] [--bitrate KBPS]
        [--max-bitrate KBPS] [--preset NAME] [--volume V] [--mute]
        [--no-audio] [--prebuffer MS] [--ahead S] [--stats FILE]
        [--log FILE] FILE|URL|-
```

Fullscreen on the alternate screen with a status line, or `--inline ROWS`
at the cursor. Keys: space/`p` pause, left/right 5 s, down/up 60 s,
PgDn/PgUp 10 min, `m` mute, `9`/`0` volume, `q`/Esc/Ctrl-C quit. Seeks are
exact (decode to the target, `k=f` with `start=`, a new IDR). `--copy` passes
8-bit H.264 through when the link has room for it (about 1.5 times its
bitrate) and switches to transcoding at once when it falls short. `--stats` logs the flow and
adaptation state every 250 ms. Video is always H.264 (libx264, Annex B,
SPS/PPS on every IDR), audio AAC-LC 48 kHz stereo.

`tests/fake_video_term` runs st-play on a PTY behind a link with a set
bandwidth, round-trip time and buffer, parses the stream with `graphics.c`
and `video.c`, and validates it with libavcodec; `tests/test_play_e2e.sh`
uses it in `make test`. `tests/pty_throttle` puts the same kind of link
between a real terminal and st-play:
`st -e tests/pty_throttle --bw 3000 --rtt 150 -- st-play clip.mp4`.

## Limits

| Resource | Limit |
| --- | ---: |
| Open videos | 4 |
| Display size | 4096 x 4096 |
| Packet payload (one APC or chunked transfer) | 64 MiB decoded |
| Queued compressed media per video | 64 MiB (`ENOBUFS`) |
| Text payload | 1,024 bytes |

## Implementation

| Path | Responsibility |
| --- | --- |
| `video.h`, `video.c` | Backend-neutral parsing: configuration text, packet headers, status replies, and the `VideoBackend` interface. |
| `graphics.c` | `a=v` dispatch, video images and their lifecycle (open, replace, delete, reset). |
| `macos/videoplayer.m` | VideoToolbox decode (each video's session lives on its own serial queue: creating one can take 50 ms), `AVSampleBufferAudioRenderer` and `AVSampleBufferRenderSynchronizer` playback, frames as Metal textures. |
| `render/video_linux.c` | libavcodec decode, PulseAudio playback, frames as OpenGL textures. |
| `play/` | `st-play`: reference sender and terminal video player (`media.c` ffmpeg pipeline, `flow.c` flow control and adaptation, `out.c` APC framing and write queue, `input.c` key and reply parser). |
| `tests/test_video.c` | Protocol and lifecycle tests against a mock backend. |

`ST_VIDEO_VOLUME` (0..1) scales the volume of every video, so tests and
harnesses can play real streams silently (`ST_VIDEO_VOLUME=0`). On macOS,
`make macos_headless` builds `tests/macos_headless`, a windowless st that
runs a command in a PTY through the real terminal core and renderer and
saves PNG snapshots, for end-to-end runs such as
`tests/macos_headless -- ssh -t host st-play clip.mp4`. With `-A` it runs
st's own event loop and reports frame times and main-thread stalls; with
`ST_VIDEO_DEBUG=1` the macOS player also logs main-thread work that took a
millisecond or more, and `ST_GRAPHICS_DEBUG=1` logs graphics commands that
took 2 ms or more.

The platform registers a `VideoBackend` with `graphics_set_video_backend()`.
Without one (the non-GPU X11 path, tests that don't register one), `k=c`
answers `ENOSYS` and opens fail. All backend calls are made on the main
thread. A backend may decode and play on its own threads, and wakes the main
loop when a frame becomes due.

## Compatibility with raw-frame players

Players that stream uncompressed frames as images (`mpv --vo=kitty`, `timg`)
send every frame as a new anonymous image placed over the previous one. `st`
removes an older placement of an anonymous image when a newer opaque image
placement at the same anchor covers it completely. The image memory stays at
about one frame instead of growing until the 320 MiB quota or the
1,024-placement limit stops the video. Graphics APC payloads are also copied
in bulk instead of byte by byte. These players still need a fast PTY: a
720p frame is about 3.7 MB of base64, so over SSH use `st-play`.
