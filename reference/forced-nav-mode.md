# Forced Nav Mode (Shift+Escape)

Forced nav mode is an st-side-only entry into vim navigation mode that bypasses all zsh coordination. It exists for situations where the normal Escape-based entry is impossible because a TUI application (Claude Code, neovim, htop, etc.) intercepts Escape and all other keybinds before zsh ever sees them.

In forced mode, st takes full control: the cursor renders as a block, prompt-space logic is disabled, and no keys are forwarded to the underlying application. The user can navigate the screen, enter visual mode, yank text, and scroll through history exactly as in regular nav mode. Editing keys (x, d, c, etc.) that would normally snap to the zsh prompt are silently ignored since there is no shell to receive them.

## Entry and Exit

**Entry** is always via Shift+Escape. There are three scenarios depending on current state:

| Current State | Shift+Escape Result |
|---|---|
| No nav mode active (e.g. inside a TUI) | Calls `vimnav_force_enter()` — fresh forced entry |
| Regular nav mode active (entered via zsh) | Sets `vimnav.forced = 1` — upgrades to forced, disabling prompt-space passthrough |
| Already in forced nav mode | Calls `vimnav_exit()` — exits completely |

**Exit** can happen via any of these keys while in forced mode:

| Key | Behavior |
|---|---|
| `i` | Exits forced nav mode (key is swallowed, not sent to app) |
| `a` | Exits forced nav mode (key is swallowed, not sent to app) |
| `Escape` | In visual mode: clears visual, stays in forced normal. In normal mode: exits forced nav mode |
| `Shift+Escape` | Exits forced nav mode (toggle off, handled in `kpress()` before `vimnav_handle_key()`) |

## Differences From Regular Nav Mode

| Behavior | Regular Nav Mode | Forced Nav Mode |
|---|---|---|
| Alt screen | Blocked (`vimnav_enter()` refuses) | Allowed (`vimnav_force_enter()` skips check) |
| Cursor position source | zsh-reported (`prompt_end + zsh_cursor`) | Terminal cursor (`term.c.x`, `term.c.y`) |
| Prompt space | Active (h/l/w/b/etc. pass through to zsh on prompt line) | Disabled (all keys handled by st) |
| Editing keys (x, d, c, ...) | Snap to prompt and pass through to zsh | Silent no-ops |
| Paste (p) | Snaps to prompt, enters zsh insert mode, pastes | Silent no-op |
| Cursor visibility | Respects TUI's DECTCEM (cursor hide) | Always visible (ignores `MODE_HIDE`) |
| Cursor shape | Whatever the app/zsh set (bar, underline, etc.) | Always block (overridden during draw) |
| Cursor color | Default cursor color (`defaultcs`) | Coral red `#ff6b6b` |
| Shell cursor sync | Active (draw syncs to `term.c.x` changes) | Disabled |
| Visual mode exit | Notifies zsh via `vimnav_notify_zsh_visual_end()` | Skips zsh notification |
| zsh cursor sync on prompt return | Active via `vimnav_sync_to_zsh_cursor()` | Skipped (prompt space returns false) |

## What Works in Forced Mode

All read-only navigation and selection operations work identically to regular nav mode:

- Movement: `h`, `j`, `k`, `l`, `0`, `$`, `w`, `b`, `e`, `W`, `B`, `E`
- Scrolling: `Ctrl+u`, `Ctrl+d`, `Ctrl+b`, `Ctrl+f`, `Ctrl+e`, `Ctrl+y`
- Screen jumps: `H`, `M`, `L`, `G`, `gg`
- Visual mode: `v`, `V`
- Text objects (in visual mode): `iw`, `aw`, `iW`, `i"`, `i(`, etc.
- Find on line: `f`, `F`, `;`, `,`
- Yank: `y` (copies selection to clipboard), `yy` (yanks current line)
- Line highlight rendering

## Relevant Files and Functions

### vimnav.h

| Item | Line | Description |
|------|------|-------------|
| `vimnav.forced` | 23 | Flag: 1 if in forced mode, 0 otherwise. Part of the `VimNav` struct |
| `vimnav_force_enter()` | 42 | Public declaration of the forced entry function |

### vimnav.c

| Function / Location | Line | Description |
|---|---|---|
| `vimnav_is_prompt_space()` | 1307 | Returns 0 immediately when `vimnav.forced` is set, disabling all prompt-space logic |
| `vimnav_force_enter()` | 1404 | Entry function. Skips alt screen check. Uses `term.c.x`/`term.c.y` for cursor position instead of zsh-reported position. Sets `vimnav.forced = 1` and mode to `VIMNAV_NORMAL` |
| `vimnav_exit()` | 1432 | Clears `vimnav.forced` along with all other state |
| `case 'i'` / `case 'a'` | 1662 | In forced mode, calls `vimnav_exit()` and breaks (swallows key). In regular mode, snaps to prompt and returns 0 (passes key to zsh) |
| Editing keys (`x`, `d`, `c`, etc.) | 1689 | In forced mode, breaks (no-op). In regular mode, snaps to prompt and returns 0 |
| `case 'p'` | 1719 | In forced mode, breaks (no-op). In regular mode, snaps to prompt and pastes |
| `case XK_Escape` | 1732-1740 | In visual mode: clears visual without notifying zsh. In forced normal mode: calls `vimnav_exit()`. In regular mode: unchanged behavior |

### x.c

| Function / Location | Line | Description |
|---|---|---|
| `MODE_HIDE` bypass in `xdrawcursor()` | 1556 | Skips the cursor-hide early return when `vimnav.forced` is set, so TUI apps that send `\033[?25l` (DECTCEM) cannot make the nav cursor invisible |
| Forced cursor color in `xdrawcursor()` | 1564-1577 | When `vimnav.forced`, overrides cursor color to coral red `#ff6b6b` (allocated once via `XftColorAllocValue`, cached in a static). Uses `TRUECOLOR()` for block cursor bg and `drawcol` for underline/bar/unfocused outline |
| `xgetcursor()` | 1782 | Returns current `win.cursor` value. Added to support save/restore of cursor style |
| Shift+Escape handler in `kpress()` | 1912-1924 | Intercepts Shift+Escape before all other key handling. Three-way branch: exit if forced, fresh-enter if inactive, upgrade if regular nav |

### st.c

| Location | Line | Description |
|---|---|---|
| Shell cursor sync skip | 2865 | The `term.c.x != vimnav.last_shell_x` sync is guarded by `!vimnav.forced`, preventing TUI cursor movements from hijacking the nav cursor |
| Block cursor override | 2879-2885 | When `vimnav.forced`, saves cursor style via `xgetcursor()`, sets to block (2) via `xsetcursor()`, draws cursor, then restores. Ensures the nav cursor is always a visible block regardless of what the TUI had set |

### win.h

| Item | Line | Description |
|------|------|-------------|
| `xgetcursor()` | 37 | Declaration of the cursor style getter, used by st.c for save/restore |

### tests/test_vimnav.c

| Test | Description |
|---|---|
| `vimnav_force_enter_works_on_altscreen` | Verifies `vimnav_force_enter()` succeeds when `MODE_ALTSCREEN` is set (where `vimnav_enter()` would refuse) |
| `vimnav_force_enter_no_double_entry` | Verifies a second `vimnav_force_enter()` is a no-op when already active |
| `vimnav_upgrade_regular_to_forced` | Verifies that setting `vimnav.forced = 1` on regular nav mode disables prompt-space passthrough (h/l handled by st instead of zsh) |
| `vimnav_forced_escape_exits` | Escape in forced normal mode exits nav mode |
| `vimnav_forced_escape_clears_visual_first` | Escape in forced visual mode clears visual first, second Escape exits |
| `vimnav_forced_i_exits` | `i` exits forced nav mode |
| `vimnav_forced_a_exits` | `a` exits forced nav mode |
| `vimnav_forced_editing_keys_noop` | All editing keys (x, X, d, D, c, C, s, S, r, R, A, I, o, O, u, ., ~, p) are consumed but do nothing |
| `vimnav_forced_navigation_works` | h, l, 0, $ all work correctly in forced mode |
| `vimnav_forced_visual_yank_works` | Visual selection and yank to clipboard work in forced mode |
| `vimnav_forced_no_prompt_space` | Lines that look like prompts ("% ...") are not treated as prompt space in forced mode |

## Flow

### Fresh entry from TUI (most common case)

```
User presses Shift+Escape in Claude Code / neovim / etc.
  → kpress() detects XK_Escape + ShiftMask
  → tisvimnav() is false
  → vimnav_force_enter() called
    → Skips MODE_ALTSCREEN check
    → Sets cursor to term.c.x, term.c.y
    → Sets vimnav.forced = 1, mode = VIMNAV_NORMAL
  → User navigates with hjkl, selects with v, yanks with y
  → draw() in st.c renders nav cursor
    → Skips shell cursor sync (forced flag)
    → Overrides win.cursor to block for the draw call
    → xdrawcursor() ignores MODE_HIDE, ensuring cursor is visible
    → xdrawcursor() uses coral red (#ff6b6b) cursor color
  → User presses i/a/Escape to exit
    → vimnav_handle_key() detects forced flag
    → Calls vimnav_exit(), key is swallowed
    → User is back in the TUI
```

### Upgrade from regular nav mode

```
User is at zsh prompt in vicmd (regular nav mode active)
  → User presses Shift+Escape
  → kpress() detects XK_Escape + ShiftMask
  → tisvimnav() is true, vimnav.forced is false
  → vimnav.forced = 1 (upgrade)
  → Prompt-space passthrough is now disabled
  → h/l/w/b/etc. all handled by st even on prompt line
  → User presses Escape to exit forced mode
    → vimnav_exit() called, fully exits nav mode
    → zsh is still in vicmd but nav mode is off
    → User presses Escape again → zsh stays in vicmd,
      zle-keymap-select fires, sends vim-mode;enter,
      regular nav mode re-enters
```
