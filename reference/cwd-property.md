# CWD Property (`_ST_CWD`)

st publishes the shell's current working directory as an `_ST_CWD` X11 window property (UTF8_STRING) on its own window. The property is updated in real-time via OSC 779, sent by a `chpwd` hook in zsh. Any X client can read the property with a single `XGetWindowProperty` call — no `/proc` traversal, no PID resolution, no child-process guessing.

## Protocol

### OSC 779

zsh sends the CWD to st using a custom OSC escape sequence:

```
\033]779;/absolute/path\a
```

st parses this in `strhandle()`, extracts the path from `strescseq.args[1]`, and calls `xsetcwd()` to set the X property.

### X11 property

| Property | Type | Format | Set by | Content |
|----------|------|--------|--------|---------|
| `_ST_CWD` | `UTF8_STRING` | 8-bit | `xsetcwd()` via `XChangeProperty` | Absolute path, no trailing slash (except root `/`) |

The property persists on the window until overwritten by the next `chpwd` or the window is destroyed. It is always the latest value — there is no history.

### Reading the property (consumer side)

```c
Atom prop = XInternAtom(dpy, "_ST_CWD", True);
if (prop != None) {
    Atom type;
    int format;
    unsigned long nitems, remain;
    unsigned char *data = NULL;

    XGetWindowProperty(dpy, win, prop, 0, 1024, False,
                       XInternAtom(dpy, "UTF8_STRING", False),
                       &type, &format, &nitems, &remain, &data);
    if (data && nitems > 0) {
        /* data is the path, nitems is the length */
        chdir((char *)data);
    }
    if (data) XFree(data);
}
```

From the shell:

```bash
xprop -id <window-id> _ST_CWD
```

### Validation

st does not validate the path. It stores exactly what zsh sends. If zsh sends garbage, the property will contain garbage. The `chpwd` hook uses `$PWD` which is always an absolute resolved path maintained by the kernel.

## zsh Configuration

The `chpwd` hook in `.zshrc` fires on every directory change, including `cd`, `pushd`, `popd`, and zoxide's `z`:

```zsh
function chpwd {
  printf '\033]779;%s\a' "$PWD"
}
chpwd  # report initial directory on shell startup
```

The bare `chpwd` call after the function definition ensures the initial working directory is reported when the shell starts (since `chpwd` only fires on *changes*, not on init).

## Debug Mode

When st is started with `-d`, the OSC 779 handler logs to stderr:

```
OSC 779: cwd=/home/user/projects
OSC 779: cwd=/tmp
```

## Relevant Files and Functions

### st.c

| Item | Line | Description |
|------|------|-------------|
| `case 779` | 2119 | OSC 779 handler in `strhandle()`. Guards on `narg > 1` and non-empty path. Calls `xsetcwd()` |
| `fprintf` (debug) | 2122 | Logs `OSC 779: cwd=<path>` to stderr when `debug_mode` is on |

### x.c

| Item | Line | Description |
|------|------|-------------|
| `stcwd` in `XWindow` struct | 100 | Atom field for the `_ST_CWD` property |
| `XInternAtom` in `xinit()` | 1250 | Registers `_ST_CWD` atom at window creation |
| `xsetcwd()` | 1702 | Calls `XChangeProperty` with `PropModeReplace`, type `UTF8_STRING`, format 8, data = raw path bytes, length = `strlen(cwd)` |

### win.h

| Item | Line | Description |
|------|------|-------------|
| `xsetcwd()` | 37 | Public declaration |

### Struct layout constraint

The `stcwd` atom field sits between `netwmpid` and `stnotify` in the `XWindow` struct (x.c:100). The local `XWindow` definitions in `sshind.c`, `notif.c`, `tests/test_sshind.c`, and `tests/test_notif.c` must include this field at the same position, or all subsequent fields will be at wrong offsets. See `reference/notifications.md` § Struct Layout Constraint.

### tests/test_cwd.c

| Test | Description |
|------|-------------|
| `cwd_basic` | Standard path sets property |
| `cwd_spaces` | Paths with spaces are preserved verbatim |
| `cwd_root` | Root path `/` works |
| `cwd_empty_ignored` | Empty string does not call `xsetcwd()` |
| `cwd_missing_arg` | Missing OSC argument does not call `xsetcwd()` |
| `cwd_wrong_osc` | Different OSC number (778) does not trigger CWD logic |
| `cwd_multiple_updates` | Successive updates each call `xsetcwd()`; last value wins |

## Flow

```
Shell (zsh)
  chpwd fires on directory change
  printf '\033]779;/new/path\a' writes OSC 779 to the pty

st (st.c)
  ttyread() reads pty data into the escape sequence parser
  Parser recognizes OSC (']') and collects args split by ';'
    args[0] = "779", args[1] = "/new/path"
  strhandle() dispatches on par=779
  Guard: narg > 1 && args[1][0] != '\0'
  If debug_mode: fprintf(stderr, "OSC 779: cwd=%s\n", args[1])
  Calls xsetcwd(args[1])

st (x.c)
  xsetcwd() calls XChangeProperty:
    property = xw.stcwd (interned as "_ST_CWD")
    type     = UTF8_STRING
    format   = 8
    mode     = PropModeReplace
    data     = raw path bytes
    length   = strlen(cwd)
  Property is now readable by any X client

External program (e.g. dwm)
  XGetWindowProperty(dpy, win, XInternAtom(dpy, "_ST_CWD", True), ...)
  Gets the path as a byte string
  chdir(path) + execvp("st", ...) to spawn a new terminal there
```


-- USER (HUMAN) FACING:
Possible problems with this is:
Read reference/cwd-property.md.
Then address this:
Run-and-close st windows that run a command like this:
st -e zsh -ic agent

Don't properly work with cwd and property setting if the underlying command uses cd.
Is this a limitation? Is there a way we can tackle this?

What do you think?
