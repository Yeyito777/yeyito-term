# Command-line mode — vim-like `:` command interface

st provides a vim-like command-line overlay activated by pressing `:` while in nav mode. It implements three editing modes (insert, normal, visual), operator-pending text objects, command history, and clipboard integration. The overlay is an X11 child window drawn at the bottom of the terminal, following the same pattern as sshind.c and notif.c.

## Architecture

The command-line is a self-contained X11 child window that sits over the last row of the terminal. All rendering, state management, and key handling live in `cmdline.c`. The window is created once at startup by `cmdline_init()` and mapped/unmapped as the user opens/closes the command line.

### Files

| File | Role |
|------|------|
| `cmdline.h` | Configuration constants (colors, sizes) and public API declarations |
| `cmdline.c` | Full implementation (~1300 lines): window management, rendering, modal key handling, text objects, history |
| `x.c` | Integration: init, draw, resize, key intercept, cursor hiding |
| `vimnav.c` | Entry point: `:` in nav mode calls `cmdline_open()` |
| `st.h` | Public function declarations |
| `tests/mocks.c` | `cmdline_open()` stub for test linking |

### Struct redeclaration pattern

`cmdline.c` redeclares `TermWindow`, `XWindow`, and `DC` structs from `x.c` to access their fields via `extern` globals. This is the standard suckless approach — `x.c` defines these structs locally and exports their instances, but doesn't export the type definitions in any header. Each overlay module (`sshind.c`, `notif.c`, `cmdline.c`) redeclares the structs it needs.

```c
extern XWindow xw;     // Display, Window, Colormap, Visual, screen
extern TermWindow win;  // tw, th, w, h, ch, cw (terminal/char dimensions)
extern DC dc;           // GC (for XCopyArea)
extern char *usedfont;
extern double usedfontsize;
```

## X11 window management

### Initialization (`cmdline_init`)

Called from `run()` in `x.c` after `cresize()` establishes terminal geometry. Creates:

1. **XftFont** — loaded via Fontconfig pattern matching using `usedfont` at `usedfontsize` pixel size
2. **Six XftColors** — fg, bg, err, curcolor, border, sel (allocated from cmdline.h constants)
3. **X child window** — `XCreateWindow` as child of `xw.win`, positioned at the bottom of the terminal, with `override_redirect = True` and `ExposureMask`
4. **Pixmap double buffer** — same dimensions as the child window
5. **XftDraw** — bound to the pixmap for Xft text rendering
6. **GC** — for `XCopyArea` from pixmap to window

### Geometry (`cmdline_compute_geometry`)

```c
int bpx = (win.h - win.th) / 2;  // vertical border padding
cl.y = bpx + win.th - win.ch;    // top of last terminal row
cl.width = win.w;                 // full window width
cl.height = win.h - cl.y;        // covers last row + bottom border
```

The overlay covers exactly the last terminal row plus any bottom border padding. This aligns with vim's command-line position.

### Resize (`cmdline_resize`)

Called from `resize()` in `x.c` after `notif_resize()`. Recomputes geometry and, if dimensions changed, calls `XMoveResizeWindow` and recreates the pixmap + XftDraw. Always redraws if visible.

### Map/Unmap

- `cmdline_open()` → `XMapRaised()` — brings the child window to the front
- `cmdline_close()` → `XUnmapWindow()` — hides it entirely

## State machine

### Top-level states

```
enum {
    CMDLINE_HIDDEN = 0,  // window unmapped, not intercepting keys
    CMDLINE_INPUT,       // active, accepting input
    CMDLINE_ERROR,       // showing error message, any key dismisses
};
```

Stored in `cl.state`. Transitions:

```
HIDDEN --[cmdline_open]--> INPUT
INPUT  --[execute, no match]--> ERROR
INPUT  --[Shift+Esc / backspace on empty]--> HIDDEN
ERROR  --[any key]--> HIDDEN
ERROR  --[':' from vimnav]--> INPUT (reopen in same window)
```

### Editing modes (`cl.cmd_mode`)

```
0 = insert     Bar cursor (2px wide), typing inserts at cursor
1 = normal     Block cursor with inverted character, vim motions
2 = visual     Block cursor + selection highlight, selection operations
```

Transitions:

```
[open] --> insert (0)
insert --[Esc]--> normal (cursor moves back one, vim-style)
normal --[i/a/I/A/c/C]--> insert
normal --[v]--> visual (anchor = cursor)
visual --[Esc/v]--> normal
visual --[y]--> normal (after yank)
visual --[d/x]--> normal (after delete)
visual --[c]--> insert (after change)
```

## Key handling flow

### Routing in x.c (`kpress`)

The command-line intercepts keys at the top of `kpress()`, before vimnav, shortcuts, or tty:

```c
if (cmdline_active()) {
    cmdline_handle_key(ksym, e->state, buf, len);
    return;
}
```

### Dispatcher (`cmdline_handle_key`)

1. **Modifier filter** — `IsModifierKey(ksym)` returns immediately. Bare Shift/Ctrl/Alt presses are ignored. This prevents modifier key events from clearing operator-pending state.

2. **Shift+Escape** — closes cmdline from any state (insert, normal, visual, error). Checked via `state & ShiftMask`.

3. **Error dismiss** — in `CMDLINE_ERROR` state, any non-modifier key closes the cmdline.

4. **Mode routing** — dispatches to `cmdline_handle_insert()`, `cmdline_handle_normal()`, or `cmdline_handle_visual()` based on `cl.cmd_mode`.

All handlers return 1 to indicate the key was consumed.

## Insert mode

Entry: opening cmdline, or `i`/`a`/`I`/`A`/`c`/`C` from normal mode.

| Key | Action |
|-----|--------|
| Esc | Switch to normal mode, cursor moves back one (vim behavior) |
| Enter | Execute command |
| Left/Right | UTF-8-aware cursor movement |
| Up/Down | History navigation (older/newer) |
| Home/End | Jump to start/end |
| Delete | Delete character at cursor |
| Backspace | Delete character before cursor; if input is empty, close cmdline |
| Printable chars | Insert at cursor position (supports UTF-8 multibyte) |

### Character insertion

Characters are inserted at `cl.cursor` via `memmove` to shift existing content right, then `memcpy` the new bytes. The buffer is `CMDLINE_MAX_INPUT` (256) bytes. Bytes with value < 0x20 are rejected (control characters).

## Normal mode

Entry: pressing Esc in insert mode.

### Basic motions

| Key | Action |
|-----|--------|
| h / Left | Move cursor left (UTF-8 aware) |
| l / Right | Move cursor right (clamped to last char, not past end) |
| 0 / Home | Jump to position 0 |
| $ / End | Jump to last character |
| k / Up | History: load older entry |
| j / Down | History: load newer entry |

### Editing

| Key | Action |
|-----|--------|
| x / Delete | Delete character at cursor |
| D | Delete from cursor to end of line |
| C | Delete from cursor to end, enter insert mode |
| dd | Delete entire line (operator-pending: `d` then `d`) |
| cc | Delete entire line, enter insert mode |
| yy | Yank entire line to clipboard |

### Mode switches

| Key | Action |
|-----|--------|
| i | Enter insert mode at cursor |
| a | Enter insert mode after cursor |
| I | Enter insert mode at position 0 |
| A | Enter insert mode at end |
| v | Enter visual mode (anchor = cursor) |
| Enter | Execute command |

### Operator-pending state machine

Normal mode supports vim's `{operator}{motion/textobject}` grammar via a two-level pending state:

```
cl.pending_op      = 0       (idle)
                   = 'd'     (delete pending)
                   = 'c'     (change pending)
                   = 'y'     (yank pending)

cl.pending_textobj = 0       (waiting for i/a or doubled key)
                   = 'i'     (inner text object pending)
                   = 'a'     (around text object pending)
```

Flow:
1. User presses `d` → `pending_op = 'd'`
2. User presses `i` → `pending_textobj = 'i'`
3. User presses `w` → `cmdline_find_textobj('i', 'w', &s, &e)` finds word boundaries → `cmdline_exec_op_range('d', s, e)` deletes the range
4. Both pending fields reset to 0

Doubled keys (dd, cc, yy): if the second key matches `pending_op`, the operation is applied to the entire line via `cmdline_exec_op_range(op, 0, cl.input_len)`.

Unknown keys after `pending_op` cancel the pending state.

## Visual mode

Entry: pressing `v` in normal mode (only if input is non-empty).

The selection is defined by `cl.anchor` (fixed end) and `cl.cursor` (moving end), both byte offsets. The selection is inclusive — both the anchor and cursor characters are selected.

### Keys

| Key | Action |
|-----|--------|
| Esc / v | Exit to normal mode |
| h / l / Left / Right | Move cursor (extends/shrinks selection) |
| 0 / $ / Home / End | Jump cursor |
| o | Swap anchor and cursor positions |
| y | Yank selection to clipboard, return to normal |
| d / x / Delete | Delete selection, return to normal |
| c | Delete selection, enter insert mode |
| i / a | Enter text object pending (adjusts selection to text object bounds) |

### Selection rendering

The visual selection is drawn as a filled rectangle behind the text using `cl.sel` color (`#4f5258`). Pixel bounds are computed via `XftTextExtentsUtf8` — the start pixel is the xOff of text up to `sel_start`, the end pixel is the xOff of text up to `sel_end_next` (byte past the last selected character, accounting for UTF-8 continuation bytes).

### Visual operations

`cmdline_visual_sel_range()` computes the byte range `[start, end_next)` from anchor and cursor (ordered, with end_next advanced past UTF-8 continuation bytes for inclusive selection).

- **Yank** (`cmdline_visual_yank`): allocates via `xmalloc`, copies range, calls `xsetsel()` + `xclipcopy()` for X11 clipboard
- **Delete** (`cmdline_visual_delete`): `memmove` to remove range, clamp cursor
- **Change** (`cmdline_visual_change`): delete + switch to insert mode

### Text objects in visual mode

`i`/`a` sets `cl.pending_textobj`. The next key is dispatched through `cmdline_find_textobj()` which returns a byte range. Instead of operating, visual mode adjusts the selection: `anchor = s`, `cursor = e - 1` (backed up past UTF-8 continuation bytes for correct positioning on the last character).

## Text objects

All text objects operate on `cl.input[]` with `cl.cursor` as the reference position. They return a byte range `[s, e)` where `s` is inclusive and `e` is exclusive.

### Word (`w`)

Three character categories:
1. **Word chars**: `[a-zA-Z0-9_]` (checked by `is_word_char()`)
2. **Whitespace**: space and tab
3. **Punctuation**: everything else

The object expands outward from cursor within the same category. `inner` returns just the category span. `around` additionally includes trailing whitespace (or leading, if no trailing whitespace exists).

### WORD (`W`)

Two categories: whitespace vs non-whitespace. Same expansion logic and around-whitespace behavior as word.

### Quote (`"`, `'`, `` ` ``)

Two-phase search:

1. **Enclosing search**: scan left from cursor for the quote delimiter. For each found, scan right from cursor (or cursor+1 if cursor is on the quote) for the matching close. First valid pair wins.

2. **Forward fallback**: if no enclosing pair found, scan right from cursor for the first quote. Then scan right from there for its pair. This allows `vi"` to match the next quoted string ahead of the cursor.

`inner` returns `(left+1, right)`. `around` returns `(left, right+1)`.

### Bracket (`()`, `{}`, `[]`, `<>`)

Two-phase nesting-aware search:

1. **Enclosing search**: scan left from cursor, tracking depth (increment on close bracket, decrement on open bracket). When depth reaches 0 at an open bracket, that's the left bound. Then scan right from left+1 with fresh depth tracking to find the matching close.

2. **Forward fallback**: if no enclosing pair found, scan right from cursor for the first open bracket. Then find its matching close with depth tracking.

Aliases: `b` = `()`, `B` = `{}`

`inner` returns `(left+1, right)`. `around` returns `(left, right+1)`.

### Dispatcher (`cmdline_find_textobj`)

Maps key to text object function:

| Key(s) | Object |
|---------|--------|
| w | word |
| W | WORD |
| " | double quote |
| ' | single quote |
| ` | backtick |
| ( ) b | parentheses |
| { } B | curly braces |
| [ ] | square brackets |
| < > | angle brackets |

### Operator execution (`cmdline_exec_op_range`)

Operates on byte range `[s, e)`:

- **y**: `xmalloc` + `memcpy` + `xsetsel()` + `xclipcopy()` — copies to X11 clipboard
- **d**: `memmove` to remove bytes, update `input_len`, set cursor to `s`, clamp for normal mode
- **c**: same as `d` but switches to insert mode (`cmd_mode = 0`)

## Command history

A ring buffer of `CMDLINE_HIST_MAX` (64) entries, each `CMDLINE_MAX_INPUT` (256) bytes.

### Storage

```c
char history[CMDLINE_HIST_MAX][CMDLINE_MAX_INPUT];
int hist_count;   // number of entries (0..64)
int hist_pos;     // -1 = live input, 0..count-1 = browsing history
```

### Save (`cmdline_hist_save`)

Called on every `cmdline_execute()`, regardless of whether the command succeeded or errored. If the buffer is full, entries shift down via `memmove` (oldest entry discarded).

### Browse

When the user first presses Up/k from live input (`hist_pos == -1`):
1. Live input, length, and cursor are saved to `saved_input`, `saved_len`, `saved_cursor`
2. `hist_pos` is set to `hist_count - 1` (most recent)

Subsequent Up/k decrements `hist_pos` (older). Down/j increments. When `hist_pos` returns to -1, the saved live input is restored.

In insert mode, cursor is placed at end after loading. In normal mode, cursor is clamped to last character.

## Cursor rendering

### Insert mode (bar)

A 2-pixel-wide rectangle at the cursor's pixel x-position:

```c
XftDrawRect(cl.draw, &cl.curcolor, cursor_x, cmdline_border_top, 2, cheight);
```

### Normal/Visual mode (block with inverted char)

A full-character-width rectangle, then the character under the cursor redrawn in background color:

```c
// Block
XftDrawRect(cl.draw, &cl.curcolor, cursor_x, cmdline_border_top, win.cw, cheight);
// Inverted character
XftDrawStringUtf8(cl.draw, &cl.bg, cl.font, cursor_x, ty,
                  (const FcChar8 *)cl.input + cl.cursor, charlen);
```

The character length accounts for UTF-8 multibyte by scanning past continuation bytes.

### Cursor clamping

`cmdline_clamp_normal_cursor()` ensures that in normal/visual mode, the cursor sits ON a character, never past the end. Called after any operation that might leave the cursor at `input_len`.

## UTF-8 handling

All cursor movement uses byte-level navigation with UTF-8 continuation byte detection:

```c
// Skip backward past continuation bytes (10xxxxxx)
while (cl.cursor > 0 && (cl.input[cl.cursor] & 0xC0) == 0x80)
    cl.cursor--;

// Skip forward past continuation bytes
while (cl.cursor < cl.input_len && (cl.input[cl.cursor] & 0xC0) == 0x80)
    cl.cursor++;
```

This is used in `cursor_left()`, `cursor_right()`, `delete_at_cursor()`, `cmdline_clamp_normal_cursor()`, visual selection range computation, and cursor rendering.

## Rendering pipeline (`cmdline_redraw`)

1. Clear entire pixmap with background color
2. Draw 1px top border in border color
3. Compute text origin: `tx = win.cw / 2`, `ty = border_top + font->ascent`
4. **INPUT state**:
   a. Draw `:` prefix in fg color, advance tx by one character width
   b. If visual mode: compute selection pixel bounds via `XftTextExtentsUtf8`, draw selection rectangle in sel color
   c. Draw input text in fg color
   d. Compute cursor pixel position via `XftTextExtentsUtf8` on text up to cursor offset
   e. Draw cursor (bar or block+inverted depending on mode)
5. **ERROR state**: draw error message in err color
6. `XCopyArea` from pixmap buffer to window

## Integration points in x.c

| Location | Code | Purpose |
|----------|------|---------|
| Line 70 | `#include "cmdline.h"` | Header |
| `xdrawcursor()` | `if (... \|\| cmdline_active()) return;` | Hide main terminal cursor while cmdline is active |
| `kpress()` | `if (cmdline_active()) { cmdline_handle_key(...); return; }` | Intercept all keys before vimnav/shortcuts/tty |
| `run()` | `cmdline_init()` | Create child window after `cresize()` |
| `expose()` | `cmdline_draw()` | Redraw on expose events |
| `resize()` | `cmdline_resize()` | Recompute geometry on terminal resize |

## Integration with vimnav.c

```c
extern void cmdline_open(void);

// In vimnav_handle_key():
case ':':
    cmdline_open();
    break;
```

### Altscreen guard

`cmdline_open()` blocks on altscreen unless forced nav mode is active:

```c
if (tisaltscreen() && !vimnav.forced)
    return;
```

This allows command-line access in forced nav mode (e.g., while in nvim's terminal) but prevents accidental activation during normal altscreen programs.

## Configuration (cmdline.h)

```c
static const char *cmdline_bg_color = "#00050f";      // background
static const char *cmdline_fg_color = "#f1faee";      // text
static const char *cmdline_err_color = "#febfb8";     // error messages
static const char *cmdline_cursor_color = "#48cae4";  // cursor
static const char *cmdline_border_color = "#1d3557";  // top border
static const char *cmdline_sel_color = "#4f5258";     // visual selection
static const int cmdline_border_top = 1;              // border thickness (px)

#define CMDLINE_MAX_INPUT 256    // max input buffer (bytes)
#define CMDLINE_HIST_MAX  64     // max history entries
```

Colors follow the st fork's theme (same palette as the terminal and other overlays).

## Debug mode

When `debug_mode` is set (st's `-d` flag), cmdline logs to stderr (which is redirected to `~/.runtime/st/st-<pid>/log.log` by persist):

- `cmdline: initialized (y=N, WxH)` — after init
- `cmdline: opened` / `cmdline: closed` — open/close
- `cmdline: input='...' cursor=N` — on character insertion
- `cmdline: insert -> normal` / `cmdline: normal -> insert` — mode switches
- `cmdline: execute '...'` — command execution
- `cmdline: history saved '...' (count=N)` — history save
- `cmdline: history[N]='...'` — history navigation
- `cmdline: op yank/delete/change '...'` — operator execution
- `cmdline: visual textobj anchor=N cursor=N` — text object selection
- `cmdline: yanked '...'` — visual yank
- `cmdline: resized (y=N, WxH)` — resize

## Command execution

Currently a stub — all commands show `"Not a terminal command: '<input>'"` as an error. The infrastructure is in place for a command dispatch table: `cmdline_execute()` receives the input string after saving to history. This is step 1 of the broader `?`/`/` search navigation feature, where the command-line will eventually handle search commands.

## Public API (st.h / cmdline.h)

```c
void cmdline_init(void);       // create X11 child window, load font/colors
void cmdline_open(void);       // map window, reset to insert mode
void cmdline_close(void);      // unmap window, reset state
int  cmdline_handle_key(unsigned long ksym, unsigned int state,
                        const char *buf, int len);  // process keypress, return 1 if consumed
void cmdline_draw(void);       // redraw if visible (called from expose)
void cmdline_resize(void);     // recompute geometry (called from resize)
int  cmdline_active(void);     // return 1 if state != HIDDEN
```

## Build

```makefile
SRC = st.c x.c vimnav.c sshind.c notif.c persist.c cmdline.c

x.o: arg.h config.h st.h win.h sshind.h notif.h persist.h cmdline.h
cmdline.o: cmdline.h vimnav.h
```

`cmdline.o` depends on `vimnav.h` for the `vimnav` struct (needed to check `vimnav.forced` in `cmdline_open()`).

## Test stub

`tests/mocks.c` provides a `cmdline_open()` stub so that `vimnav.c` (which calls `cmdline_open()`) can link in the test binary without pulling in X11 dependencies:

```c
void cmdline_open(void) { /* Stub for cmdline open */ }
```
