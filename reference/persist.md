# Persistence — dwm restart survival

st saves terminal state (scrollback history, screen content, CWD) to disk so dwm can restore it after a restart. This integrates with dwm's `--persist` mode via the `_DWM_SAVE_ARGV` X11 property.

## Runtime directory

Each st instance writes to `~/.runtime/st/st-<pid>/`:

```
~/.runtime/st/st-12345/
    scrollback-history.save   # binary: history + screen buffers
    generic-data.save         # text: key=value metadata (cwd, future fields)
    log.log                   # stderr redirect (write-only debug log)
```

Created by `persist_init()`. Deleted by `persist_cleanup()` when the shell exits normally.

## CLI

```
st --from-save ~/.runtime/st/st-12345/
```

Parsed with `strcmp` before `ARGBEGIN` in `x.c` (arg.h only handles single-char flags — same pattern as dwm's `--persist`). The `--from-save` arg and its value are stripped from argv via `memmove` before normal option parsing proceeds.

## DWM registration

After X11 window creation, st sets the `_DWM_SAVE_ARGV` property on its window:

```
_DWM_SAVE_ARGV = "st --from-save /home/yeyito/.runtime/st/st-<pid>/"
```

Done via `XChangeProperty` in `xsetdwmsaveargv()` (`x.c`), called from `persist_register()`. dwm reads this property when saving window state during a restart.

## File formats

### scrollback-history.save (binary)

```
PersistHeader (20 bytes):
    magic:   "STHIST"  (6 bytes, not null-terminated)
    version: uint16_t  (currently 1)
    col:     uint16_t  (terminal columns at save time)
    row:     uint16_t  (terminal rows at save time)
    histi:   uint16_t  (circular buffer write position)
    histn:   uint16_t  (number of valid history lines, max HISTSIZE)
    pad:     uint16_t  (alignment padding, zero)

History section (histn lines):
    Each line: col * sizeof(Glyph) raw bytes
    Written oldest-to-newest from the circular buffer:
        oldest = (histi - histn + 1 + HISTSIZE) % HISTSIZE
        for i in 0..histn-1: write hist[(oldest + i) % HISTSIZE]

Screen section (row lines):
    Each line: col * sizeof(Glyph) raw bytes
    Saves the main screen buffer:
        MODE_ALTSCREEN set → term.alt (holds main while alt is active)
        Otherwise → term.line
```

Glyph layout (from st.h): `{ Rune u, ushort mode, uint32_t fg, uint32_t bg }`.

### generic-data.save (text, line-oriented key=value)

```
cwd=/home/yeyito/some/project
cursor_y=23
```

The parser skips unknown keys, so new fields are forward-compatible.

### log.log

`persist_init()` redirects stderr to this file via `dup2()`. All `fprintf(stderr, ...)` calls throughout st go here. Never read back by st.

## History line counting

The circular buffer `term.hist[HISTSIZE]` (HISTSIZE = 32768) has no built-in count of valid entries. The field `term.histn` (added at the END of the Term struct for ABI compatibility with vimnav.c's duplicated struct) tracks this:

- Incremented in `tscrollup()` when `copyhist` is true: `if (term.histn < HISTSIZE) term.histn++`
- Reset in `treset()`: `term.histn = 0`
- Tells persist how many history lines to actually write (avoids dumping 32768 empty lines)

## CWD tracking

The shell reports CWD via OSC 779. Previously this only set the `_ST_CWD` X11 property. Now the OSC 779 handler also calls `persist_set_cwd()`, which stores the path in a static `PATH_MAX` buffer inside `persist.c`. On restore, CWD is read from `generic-data.save` and `execsh()` calls `chdir()` before `execvp()`.

## Periodic save timer

In `run()` (`x.c`), alongside blink and notification timeout logic:

```c
if (persist_active()) {
    double persist_remain = persistinterval - TIMEDIFF(now, lastpersist);
    if (persist_remain <= 0) {
        persist_save();
        lastpersist = now;
        persist_remain = persistinterval;
    }
    if (timeout < 0 || persist_remain < timeout)
        timeout = persist_remain;
}
```

`persistinterval` = 30000ms (30 seconds), defined in `config.h`. Writes are non-atomic.

## Startup flows

### Normal startup

```
main() → tnew() → xinit() → xsetenv()
       → persist_init(getpid())       # mkdir runtime dir, redirect stderr
       → persist_register()           # XChangeProperty _DWM_SAVE_ARGV
       → signal(SIGTERM, sigterm)     # install save-on-kill handler
       → run()                        # event loop with periodic saves
```

### Restore startup (--from-save)

```
main() → tnew(default cols, rows)
       → persist_restore(dir)         # read generic-data → set CWD
       │   ├── read scrollback header, validate magic/version
       │   ├── tresize() if saved dimensions differ
       │   ├── copy history lines → term.hist[0..histn-1]
       │   ├── copy screen lines → term.line[]
       │   ├── set term.histi, term.histn, term.scr = 0
       │   ├── restore term.c.y from cursor_y, set term.c.x = 0
       │   ├── tfulldirt()
       │   └── rmdir_recursive(dir)   # consume the save directory
       → xinit() → xsetenv()
       → persist_init(getpid())       # NEW runtime dir (new PID)
       → persist_register()           # register new DWM argv
       → run()                        # shell spawns with chdir(persist_get_cwd())
```

On restore, `persist_restore()` deletes the consumed save directory after reading it.

## Exit behavior

| Exit path | Save? | Cleanup dir? | Why |
|-----------|-------|-------------|-----|
| `sigchld` — shell exited | Yes | Yes | Terminal closing normally, dir not needed |
| `ttyread` — EOF | Yes | Yes | Shell closed pty, terminal closing |
| `cmessage` — WM_DELETE_WINDOW | Yes | Yes | WM requested close (e.g. Alt+Shift+C), uses `_exit()` to avoid SIGCHLD race |
| `die()` — X error | Yes | No | Likely dwm restart, dir needed for restore |
| SIGTERM handler | Yes | No | External kill / dwm restart |
| SIGKILL / crash | No | No | Last periodic save used (at most 30s stale) |

`persist_save()` is called first in `sigchld()`, `ttyread()` EOF, and `cmessage()` WM_DELETE_WINDOW, followed by `persist_cleanup()`. The WM_DELETE_WINDOW path uses `_exit(0)` instead of `exit(0)` to prevent a deadlock: `ttyhangup()` sends SIGHUP which can kill the child before exit completes, and `sigchld`'s `die()` calling `exit()` from within `exit()` deadlocks on glibc's exit lock. In `die()`, only `persist_save()` runs (no cleanup — the directory must survive for dwm to re-launch st from it).

## API (persist.h)

```c
void persist_init(pid_t pid);          // mkdir runtime dir, redirect stderr, set initialized
void persist_register(void);           // XChangeProperty _DWM_SAVE_ARGV
void persist_save(void);               // write scrollback + generic data files
void persist_restore(const char *dir); // read saved state, populate term buffers, rm dir
void persist_cleanup(void);            // rm -rf runtime dir
int  persist_active(void);             // whether persist_init() was called
void persist_set_cwd(const char *cwd); // store CWD in memory (NULL clears)
const char *persist_get_cwd(void);     // retrieve stored CWD (empty string if unset)
const char *persist_get_dir(void);     // return runtime dir path
```

## Suckless struct duplication

`persist.c` duplicates the `Term` struct, `TCursor`, `HISTSIZE`, `IS_SET`, and `MODE_ALTSCREEN` from `st.c` (these are not exported in `st.h`). This follows the same pattern as `vimnav.c`. The `term` global is declared `extern Term term` and resolved at link time against `st.c`'s definition.

`term.histn` was placed at the END of the Term struct specifically so that modules duplicating the struct without the field (like vimnav.c) don't have their field offsets shifted — trailing unknown fields are harmless.

## Testing

`make test_persist` — 7 tests covering CWD tracking, full save/restore roundtrip, empty history, bad magic rejection, and DWM registration. The test compiles `persist.c` separately into `tests/persist.o` and links with `tests/test_persist.o` (which provides its own Term definition and mock functions).
