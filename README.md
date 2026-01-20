# To make this terminal work it needs to work well with zsh. Put this in your .zshrc:

```zsh
bindkey -v
bindkey -M viins '^?' backward-delete-char
bindkey -M viins '^H' backward-delete-char
KEYTIMEOUT=1
function zle-keymap-select {
  if [[ $KEYMAP == vicmd ]]; then
    echo -ne '\e[2 q'
    printf '\033]777;vim-mode;enter\a'
  else
    echo -ne '\e[6 q'
    printf '\033]777;vim-mode;exit\a'
  fi
}
zle -N zle-keymap-select
echo -ne '\e[6 q'
function zle-line-init {
  echo -ne '\e[6 q'
  printf '\033]777;vim-mode;exit\a'
}
zle -N zle-line-init
function zle-line-finish {
  printf '\033]777;vim-mode;exit\a'
}
zle -N zle-line-finish
bindkey -M vicmd 'j' self-insert
bindkey -M vicmd 'k' self-insert
bindkey -M vicmd 'J' down-line-or-history
bindkey -M vicmd 'K' up-line-or-history
ssh() {
  local host="${@: -1}"
  printf '\033]778;ssh;%s\007' "$host"
  command ssh "$@"
  local ret=$?
  printf '\033]778;ssh;exit\007'
  return $ret
}
```
