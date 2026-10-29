#!/bin/sh
# A minimal mode-2032 application for tests/macos_headless -A: fills the
# alternate screen, then every INTERVAL seconds (default 0.3) sends one move
# whose 3-row easing term st animates over 100 ms (docs/app-smooth-scroll.md).
#   tests/macos_headless -A -t 10 -- tests/glide_driver.sh
interval=${1:-0.3}
rows=$(stty size 2>/dev/null | cut -d' ' -f1); rows=${rows:-40}
cols=$(stty size 2>/dev/null | cut -d' ' -f2); cols=${cols:-100}
printf '\033[?1049h\033[?2032h\033]2032;r;1,1,%d,%d\033\\' "$rows" "$cols"
i=0
while [ $i -lt "$rows" ]; do printf 'line %d of the glide driver\r\n' $i; i=$((i + 1)); done
trap 'printf "\033[?2032l\033[?1049l"; exit 0' INT TERM HUP
while :; do
	printf '\033]2032;m;0;3000;100\033\\'
	sleep "$interval"
done
