# Ctrl+Number Row Function Keys

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
