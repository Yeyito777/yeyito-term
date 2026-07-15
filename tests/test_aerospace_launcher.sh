#!/bin/sh

set -eu

CDPATH=
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/st-aerospace-launcher-test.$$
mkdir -p "$tmp/home" "$tmp/bin"
log=$tmp/aerospace.log
pids=$tmp/st.pids

cleanup()
{
	if [ -f "$pids" ]; then
		while IFS= read -r pid; do
			kill "$pid" 2>/dev/null || true
		done < "$pids"
	fi
	rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

cat > "$tmp/bin/st" <<'EOF'
#!/bin/sh
printf '%s\n' "$$" >> "$FAKE_ST_PIDS"
sleep 2
EOF

cat > "$tmp/bin/aerospace" <<'EOF'
#!/bin/sh
set -eu

command=$1
shift
case "$command" in
	list-workspaces)
		printf 'TEST\n'
		;;
	list-windows)
		pid=
		while [ "$#" -gt 0 ]; do
			if [ "$1" = --pid ]; then
				pid=$2
				break
			fi
			shift
		done
		[ -n "$pid" ] || exit 64
		printf 'query %s\n' "$pid" >> "$FAKE_AEROSPACE_LOG"
		kill -0 "$pid" 2>/dev/null && printf '%s\n' "$pid"
		;;
	move-node-to-workspace)
		[ "$1" = --window-id ]
		printf 'move %s %s\n' "$2" "$3" >> "$FAKE_AEROSPACE_LOG"
		;;
	focus)
		[ "$1" = --window-id ]
		printf 'focus %s\n' "$2" >> "$FAKE_AEROSPACE_LOG"
		;;
	*)
		exit 64
		;;
esac
EOF

chmod +x "$tmp/bin/st" "$tmp/bin/aerospace"
export FAKE_ST_PIDS="$pids"
export FAKE_AEROSPACE_LOG="$log"

run_launcher()
{
	HOME=$tmp/home \
	AEROSPACE_BIN=$tmp/bin/aerospace \
	ST_BINARY=$tmp/bin/st \
	ST_AEROSPACE_POLL_INTERVAL=0.001 \
	"$repo/scripts/st-aerospace-launch"
}

launch_count=12
launcher_pids=
launch_number=0
while [ "$launch_number" -lt "$launch_count" ]; do
	run_launcher &
	launcher_pids="$launcher_pids $!"
	launch_number=$((launch_number + 1))
done
for launcher_pid in $launcher_pids; do
	wait "$launcher_pid"
done

focus_ids=$(awk '$1 == "focus" { print $2 }' "$log")
focus_count=$(printf '%s\n' "$focus_ids" | awk 'NF { count++ } END { print count + 0 }')
unique_focus_count=$(printf '%s\n' "$focus_ids" | sort -u | awk 'NF { count++ } END { print count + 0 }')
move_count=$(awk '$1 == "move" && $3 == "TEST" { count++ } END { print count + 0 }' "$log")

if [ "$focus_count" -ne "$launch_count" ] ||
	[ "$unique_focus_count" -ne "$launch_count" ] ||
	[ "$move_count" -ne "$launch_count" ]; then
	printf 'launcher did not independently target every st process:\n' >&2
	cat "$log" >&2
	exit 1
fi

for window_id in $focus_ids; do
	if ! grep -q "^query $window_id\$" "$log"; then
		printf 'focused window %s was not selected by its owning PID\n' "$window_id" >&2
		exit 1
	fi
done

printf 'AeroSpace launcher concurrency test passed\n'
