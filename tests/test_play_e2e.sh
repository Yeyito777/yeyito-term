#!/bin/sh
# st-play end to end through tests/fake_video_term: st's graphics/video
# parsing, a timing-model player that validates the stream with libavcodec,
# and a throttled link.  Every run is bounded (fake_video_term --timeout,
# which kills the player's process group, and an outer timeout).
set -u
cd "$(dirname "$0")/.." || exit 1

FAKE=./tests/fake_video_term
PLAY=./st-play
pass=0
fail=0

scratch=$(mktemp -d "${TMPDIR:-/tmp}/st-play-e2e.XXXXXX" 2>/dev/null)
clip() { # name seconds size rate
	ffmpeg -v error -y -f lavfi -i "testsrc2=size=$3:rate=$4" \
	    -f lavfi -i "sine=frequency=440:sample_rate=48000" -t "$2" \
	    -c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 60 \
	    -c:a aac -b:a 96k "$scratch/$1.mp4"
}
if [ -z "$scratch" ] || ! clip short 20 960x540 30 2>/dev/null; then
	# A full /tmp (quota): use the build tree.
	[ -n "$scratch" ] && rm -rf "$scratch"
	scratch=$(pwd)/tests/.play-e2e.$$
	mkdir -p "$scratch"
	clip short 20 960x540 30 || { echo "test_play_e2e: ffmpeg failed"; exit 1; }
fi
trap 'rm -rf "$scratch"' EXIT INT TERM
clip tiny 2 320x180 25 || exit 1

run() { # name, fake_video_term args... -- st-play args...
	name=$1
	shift
	timeout 40 "$FAKE" --decode --timeout 25 "$@" >"$scratch/$name.out" 2>&1
	echo $? >"$scratch/$name.code"
}

value() { # name key
	sed -n "s/^$2=//p" "$scratch/$1.out" | head -n 1
}

key_latency() { # name key-name field
	awk -v k="$2" -v f="$3" '$1 == "key" && $2 == k {
		for (i = 3; i <= NF; i++) { split($i, kv, "="); if (kv[1] == f) { print kv[2]; exit } }
	}' "$scratch/$1.out"
}

check() { # description, awk condition on $v
	desc=$1
	cond=$2
	v=$3
	if [ -n "$v" ] && awk -v v="$v" "BEGIN { exit !($cond) }"; then
		pass=$((pass + 1))
		printf "  %-58s ok (%s)\n" "$desc" "$v"
	else
		fail=$((fail + 1))
		printf "  %-58s FAIL (%s)\n" "$desc" "${v:-missing}"
	fi
}

clean_exit() { # name [expected exit status]
	check "$1: exit status" "v == ${2:-0}" "$(value "$1" exit_status)"
	check "$1: video deleted" "v == 0" "$(value "$1" videos_left)"
	check "$1: alternate screen left" "v == 0" "$(value "$1" alt_screen_left_on)"
	check "$1: cursor shown" "v == 0" "$(value "$1" cursor_left_hidden)"
	check "$1: terminal cooked again" "v == 1" "$(value "$1" termios_cooked)"
	check "$1: no replies left for the shell" "v == 0" "$(value "$1" stray_reply_bytes)"
	check "$1: parser not inside an escape" "v == 1" "$(value "$1" parser_ground)"
}

# Local (unthrottled): keys and a seek.
run local --key 2:space --key 3:space --key 4:right --key 7:q \
    -- $PLAY "$scratch/short.mp4" &
# A 3 Mbit/s, 150 ms RTT link (the MacBook over SSH).
run slow --bw 3000 --rtt 150 --key 6:space --key 7:space --key 8:right \
    --key 11:q -- $PLAY "$scratch/short.mp4" &
# The link drops to 0.8 Mbit/s (with only 2 s of lead, so a sender that
# didn't adapt would run dry).
run drop --bw 3000 --rtt 100 --bw-at 3:800 --key 11:q \
    -- $PLAY --ahead 2 "$scratch/short.mp4" &
# Plays to the end and exits by itself.
run eof -- $PLAY "$scratch/tiny.mp4" &
# A resize, then SIGTERM.
run term --signal-at 3:15 --resize-at 1.5:100x30 \
    -- $PLAY "$scratch/short.mp4" &
# Inline mode at the cursor.
run inline --key 1.5:space --key 2:space --key 2.5:q \
    -- $PLAY --inline 12 "$scratch/short.mp4" &
# Terminals without video.
run novideo --no-video -- $PLAY "$scratch/tiny.mp4" &
run nographics --no-graphics -- $PLAY "$scratch/tiny.mp4" &
wait

echo "st-play end-to-end"
for n in local slow drop eof term inline; do
	check "$n: stream decodes cleanly" "v == 0" "$(value $n decode_errors)"
	check "$n: keyframe with SPS/PPS after open/flush" "v == 0" \
	    "$(value $n key_violations)"
done
reply_rate() { # name: replies per second of the run
	awk -F'[= ]' '/^reply_blocked=/ { r = $4 } /^child_exit=/ { t = $2 }
	    END { if (t > 0) printf "%.0f", r / t }' "$scratch/$1.out"
}

clean_exit local
check "local: no flood of replies (per s)" "v < 80" "$(reply_rate local)"
check "local: startup to playing (s)" "v > 0 && v < 1.5" "$(value local startup_playing)"
check "local: pause latency (s)" "v >= 0 && v < 0.3" "$(key_latency local space effect_latency)"
check "local: seek to playing (s)" "v >= 0 && v < 1.0" "$(key_latency local right playing_latency)"
check "local: no rebuffering" "v == 0" "$(value local rebuffers)"

clean_exit slow
check "slow: no flood of replies (per s)" "v < 40" "$(reply_rate slow)"
check "slow: startup to playing (s)" "v > 0 && v < 1.5" "$(value slow startup_playing)"
check "slow: pause latency (s)" "v >= 0 && v < 0.6" "$(key_latency slow space effect_latency)"
check "slow: seek reaches terminal (s)" "v >= 0 && v < 0.6" "$(key_latency slow right effect_latency)"
check "slow: seek to playing (s)" "v >= 0 && v < 1.5" "$(key_latency slow right playing_latency)"
check "slow: quit reaches terminal (s)" "v >= 0 && v < 0.6" "$(key_latency slow q effect_latency)"
check "slow: no rebuffering" "v == 0" "$(value slow rebuffers)"
rates=$(value slow wire_kbit_per_s)
check "slow: fills the link while buffering (kbit/s, s 2-4)" "v > 2500 && v < 3200" \
    "$(echo "$rates" | tr , '\n' | sed -n '2,4p' | awk '{ s += $1 } END { if (NR) printf "%.0f", s / NR }')"

clean_exit drop
check "drop: no rebuffering after 3 -> 0.8 Mbit/s" "v == 0" "$(value drop rebuffers)"
check "drop: ends below its largest resolution" "v == 1" \
    "$(value drop sizes | tr , '\n' | awk -F'[:x]' '{ a = $2 * $3; if (a > max) max = a; last = a }
	END { print (NR > 1 && last < max) ? 1 : 0 }')"

clean_exit eof
check "eof: exits by itself after the clip (s)" "v > 1.5 && v < 6" "$(value eof child_exit)"

clean_exit term 0
check "term: resize reopened the video" "v >= 2" \
    "$(value term opens | sed 's/ .*//')"

clean_exit inline
check "inline: pause latency (s)" "v >= 0 && v < 0.3" "$(key_latency inline space effect_latency)"

clean_exit novideo 1
clean_exit nographics 1

echo "$pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
	for n in local slow drop eof term inline novideo nographics; do
		echo "--- $n (exit $(cat "$scratch/$n.code"))"
		grep -v '_per_s=' "$scratch/$n.out" | head -n 40
	done
	exit 1
fi
