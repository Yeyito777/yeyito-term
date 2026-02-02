# Line Highlight

The line highlight feature visually indicates which line the cursor is on while in vim nav mode. It changes the background color of the entire line to a deep navy blue (`#090d35`), helping users track their position when navigating through terminal history.

The highlight only appears when:
- In nav mode normal state (not visual mode)
- Cursor is in history (not in prompt space)
- zsh is not in visual selection mode

## Relevant Files and Functions

### config.h / config.def.h

| Item | Line | Description |
|------|------|-------------|
| `"#090d35"` | 128 | Color definition for the line highlight (index 261 in color palette) |
| `vimnav_curline_bg` | 141 | Variable holding the color index (261) used for highlighting |

### vimnav.c

| Function | Line | Description |
|----------|------|-------------|
| `vimnav_curline_y()` | 222 | Returns the screen row to highlight, or -1 if no highlight should be shown. Checks: must be in VIMNAV_NORMAL mode, not in prompt space, and zsh not in visual mode |

### vimnav.h

| Declaration | Line | Description |
|-------------|------|-------------|
| `vimnav_curline_y()` | 46 | Public declaration of the function that returns which line to highlight |

### x.c

| Code | Line | Description |
|------|------|-------------|
| Highlight application | 1466-1467 | During glyph rendering, if `y == vimnav_curline_y()` and the glyph has default background, override background with `vimnav_curline_bg` color |

## Flow

1. During screen redraw, `x.c` calls `vimnav_curline_y()` for each row
2. `vimnav_curline_y()` returns `vimnav.y` (current cursor row) if conditions are met, otherwise -1
3. If the row matches and glyph has default background, the highlight color is applied
4. Glyphs with custom backgrounds (from programs like fastfetch) are preserved
