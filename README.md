# Building

On Linux, install the X11, Xft, Fontconfig, FreeType, and OpenGL development
packages for your distribution, then run:

```sh
make clean
make
```

On macOS, `st` uses a native AppKit window, CoreText font shaping, and a Metal
GPU renderer. XQuartz is not used or required. Xcode's command-line tools are
the only build dependency:

The native backend defaults to the same typography as the local Terminal
`Basic` profile: SF Mono Regular at 11 points with Terminal's standard cell
spacing. Passing `-f` still overrides the native font for an individual launch.

```sh
make clean
make
```

To create a signed macOS application bundle and install it for the current
user, run:

```sh
make install-app
```

The app is installed at `~/Applications/st.app`, appears in Spotlight as `st`,
uses an ordinary macOS window that tiling managers can control, and bundles the
`st-notify`, `st-save-cmd`, and `st-aerospace-launch` helper scripts.

## Inline images, including over SSH

`st` implements the direct-stream subset of the Kitty terminal graphics
protocol. PNG, RGB, and RGBA images can be transmitted in APC escape sequences,
split into 4096-byte base64 chunks, and optionally compressed with zlib. Images
support source cropping, cell scaling, alpha, z-order, image and placement IDs,
deletion, cursor movement control, the protocol capability query, and terminal
scrollback. Both the native Metal backend and the X11 OpenGL backend render the
same backend-neutral image state.

Direct transmission is intentional: unlike file and shared-memory references,
it names no resources on the machine running the terminal and therefore works
unchanged through SSH. In an interactive remote shell, for example:

```sh
kitten icat --transfer-mode=stream image.png
chafa --format kitty image.png
```

Applications should use the Kitty graphics query rather than infer support from
`$TERM`; the terminal continues to identify as `st-256color`. The PTY reports
both cell and pixel dimensions, and `CSI 14 t` / `CSI 16 t` are available as
geometry-query fallbacks.

The current implementation does not accept local file, temporary-file, or
shared-memory image transfers. Animations, relative placements, and Kitty
Unicode placeholders are also not yet implemented. Kitty graphics can cross
tmux with explicitly enabled passthrough, but tmux does not natively preserve
these placements; use a direct SSH shell for the supported path.

See [Kitty graphics architecture](docs/kitty-graphics.md) for the protocol
subset, data flow, renderer integration, scrollback model, and safety limits.

## AeroSpace launch integration

To open `st` on the currently focused AeroSpace workspace, bind a key to the
bundled launcher. Replace `/Users/you` with your home directory because
AeroSpace bindings do not expand `~` in executable paths:

```toml
alt-shift-enter = 'exec-and-forget /Users/you/Applications/st.app/Contents/Resources/bin/st-aerospace-launch'
```

The helper records the new `st` process ID and asks AeroSpace for the window
owned by that exact process before moving and focusing it. This remains
deterministic when the key is pressed repeatedly: overlapping launches cannot
mistake another terminal's window for their own. An `st` started by this helper
also remains transparent until AeroSpace has completed its layout and native
focus pass, so its initial centered frame is never shown before the tiled one.
Direct launches keep the normal standalone reveal behavior.

# Zsh integration

To make this terminal work well with zsh, put this in your `.zshrc`:

```zsh
bindkey -v
bindkey -M viins '^?' backward-delete-char
bindkey -M viins '^H' backward-delete-char
KEYTIMEOUT=1

# Report cursor position to st (for cursor sync)
function _st_report_cursor {
  printf '\033]777;cursor;%d\a' "$CURSOR"
}

# Report visual mode state to st
function _st_report_visual {
  if [[ $REGION_ACTIVE -eq 1 ]]; then
    local type="char"
    [[ $KEYMAP == "visual-line" ]] && type="line"
    printf '\033]777;visual;start;%d;%s\a' "$MARK" "$type"
  else
    printf '\033]777;visual;end\a'
  fi
}

# Wrapper to report cursor after movement
function _st_cursor_wrapper {
  zle ".$WIDGET"
  _st_report_cursor
  _st_report_visual
}

# Wrap common movement widgets to report cursor position
for widget in vi-forward-char vi-backward-char vi-forward-word vi-forward-word-end \
              vi-backward-word vi-beginning-of-line vi-end-of-line \
              vi-goto-column forward-char backward-char forward-word backward-word \
              beginning-of-line end-of-line; do
  zle -N $widget _st_cursor_wrapper
done

# Wrap visual mode widgets
function _st_visual_wrapper {
  zle ".$WIDGET"
  _st_report_cursor
  _st_report_visual
}
for widget in visual-mode visual-line-mode; do
  zle -N $widget _st_visual_wrapper 2>/dev/null  # May not exist in all zsh versions
done

# Wrap text object widgets (for viw, viW, etc.)
for widget in select-in-word select-a-word select-in-blank-word select-a-blank-word \
              select-in-shell-word select-a-shell-word; do
  zle -N $widget _st_cursor_wrapper
done

function zle-keymap-select {
  if [[ $KEYMAP == vicmd ]]; then
    echo -ne '\e[2 q'
    printf '\033]777;vim-mode;enter\a'
  else
    echo -ne '\e[6 q'
    printf '\033]777;vim-mode;exit\a'
  fi
  _st_report_cursor
  _st_report_visual
}
zle -N zle-keymap-select
echo -ne '\e[6 q'
function zle-line-init {
  echo -ne '\e[6 q'
  printf '\033]777;vim-mode;exit\a'
  _st_report_cursor
}
zle -N zle-line-init
function zle-line-finish {
  printf '\033]777;vim-mode;exit\a'
  printf '\033]777;visual;end\a'
}
zle -N zle-line-finish
bindkey -M vicmd 'j' self-insert
bindkey -M vicmd 'k' self-insert
bindkey -M vicmd 'J' down-line-or-history
bindkey -M vicmd 'K' up-line-or-history

# Make zsh's visual mode highlight invisible (st renders the selection instead)
zle_highlight=(region:none)

# Report cwd to st (native window metadata on macOS, _ST_CWD on X11)
function chpwd {
  printf '\033]779;%s\a' "$PWD"
}
chpwd  # report initial directory

ssh() {
  local host="${@: -1}"
  printf '\033]778;ssh;%s\007' "$host"
  command ssh "$@"
  local ret=$?
  # Also tells st to discard remote TUI input modes if SSH disconnected
  # before the remote application could restore them.
  printf '\033]778;ssh;exit\007'
  return $ret
}
```
