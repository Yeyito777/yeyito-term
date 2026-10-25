#!/bin/sh

set -eu

CDPATH=
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
launcher=${ST_AEROSPACE_LAUNCHER:-$repo/st-aerospace-launch}
if [ "$(uname -s)" != Darwin ]; then
	printf 'AeroSpace launcher tests skipped: macOS only\n'
	exit 0
fi
# Not $TMPDIR: on macOS it is so long that the spares' control socket paths
# would exceed the 104-byte sun_path limit.
tmp=/tmp/st-aerospace-launcher-test.$$
mkdir -p "$tmp/home" "$tmp/bin"
tmp=$(cd "$tmp" && pwd -P)
log=$tmp/aerospace.log
ready=$tmp/ready
reveal_log=$tmp/reveal.log
events=$tmp/events
slot=$tmp/home/.runtime/st/spare
socket=$tmp/aerospace.sock
mkdir -p "$ready"
: > "$log"
: > "$reveal_log"
: > "$events"

# Every process this test leaves running names $tmp on its command line:
# the fake server, the fake spares (also those the launcher started in their
# own session, out of reach of this shell), their listeners and sleepers, the
# cwd stand-ins and the watchdog. None of them polls, and each one also ends
# on its own (a timeout, or the server noticing this shell is gone).
cleanup()
{
	trap - EXIT
	pkill -f "$tmp/" 2>/dev/null || true
	rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP
# A hung test must not keep anything running: give up after five minutes.
perl -e 'sleep 300; kill TERM => $ARGV[0]' "$$" "$tmp/watchdog" &

# A spare like `st --spare`: publish the slot, serve one spawn request on the
# control socket, then "open" the window, which AeroSpace reports as an event.
cat > "$tmp/bin/st" <<'EOF'
#!/bin/sh
runtime=$HOME/.runtime/st
slot=$runtime/spare
case "${1:-}" in
	--spare) ;;
	--spare-slot) slot=$2 ;;
	*) exit 64 ;;
esac
dir=$runtime/st-$$
mkdir -p "$dir"
# Not `nc -lU`: macOS nc in listen mode drops unread input when the sender
# closes right after writing (requests carry a ~3 KiB environment).
perl -MIO::Socket::UNIX -e '
	alarm 120;
	my $server = IO::Socket::UNIX->new(Type => SOCK_STREAM(),
	    Local => $ARGV[0], Listen => 1) or die "listen: $!";
	my $client = $server->accept or die "accept: $!";
	local $/;
	my $request = <$client>;
	open(my $out, ">", $ARGV[1]) or die "open: $!";
	print $out $request;
' "$dir/control.sock" "$dir/request" &
listener=$!
while [ ! -S "$dir/control.sock" ]; do sleep 0.01; done
stamp=$(stat -f '%i:%m' "$0" 2>/dev/null || stat -c '%i:%Y' "$0")
printf '%s %s %s\n' "$$" "$$" "$stamp" > "$slot.$$.tmp"
if ! ln "$slot.$$.tmp" "$slot" 2>/dev/null; then
	rm -f "$slot.$$.tmp"
	kill "$listener"
	exit 0
fi
rm -f "$slot.$$.tmp"
trap 'printf "reveal %s\n" "$$" >> "$FAKE_REVEAL_LOG"' USR1
wait "$listener"
[ -s "$dir/request" ] || exit 0
tr '\0' '\n' < "$dir/request" > "$dir/fields"
# Like st, ignore a request that does not end with its end field.
[ "$(tail -n 1 "$dir/fields")" = end ] || exit 65
sed -n '1p' "$dir/fields" > "$FAKE_ST_READY_DIR/$$.command"
sed -n 's/^cwd=//p' "$dir/fields" > "$FAKE_ST_READY_DIR/$$.cwd"
sed -n 's/^env=ST_INHERIT_VIRTUAL_ENV=//p' "$dir/fields" > "$FAKE_ST_READY_DIR/$$.venv"
sed -n 's/^PATH=//p' "$dir/fields" > "$FAKE_ST_READY_DIR/$$.path"
sed -n 's/^ST_TEST_LAUNCHER_ONLY=//p' "$dir/fields" > "$FAKE_ST_READY_DIR/$$.marker"
sed -n 's/^title=//p' "$dir/fields" > "$FAKE_ST_READY_DIR/$$.title"
sed -n 's/^arg=//p' "$dir/fields" > "$FAKE_ST_READY_DIR/$$.args"
: > "$FAKE_ST_READY_DIR/$$"
# The fake AeroSpace server turns this into a window-detected event.
printf 'detected %s\n' "$$" >> "$FAKE_EVENTS"
# Wait for the reveal without forking: a trapped USR1 interrupts `wait`.
# The sleeper bounds the lifetime of a spare whose test died.
perl -e 'sleep 120' "$dir" &
sleeper=$!
while kill -0 "$sleeper" 2>/dev/null; do
	wait "$sleeper" || :
done
EOF

chmod +x "$tmp/bin/st"
export FAKE_TEST_PID=$$
export FAKE_AEROSPACE_LOG="$log"
export FAKE_ST_READY_DIR="$ready"
export FAKE_REVEAL_LOG="$reveal_log"
export FAKE_EVENTS="$events"
# The server's state lives in files: it runs for the whole test, and spares
# boot long before they are used. Window ids are the owning fake st's PID.
export FAKE_DETECT_WORKSPACE="$tmp/detect-workspace"
export FAKE_FOCUS_WORKSPACE="$tmp/focus-workspace"
export FAKE_FOCUSED="$tmp/focused"
export FAKE_NO_DETECT="$tmp/no-detect"
export FAKE_FAIL_FOCUS="$tmp/fail-focus"
printf 'TEST\n' > "$FAKE_DETECT_WORKSPACE"
printf 'TEST\n' > "$FAKE_FOCUS_WORKSPACE"
: > "$FAKE_FOCUSED"
perl "$repo/tests/fake_aerospace_server.pl" "$socket" &
server=$!
attempt=0
while [ ! -S "$socket" ]; do
	attempt=$((attempt + 1))
	if [ "$attempt" -gt 500 ] || ! kill -0 "$server" 2>/dev/null; then
		printf 'fake AeroSpace server did not start\n' >&2
		exit 1
	fi
	sleep 0.01
done

run_launcher()
{
	HOME=$tmp/home \
	ST_AEROSPACE_SOCKET=$socket \
	ST_BINARY=$tmp/bin/st \
	ST_AEROSPACE_TICK=${ST_AEROSPACE_TICK:-0.02} \
	ST_AEROSPACE_MAX_TICKS=${ST_AEROSPACE_MAX_TICKS:-500} \
	"$launcher" "$@"
}

# The window of the most recent launch (focused or moved with focus).
launched()
{
	awk '$1 == "focus" || $1 == "move" { id = $2 } END { print id }' "$log"
}

wait_for_reveal()
{
	attempt=0
	while ! grep -q "^reveal $1\$" "$reveal_log"; do
		attempt=$((attempt + 1))
		if [ "$attempt" -gt 200 ]; then
			printf 'st process %s did not receive its reveal handshake\n' "$1" >&2
			exit 1
		fi
		sleep 0.01
	done
}

spare_holder()
{
	holder=
	[ -s "$slot" ] && read -r holder _ < "$slot"
	printf '%s\n' "$holder"
}

expect_live_spare()
{
	holder=$(spare_holder)
	if [ -z "$holder" ] || ! kill -0 "$holder" 2>/dev/null; then
		printf 'expected a live spare in %s\n' "$slot" >&2
		exit 1
	fi
	if [ -f "$ready/$holder" ]; then
		printf 'spare %s was already claimed\n' "$holder" >&2
		exit 1
	fi
}

# Every launch appends one line to the launch log under the test's HOME.
launch_log=$tmp/home/Library/Logs/st/launch.log
logged=0
log_lines()
{
	if [ -f "$launch_log" ]; then
		wc -l < "$launch_log" | tr -d ' '
	else
		printf '0\n'
	fi
}

check_log_format()
{
	bad=$(grep -Ev '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}[+-][0-9]{4} st=[0-9]+ src=launcher result=(ok|fail)( [a-z0-9_]+=("[^"]*"|[^ "]+))*$' \
		"$launch_log" || true)
	if [ -n "$bad" ]; then
		printf 'malformed launch log lines:\n%s\n' "$bad" >&2
		exit 1
	fi
}

# Exactly one line since the last check, matching every extended regex given;
# a pattern starting with ! must not match.
expect_logged()
{
	count=$(log_lines)
	if [ "$count" -ne $((logged + 1)) ]; then
		printf 'expected one new launch log line, got %s:\n' \
			$((count - logged)) >&2
		cat "$launch_log" >&2
		exit 1
	fi
	logged=$count
	check_log_format
	line=$(tail -n 1 "$launch_log")
	for pattern; do
		case $pattern in
			!*) ! printf '%s\n' "$line" | grep -Eq -- "${pattern#!}" ;;
			*) printf '%s\n' "$line" | grep -Eq -- "$pattern" ;;
		esac || {
			printf 'launch log line does not satisfy <%s>:\n%s\n' \
				"$pattern" "$line" >&2
			exit 1
		}
	done
}

ms='[0-9]+\.[0-9]'
warm_fields="result=ok path=warm spare_age=$ms cmd=shell main=$ms connected=$ms claimed=$ms target=$ms spawn=$ms detected=$ms via=(event|query) focused=$ms revealed=$ms load1=[0-9]+\.[0-9]{2}\$"

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
move_count=$(awk '$1 == "move" { count++ } END { print count + 0 }' "$log")

if [ "$focus_count" -ne "$launch_count" ] ||
	[ "$unique_focus_count" -ne "$launch_count" ] ||
	[ "$move_count" -ne 0 ]; then
	printf 'launcher did not independently target every st process:\n' >&2
	cat "$log" >&2
	exit 1
fi

for window_id in $focus_ids; do
	if [ "$(cat "$ready/$window_id.command")" != spawn ]; then
		printf 'focused window %s was not spawned through its own spare\n' \
			"$window_id" >&2
		exit 1
	fi
	wait_for_reveal "$window_id"
done
# Concurrent launchers race to replace the spare; exactly one wins.
expect_live_spare

# One line each, never interleaved, naming each launch's own st. The first
# found no spare: cold.
logged=$(log_lines)
[ "$logged" -eq "$launch_count" ]
check_log_format
[ "$(grep -c ' result=ok ' "$launch_log")" -eq "$launch_count" ]
[ "$(sed -n 's/^[^ ]* st=\([0-9]*\) .*/\1/p' "$launch_log" | sort)" = \
	"$(printf '%s\n' $focus_ids | sort)" ]
grep -q ' path=cold reason=no-spare cmd=shell ' "$launch_log"

printf 'AeroSpace launcher concurrency and reveal handshake test passed\n'

# The next launch claims the waiting spare, then leaves a fresh one.
spare=$(spare_holder)
run_launcher
[ "$(launched)" = "$spare" ]
wait_for_reveal "$spare"
expect_live_spare
[ "$(spare_holder)" != "$spare" ]
expect_logged "^[^ ]+ st=$spare src=launcher $warm_fields"

# The spare's age is the time since it published its slot.
spare=$(spare_holder)
touch -t "$(date -v-2H '+%Y%m%d%H%M.%S')" "$slot"
run_launcher -e /usr/local/bin/exocortex --flag
wait_for_reveal "$spare"
expect_logged "st=$spare " ' cmd=exocortex '
age=$(tail -n 1 "$launch_log" | sed -n 's/.* spare_age=\([0-9]*\).*/\1/p')
[ "$age" -ge 7190 ] && [ "$age" -le 7300 ]

# A window AeroSpace placed elsewhere is moved, with focus, to the workspace
# that was focused at invocation time.
printf 'OTHER\n' > "$FAKE_DETECT_WORKSPACE"
run_launcher
printf 'TEST\n' > "$FAKE_DETECT_WORKSPACE"
[ "$(awk '$1 == "focus" || $1 == "move" { last = $1 " " $3 } END { print last }' \
	"$log")" = "move TEST" ]
wait_for_reveal "$(launched)"
expect_logged ' result=ok path=warm ' " revealed=$ms moved=1 "

# A dead spare's slot is discarded and the launch still succeeds.
holder=$(spare_holder)
kill "$holder"
printf '999999 999999 0:0\n' > "$slot"
before=$(launched)
run_launcher
[ "$(launched)" != "$before" ]
[ "$(launched)" != 999999 ]
wait_for_reveal "$(launched)"
expect_live_spare
expect_logged "st=$(launched) src=launcher result=ok path=cold reason=dead cmd=shell main=$ms connected=$ms claimed=$ms " \
	'!spare_age' " revealed=$ms "

# A spare started from a binary that has since been replaced is never used.
holder=$(spare_holder)
read -r _ window _ < "$slot"
printf '%s %s 0:0\n' "$holder" "$window" > "$slot"
run_launcher
[ "$(launched)" != "$holder" ]
attempt=0
while kill -0 "$holder" 2>/dev/null; do
	attempt=$((attempt + 1))
	if [ "$attempt" -gt 200 ]; then
		printf 'outdated spare %s was not stopped\n' "$holder" >&2
		exit 1
	fi
	sleep 0.01
done
expect_live_spare
expect_logged ' result=ok path=cold reason=outdated '

# Title, command and the caller's environment reach the spawned terminal.
(PATH="$tmp/extra:$PATH"; export ST_TEST_LAUNCHER_ONLY=yes
	run_launcher -T 'a title' -e /bin/echo 'two words')
pid=$(launched)
if [ "$(cat "$ready/$pid.marker")" != yes ]; then
	printf 'launcher environment was not forwarded to %s; request:\n' "$pid" >&2
	cat "$tmp/home/.runtime/st/st-$pid/fields" >&2
	exit 1
fi
[ "$(cat "$ready/$pid.title")" = 'a title' ]
[ "$(sed -n '1p' "$ready/$pid.args")" = /bin/echo ]
[ "$(sed -n '2p' "$ready/$pid.args")" = 'two words' ]
[ "$(sed -n '3p' "$ready/$pid.args")" = '' ]
case "$(cat "$ready/$pid.path")" in
	"$tmp/extra:"*) ;;
	*) printf 'PATH was not forwarded\n' >&2; exit 1 ;;
esac
# A plain launch sends no command, so the spare starts the shell.
run_launcher
[ ! -s "$ready/$(launched).args" ]

# ST_AEROSPACE_KEEP_SPARE=0 leaves nothing running behind.
holder=$(spare_holder)
run_launcher
[ "$(launched)" = "$holder" ]
(export ST_AEROSPACE_KEEP_SPARE=0; run_launcher)
[ ! -e "$slot" ]

printf 'AeroSpace launcher spare reuse, replacement and request tests passed\n'

# Workspace names survive JSON both ways: quotes, backslashes, non-ASCII.
name='wörk "1" \ ✓'
printf '%s\n' "$name" > "$FAKE_FOCUS_WORKSPACE"
printf '%s\n' "$name" > "$FAKE_DETECT_WORKSPACE"
placement()
{
	grep -E '^(focus|move) ' "$log" | tail -n 1
}
run_launcher
[ "$(placement)" = "focus $(launched)" ]
printf 'OTHER\n' > "$FAKE_DETECT_WORKSPACE"
run_launcher
[ "$(placement)" = "move $(launched) $name" ]
printf 'TEST\n' > "$FAKE_FOCUS_WORKSPACE"
printf 'TEST\n' > "$FAKE_DETECT_WORKSPACE"
wait_for_reveal "$(launched)"

# The window-detected event alone completes a launch: with the first
# fallback query 10 s away, none is made.
logged=$(log_lines)
before=$(grep -c '^query' "$log" || true)
ST_AEROSPACE_TICK=5
run_launcher
unset ST_AEROSPACE_TICK
[ "$(grep -c '^query' "$log")" -eq "$before" ]
wait_for_reveal "$(launched)"
expect_logged " detected=$ms via=event "

# Requests carry AeroSpace's window/workspace context, like its CLI, and
# never leave the fields out (the server warns about that).
(export AEROSPACE_WINDOW_ID=42 AEROSPACE_WORKSPACE=W; run_launcher --inherit-cwd)
[ "$(grep '^focused-context' "$log" | tail -n 1)" = 'focused-context 42 W' ]
(unset AEROSPACE_WINDOW_ID AEROSPACE_WORKSPACE; run_launcher --inherit-cwd)
[ "$(grep '^focused-context' "$log" | tail -n 1)" = 'focused-context null null' ]
if grep -q '^incomplete-request\|^unexpected' "$log"; then
	printf 'launcher sent requests the server did not expect:\n' >&2
	cat "$log" >&2
	exit 1
fi

expect_failure()
{
	expected=$1
	shift
	status=0
	"$@" 2> "$tmp/stderr" || status=$?
	if [ "$status" -ne 1 ] || ! grep -qF "$expected" "$tmp/stderr"; then
		printf 'expected exit 1 and <%s>, got %s:\n' "$expected" "$status" >&2
		cat "$tmp/stderr" >&2
		exit 1
	fi
}

wait_until_dead()
{
	attempt=0
	while kill -0 "$1" 2>/dev/null; do
		attempt=$((attempt + 1))
		if [ "$attempt" -gt 200 ]; then
			printf 'st process %s was not stopped\n' "$1" >&2
			exit 1
		fi
		sleep 0.01
	done
}

expect_unrevealed()
{
	if grep -q "^reveal $1\$" "$reveal_log"; then
		printf 'st process %s was revealed after a failed launch\n' "$1" >&2
		exit 1
	fi
}

# Without AeroSpace the launch fails before it takes the spare.
holder=$(spare_holder)
logged=$(log_lines)
expect_failure "Can't connect to the AeroSpace server" \
	env HOME="$tmp/home" ST_AEROSPACE_SOCKET="$tmp/missing.sock" \
	ST_BINARY="$tmp/bin/st" "$launcher"
[ "$(spare_holder)" = "$holder" ]
expect_live_spare
expect_logged "st=0 src=launcher result=fail cmd=shell main=$ms load1=[0-9.]+ error=\"Can't connect to the AeroSpace server at $tmp/missing.sock \(.*\)\. Is AeroSpace.app running\?\"\$"

# A window AeroSpace never reports: queries back off (2, 4, 8 ticks), then
# the launch fails and the claimed st is stopped, not left invisible.
: > "$FAKE_NO_DETECT"
before=$(grep -c '^query' "$log" || true)
ST_AEROSPACE_MAX_TICKS=10
expect_failure "AeroSpace did not register the st window for PID $holder" \
	run_launcher
unset ST_AEROSPACE_MAX_TICKS
rm -f "$FAKE_NO_DETECT"
[ "$(($(grep -c '^query' "$log") - before))" -eq 3 ]
wait_until_dead "$holder"
expect_unrevealed "$holder"
expect_logged "st=$holder src=launcher result=fail path=warm " " spawn=$ms " \
	'!detected=' "error=\"AeroSpace did not register the st window for PID $holder\"\$"

# A failed focus also stops the st it claimed.
run_launcher
holder=$(spare_holder)
: > "$FAKE_FAIL_FOCUS"
logged=$(log_lines)
expect_failure "AeroSpace could not focus st window $holder" run_launcher
rm -f "$FAKE_FAIL_FOCUS"
wait_until_dead "$holder"
expect_unrevealed "$holder"
expect_logged "st=$holder src=launcher result=fail " " detected=$ms via=(event|query) " \
	'!focused=' '!revealed=' "error=\"AeroSpace could not focus st window $holder\"\$"

# Usage errors exit 64 and are no launch: nothing is logged.
logged=$(log_lines)
status=0
run_launcher --bogus 2>/dev/null || status=$?
[ "$status" -eq 64 ]
status=0
run_launcher --cwd 2>/dev/null || status=$?
[ "$status" -eq 64 ]
[ "$(log_lines)" -eq "$logged" ]
check_log_format

printf 'AeroSpace launcher protocol and failure tests passed\n'

check_directory()
{
	expected=$1
	shift
	run_launcher "$@"
	pid=$(launched)
	actual=$(cat "$ready/$pid.cwd")
	if [ "$actual" != "$expected" ]; then
		printf 'Expected cwd <%s>, got <%s>\n' "$expected" "$actual" >&2
		exit 1
	fi
}

check_directory "$tmp" --cwd "$tmp"
# A missing directory falls back to home rather than failing the launch.
check_directory "$tmp/home" --cwd "$tmp/missing"

focused()
{
	printf '%s\n' "$1" > "$FAKE_FOCUSED"
}

# No focus and non-terminal focus both retain the normal home-directory launch.
focused ''
check_directory "$tmp/home" --inherit-cwd
focused "com.apple.finder $$"
check_directory "$tmp/home" --inherit-cwd

# Real parent/PTY-child stand-in: only the child changes directory, so reading
# the terminal parent's cwd would fail this test. Include spaces and punctuation.
directory="$tmp/project with spaces ' and \$"
mkdir -p "$directory"
sh -c 'sh -c '\''cd "$1"; exec perl -e "sleep 120" "$1"'\'' sh "$1" & wait' sh "$directory" &
terminal_pid=$!
attempt=0
child_pid=
while [ "$attempt" -lt 100 ]; do
	child_pid=$(/usr/bin/pgrep -P "$terminal_pid" | sed -n '1p')
	if [ -n "$child_pid" ]; then
		actual=$(/usr/sbin/lsof -a -p "$child_pid" -d cwd -Fn 2>/dev/null |
			sed -n 's/^n//p')
		[ "$actual" = "$directory" ] && break
	fi
	attempt=$((attempt + 1))
	sleep 0.01
done
[ -n "$child_pid" ]
focused "io.yeyito.st $terminal_pid"
check_directory "$directory" --inherit-cwd
venv="$directory/venv with spaces"
mkdir -p "$venv/bin" "$tmp/home/.cache/st-shell-context"
: > "$venv/bin/activate"
context="$tmp/home/.cache/st-shell-context/$terminal_pid-$child_pid.venv"
printf '%s\n' "$venv" > "$context"
check_directory "$directory" --inherit-cwd
[ "$(cat "$ready/$pid.venv")" = "$venv" ]
# The original shortcut must still open at home even with a terminal focused.
check_directory "$tmp/home"
[ -z "$(cat "$ready/$pid.venv")" ]
# Other windows must not inherit the previously focused terminal's venv.
focused "com.apple.finder $$"
check_directory "$tmp/home" --inherit-cwd
[ -z "$(cat "$ready/$pid.venv")" ]
focused "io.yeyito.st $terminal_pid"
# Deactivation and a deleted venv both launch a clean shell.
printf '\n' > "$context"
check_directory "$directory" --inherit-cwd
[ -z "$(cat "$ready/$pid.venv")" ]
printf '%s\n' "$venv/missing" > "$context"
check_directory "$directory" --inherit-cwd
[ -z "$(cat "$ready/$pid.venv")" ]
kill "$child_pid" "$terminal_pid" 2>/dev/null || true
wait "$terminal_pid" 2>/dev/null || true
check_directory "$tmp/home" --inherit-cwd
printf 'AeroSpace launcher cwd inheritance and fallback tests passed\n'
