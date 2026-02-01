# To make this terminal work it needs to work well with zsh. Put this in your .zshrc:

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

ssh() {
  local host="${@: -1}"
  printf '\033]778;ssh;%s\007' "$host"
  command ssh "$@"
  local ret=$?
  printf '\033]778;ssh;exit\007'
  return $ret
}
```
