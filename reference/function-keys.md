# Ctrl+Number Row Function Keys

## macOS desktop symbol shortcuts

The user's AeroSpace `ctrl-1`, `ctrl-2`, and `ctrl-3` bindings override the
terminal-local table below: **•, ←, →**, respectively. They invoke
`~/.config/aerospace/type-symbol.sh`, which validates the symbol and runs
`type-symbol-native` beside it. The native helper's source is
`scripts/macos-type-symbol.m`; rebuild it with:

```
clang -O2 -fobjc-arc -framework Cocoa scripts/macos-type-symbol.m \
  -o ~/.config/aerospace/type-symbol-native
```

The helper posts explicit Unicode events with no modifiers to the frontmost
application's PID, without using the clipboard or changing focus. Unlike
System Events `keystroke`, it does not translate Unicode into layout-dependent
physical keys. Its unmapped virtual key also avoids a spurious `a` key-release.
st receives ordinary Unicode text (or its Unicode Kitty keyboard encoding),
not Ctrl+number/F-key events, including in an alternate-screen TUI. This path
is intended for already-running native st processes without a terminal rebuild;
the isolated tests are not proof of delivery through the desktop shortcut.
Accessibility event-posting permission is required; the helper fails without
opening a permission dialog if it is unavailable.

These three desktop shortcuts intentionally take precedence over the table's
nav jumps/F14–F16 mappings. No terminal-local binding, other modifier, Ctrl
editing shortcut, or workspace binding is changed. The tests in
`tests/test_macos_symbol_input.m` exercise the actual native input handler and
post events only to their own hidden test process.

Posting is asynchronous. The helper keeps its event source and run loop alive
for a bounded 500 ms handoff interval **after** posting; there is no pre-input
delay. Returning immediately could discard both events before the recipient
received them. The regression test launches short-lived helper children and
uses their received events through both Kitty dispatch and the real
`interpretKeyEvents:`/`insertText:` shell path.

For non-typing permission diagnostics, run `type-symbol.sh --probe`.
The native helper also supports an explicit `--trace SYMBOL` diagnostic for
an authorized user invocation: it records invocation, posting permission,
target PID/bundle, and a target-process event tap filtered to that helper's
own tagged events. It never records unrelated keyboard input. Tracing is off
for normal shortcuts.

## Terminal-local bindings (without the desktop override)

The number row (1-9, 0, -) is repurposed under Ctrl to serve three roles depending on context: nav mode screen jumps, direct Unicode character insertion in the shell, and F-key escape sequences for alt screen programs.

## Key Mapping

| Key | F-Key | Escape Sequence | Shell Insert | Nav Mode |
|-----|-------|-----------------|--------------|----------|
| Ctrl+1 | F14 | `\033[1;2Q` | ← (U+2190) | 0% screen |
| Ctrl+2 | F15 | `\033[1;2R` | • (U+2022) | 10% screen |
| Ctrl+3 | F16 | `\033[1;2S` | → (U+2192) | 20% screen |
| Ctrl+4 | F17 | `\033[15;2~` | `<F17>` | 30% screen |
| Ctrl+5 | F18 | `\033[17;2~` | `<F18>` | 40% screen |
| Ctrl+6 | F19 | `\033[18;2~` | `<F19>` | 50% screen |
| Ctrl+7 | F20 | `\033[19;2~` | `<F20>` | 60% screen |
| Ctrl+8 | F21 | `\033[20;2~` | `<F21>` | 70% screen |
| Ctrl+9 | F22 | `\033[21;2~` | … (U+2026) | 80% screen |
| Ctrl+0 | F23 | `\033[23;2~` | – (U+2013) | 90% screen |
| Ctrl+- | F24 | `\033[24;2~` | — (U+2014) | 100% screen |

The escape sequences are Shift+F1 through Shift+F12 in standard xterm encoding (the `;2` modifier means Shift).

## Behavior by Context

### Nav mode (vimnav active)

Handled by `vimnav_handle_key()` in vimnav.c. Ctrl+1-9,0,- call `vimnav_move_screen_percent()` to jump the cursor to a vertical screen percentage. This takes priority because nav mode key handling runs before everything else in `kpress()`.

### Shell (not alt screen, not nav mode)

Handled by the Ctrl+number row block in `kpress()` in x.c. When `!tisaltscreen()`, the keys write UTF-8 characters directly via `ttywrite()` instead of falling through to `kmap()`. Keys with Unicode bindings (Ctrl+1-3, 9, 0, -) insert the character. Unbound keys (Ctrl+4-8) insert literal `<Fnum>` text.

This intercepts the keys before `kmap()` reaches the config.h `key[]` table, so the F-key escape sequences are never sent to the shell.

### Alt screen (neovim, vim, less, etc.)

The x.c block is skipped (`tisaltscreen()` is true), so the keys fall through to `kmap()` which matches them against the config.h `key[]` table and sends F14-F24 escape sequences. Programs can bind these freely.

## Processing Pipeline

```
kpress() in x.c
  │
  ├─ Nav mode? (tisvimnav())
  │   └─ Yes → vimnav_handle_key() → Ctrl+1-9,0,- → screen percentage jump
  │
  ├─ Not alt screen? (!tisaltscreen()) && ControlMask
  │   └─ Yes → ttywrite() Unicode char or <Fnum> literal → return
  │
  ├─ shortcuts[] lookup
  │
  └─ kmap() → config.h key[] table
      └─ Ctrl+1-9,0,- → F14-F24 escape sequences (alt screen path)
```

## Relevant Files and Locations

### config.h

| Item | Line | Description |
|------|------|-------------|
| `mappedkeys[]` | 243-246 | Registers XK_0-9 and XK_minus so `kmap()` processes them |
| `key[]` Ctrl+number entries | 260-271 | Maps Ctrl+1-9,0,- to F14-F24 escape sequences (used in alt screen) |

### x.c

| Item | Line | Description |
|------|------|-------------|
| Ctrl+number row block in `kpress()` | 1965-1985 | Shell-mode handler: writes UTF-8 chars directly when not in alt screen |

### vimnav.c

| Item | Line | Description |
|------|------|-------------|
| `vimnav_move_screen_percent()` | 1320-1342 | Jumps cursor to a vertical percentage of the screen |
| Ctrl+1-9,0,- cases | 1721-1731 | Nav mode dispatch to `vimnav_move_screen_percent()` |
