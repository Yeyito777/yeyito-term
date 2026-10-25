# Warm spare (instant AeroSpace launches)

A cold `st` launch costs roughly half a second before its window even exists:
`NSApplication` setup, the Metal device and pipeline, CoreText font
registration and the `NSWindow` itself. AeroSpace then has to register a new
app whose main thread is still busy, which it often gives up on until
something else triggers a refresh. `st-aerospace-launch` avoids all of that
by claiming a process that has already started.

## Spare mode

`st --spare` (or `st --spare-slot PATH`) runs the normal startup up to and
including the window, then stops short of:

- ordering the window in (AeroSpace cannot see it),
- `persist_init`, `ttynew` and the PTY/timer sources (no shell yet),
- anything that depends on the command line's directory, `-e` or `-T`.

Once its run loop is serving requests it publishes itself in the slot file,
`$HOME/.runtime/st/spare` by default, as `PID WINDOW-ID STAMP`. `STAMP` is
`inode:mtime` of the executable. `link(2)` makes publishing atomic: if a
live spare already holds the slot the newcomer exits, and a slot naming a
dead process is replaced. A spare checks every minute that the slot still
names it and exits otherwise (lost race, or a launcher that died between
claiming and sending its request). It also holds an App Nap exemption while
waiting. A spare is always in managed reveal mode, and an unclaimed spare
removes its `st-PID` runtime directory on exit.

Implementation: `macos/spare.c` (request parsing, slot protocol; unit tests
in `tests/test_macos_spare.c`) and `publishSpare`/`claimSpare` in
`macos/backend.m`.

## Claiming

The launcher claims the slot with `rename(2)` (atomic, so two launchers can
never claim the same spare) and only uses it when its stamp matches the
installed binary, the process is alive and its control socket exists. An
outdated spare is killed. It then sends a `spawn` request over
`$HOME/.runtime/st/st-PID/control.sock`:

```
spawn\n
cwd=DIR\0
environ\0 NAME=VALUE\0 ... \0   (the launcher's environ, then an empty field)
env=NAME=VALUE\0               (repeatable; the launcher sends ST_INHERIT_VIRTUAL_ENV)
title=TITLE\0
arg=WORD\0                     (repeatable, in order; like st -e)
end\0
```

The `environ` block replaces the spare's environment: that is merely the
environment of whatever booted the spare (an earlier launch, perhaps from a
shell or a script), so the terminal gets the claiming launcher's instead,
exactly as if it had started st itself. A request without the final `end`
(cut short) is rejected; unknown keys are ignored. The spare applies the
environment and then what st itself exports (`LC_CTYPE`, `WINDOWID`, its
bundled `bin` first on `PATH`), changes directory (falling back to
`$HOME`), orders its window in with `orderFrontRegardless` (plain
`orderFront:` is ignored for an inactive app), then starts the shell.
Requests are accepted only once.

## Launcher flow

`st-aerospace-launch` is a small C program (`macos/aerospace_launch.c`; the
protocol and parsing helpers are in `macos/aerospace.c`, unit-tested in
`tests/test_macos_aerospace.c`). It talks to AeroSpace's server socket,
`/tmp/bobko.aerospace-$USER.sock` (`ST_AEROSPACE_SOCKET` overrides it),
itself: each `aerospace` CLI call is a Swift process costing tens of
milliseconds, and the shell launcher it replaces spent ~100 ms in such
processes and `mktemp`/`mkfifo`/`nc`/`env` before the spawn request even
reached st. Nothing is started on the warm path.

AeroSpace's protocol (`Sources/Cli/ServerConnection.swift`,
`Sources/AppBundle/server.swift` in AeroSpace): the client sends its
protocol version as a native-endian `UInt32` and reads the server's; then
every message is a native-endian `UInt32` length and JSON. A request is
`{"args":[...],"stdin":"","windowId":ID,"workspace":NAME}`, with
`AEROSPACE_WINDOW_ID`/`AEROSPACE_WORKSPACE` forwarded like the CLI does, or
explicit `null`s; the answer is `{"exitCode":N,"stdout":"...","stderr":"..."}`.
After `subscribe` a connection only streams events, so the launcher keeps
two: one subscription, one for commands. Both are opened (and the subscribe
request sent, pipelined behind the version) before anything else.

1. `subscribe focus-changed window-detected`. Its first event is the focus
   at invocation time, which gives the target workspace, and proves the
   subscription is registered before the window can appear. Without the
   server the launch fails at once, before taking the spare.
2. `--inherit-cwd`: `list-windows --focused` names the focused st's PID;
   its PTY child comes from `proc_listchildpids`, the child's directory from
   `proc_pidinfo(PROC_PIDVNODEPATHINFO)`, the venv from
   `~/.cache/st-shell-context/ST-CHILD.venv`.
3. Claim the spare. Without one, `posix_spawn` `st --spare-slot` on a
   private slot (in its own session, in `$HOME`, no inherited descriptors),
   wait for it to publish, then send `list-apps`: AeroSpace skips
   registering an app that is busy launching, and that query makes it look
   again.
4. Send `spawn` (one write on the control socket), then `poll()` the event
   stream for `window-detected` with the spare's window ID. If none
   arrives, query `list-windows --pid` after 2, 4, 8 and 16 ticks (0.5, 1,
   2 and 4 s; `ST_AEROSPACE_TICK`), giving up after 24 ticks
   (`ST_AEROSPACE_MAX_TICKS`). Every command runs a session that cancels
   AeroSpace's in-flight full refresh, where new windows are found, so
   polling faster delays detection rather than speeding it up.
5. `focus --window-id` (or `move-node-to-workspace --focus-follows-window`
   when AeroSpace placed the window on another workspace), then `SIGUSR1`.
   Any failure after the claim stops the claimed st, so no invisible window
   is left behind.
6. Boot the next spare. Once it publishes, send `list-apps` so AeroSpace
   registers it now, while it is idle. A registered app's new window is
   reported within tens of milliseconds. The window is already focused and
   revealed; only the launcher process lingers meanwhile.

`ST_AEROSPACE_TRACE=1` prints each step's time since the process started
(including whatever exec'd the launcher) to stderr.
Every launch is also logged; see [Launch log](#launch-log).

The launcher itself takes about a millisecond from `main` to the spawn
request and one round trip per AeroSpace command, so a warm launch costs
AeroSpace's detection plus its focus round trip.

With a warm spare and the earlier shell launcher a launch took ~0.2 s from
key press to a focused, revealed window (a cold launch, including AeroSpace
detection, used to take 0.7–1.4 s). The shell starts in parallel with AeroSpace's detection and
focus.

## Launch log

`~/Library/Logs/st/launch.log` gets two lines per launch, so that an
occasional slow or laggy launch can be explained afterwards. Past 1 MB (a few
thousand launches) it is renamed to `launch.log.1`, replacing the previous
one. Each line is written with one `O_APPEND` write, so concurrent writers
never interleave. Lines are `key=value`; values with spaces are
double-quoted; fields not reached are left out; times are ms with one decimal.
Both lines start with a local timestamp and `st=PID`, the claimed st, so
`grep st=PID` finds both halves of a launch.

```
2026-10-25T16:25:01.920-0400 st=42853 src=launcher result=ok path=warm spare_age=10.3 cmd=shell main=23.1 connected=180.7 claimed=182.1 target=183.6 spawn=183.7 detected=445.7 via=event focused=453.4 revealed=453.4 load1=3.70
2026-10-25T16:25:02.119-0400 st=42853 src=st cmd=zsh spare_age=10.3 handled=4.8 ordered=33.4 started=50.2 revealed=448.3 output=691.8 frame=187.5 presented=721.4 content=697.4 frames=9 max_frame=86.9 slow=1 pageins=154 faults=1257 cpu=166 load1=3.48
```

`src=launcher` (`macos/aerospace_launch.c`) is written once per launch,
successful or not (usage errors aside), after the `SIGUSR1` reveal and before
the next spare is booted; nothing but timestamps is taken on the way. Its
timestamp is the launcher's exec, and its times count from there:

| Field | Meaning |
|-------|---------|
| `result` | `ok` or `fail` (then `error="..."`, the message printed on stderr, comes last) |
| `path` | `warm` (claimed the shared spare) or `cold` (booted a private one) |
| `reason` | why cold: `no-spare`, `bad-slot`, `dead` (its pid is gone), `outdated` (stamp differs from the binary), `no-socket` |
| `spare_age` | s since the claimed spare published its slot (the slot file's mtime); warm only |
| `cmd` | basename of `-e COMMAND`, or `shell` |
| `main` | the launcher's `main` (exec and dyld; slow under memory pressure) |
| `connected` | both AeroSpace connections open |
| `claimed` | spare claimed; cold: the private spare published itself (st boot) |
| `target` | AeroSpace's first answer: the focused workspace is known |
| `spawn` | spawn request written to st |
| `detected`, `via` | AeroSpace reported the window: `event` (`window-detected`) or `query` (fallback `list-windows`) |
| `focused` | `focus`/`move-node-to-workspace` answered; `moved=1` if AeroSpace placed it on another workspace |
| `revealed` | `SIGUSR1` sent |
| `load1` | 1-minute load average |

`src=st` (`launchStats` in `macos/backend.m`) is written by every claimed
spare, on either launcher path, 3 s after the claim (or at exit, with
`exited=1`, if that comes first). A plain `st` started by hand logs nothing.
Its timestamp is when the spawn request arrived, and its times count from
there; until the line is written only timestamps and a few counters in
`drawInMTKView` are kept:

| Field | Meaning |
|-------|---------|
| `cmd` | the child's command name (`zsh`, `exocortex`, ...) |
| `spare_age` | s since this spare published itself |
| `handled` | the main thread picked up the request (a busy or paged-out main thread shows here) |
| `ordered` | window ordered in |
| `started` | PTY and child started |
| `revealed` | window made opaque: `SIGUSR1` received and the window is key |
| `output` | first bytes from the PTY: the shell's or TUI's own startup time |
| `frame` | first frame committed (usually before the reveal, so not shown) |
| `presented` | first frame actually shown on screen (Metal `presentedTime`) |
| `content` | first frame committed after the first output |
| `frames`, `max_frame`, `slow` | frames drawn in the 3 s, the longest `draw()` in ms (including the wait for a drawable), and how many took over 33 ms |
| `pageins`, `faults` | major (page-in) and minor faults of st during the 3 s (`getrusage`) |
| `cpu` | ms of CPU st used during the 3 s |
| `load1` | 1-minute load average |

Reading it: a late `output` with a normal `content - output` is the child
starting slowly (zsh config, the TUI); a late `handled`, `ordered` or
`frame`, a high `max_frame`/`slow` or many `pageins` point at st itself
(paged out after a long idle, cold GPU resources); a late `detected` or
`focused` with normal st times points at AeroSpace; `main`/`claimed` high
together with `load1` points at a loaded machine.

