# Debug Mode

Debug mode is a diagnostic overlay activated by starting st with the `-d` flag (`./st -d`). It visually highlights the prompt line(s) that st detects, making it easy to verify prompt-space detection at a glance.

When active:
- All prompt lines get a subtle dark yellow background tinge (`#1a1a00`)
- The text `"prompt line"` appears in golden reflection (`#ffe066`) 5 spaces to the right of any content on those lines, like an LSP inline hint in an IDE
- Only glyphs with the default background are tinted — custom backgrounds (from programs like fastfetch, colored output, etc.) are preserved
- The highlight disappears when scrolled up into history or on the alt screen, since the prompt is no longer visible

When st is started without `-d`, none of this code has any effect.

## Relevant Files and Functions

### config.h / config.def.h

| Item | Line | Description |
|------|------|-------------|
| `"#1a1a00"` | 129 | Color definition for debug prompt background (index 262, dark yellow tinge) |
| `"#ffe066"` | 130 | Color definition for debug prompt text (index 263, golden reflection) |
| `debug_prompt_bg` | 144 | Variable holding the background color index (262) |
| `debug_prompt_fg` | 145 | Variable holding the text color index (263) |

### st.h

| Item | Line | Description |
|------|------|-------------|
| `extern int debug_mode` | 150 | Declaration of the global debug mode flag |

### st.c

| Item | Line | Description |
|------|------|-------------|
| `int debug_mode = 0` | 239 | Definition of the debug mode flag, off by default |
| `tfulldirt()` in `draw()` | 2831-2833 | When `debug_mode` is on, forces all lines dirty before every redraw. Ensures overlays clean up immediately when the prompt moves — no stale highlights left on old lines. Acceptable cost since `draw()` is event-driven and debug mode is a diagnostic tool |

### vimnav.h

| Declaration | Line | Description |
|-------------|------|-------------|
| `vimnav_prompt_line_range()` | 49 | Public function returning the screen row range of the prompt space |

### vimnav.c

| Function | Line | Description |
|----------|------|-------------|
| `vimnav_prompt_line_range()` | 248 | Returns prompt row range via `start_y`/`end_y` output params. Sets both to -1 if on alt screen or scrolled. Otherwise delegates to the existing static `vimnav_find_prompt_start_y()` for `start_y` and uses `term.c.y` for `end_y` |

### x.c

| Code | Line | Description |
|------|------|-------------|
| `-d` flag parsing | 2236 | Sets `debug_mode = 1` in the `ARGBEGIN` block |
| Background tint | 1469-1475 | In `xdrawglyphfontspecs()`, after the vimnav curline highlight: if `debug_mode` is on and glyph has default background, checks `vimnav_prompt_line_range()` and overrides `bg` to `debug_prompt_bg` |
| Inline hint text | 1729-1749 | In `xdrawline()`, after all glyphs are rendered: if `debug_mode` is on and the row is a prompt line, draws `" prompt line"` using `XftDrawStringUtf8` with `debug_prompt_fg` color and `dc.font.match` font. Background behind the text is filled with `debug_prompt_bg`. Only drawn if it fits within the terminal width |

### tests/test_vimnav.c

| Test | Description |
|------|-------------|
| `prompt_range_at_bottom` | With prompt at row 23 and no scroll, returns start=23, end=23 |
| `prompt_range_scrolled` | When `term.scr > 0`, returns -1/-1 |
| `prompt_range_altscreen` | When `MODE_ALTSCREEN` is set, returns -1/-1 |

## Flow

1. User starts `st -d` — `main()` in x.c sets `debug_mode = 1`
2. On each `draw()` call, `tfulldirt()` marks all lines dirty so overlays never go stale
3. `drawregion()` calls `xdrawline()` for each dirty row (all of them in debug mode)
3. Inside `xdrawline()`, glyphs are grouped by attribute and passed to `xdrawglyphfontspecs()`
4. `xdrawglyphfontspecs()` checks `debug_mode` — if on, calls `vimnav_prompt_line_range()` to get the prompt row range
5. If the current row is within the prompt range and has default background, the background color pointer is overridden to `debug_prompt_bg` (dark yellow tinge)
6. After all glyph groups are drawn, `xdrawline()` checks `debug_mode` again for the inline text overlay
7. If the row is a prompt line, it calculates the text position using `tlinelen()` and draws `"prompt line"` in golden reflection after the last content character
8. The 5 leading spaces in `"     prompt line"` provide a visual gap between content and the hint text
