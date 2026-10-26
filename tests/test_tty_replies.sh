#!/usr/bin/env bash
# Replies to a program that isn't reading its input must neither block st
# nor make it re-enter ttyread() from inside the parser (upstream st did,
# and corrupted the buffer being parsed: crashes under st video's status
# replies).  The child sends many DA1 queries without reading, interleaved
# with output, then reads every reply: all must arrive, in order, and st
# must still be running.  Runs st in a nested X environment (xenv).
set -euo pipefail

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
st_bin=${ST_BIN:-"$repo/st"}
env_name="st-tty-replies-$$"
tmpdir=$(mktemp -d /tmp/st-tty-replies.XXXXXX)

cleanup() {
	set +e
	if [[ -n "${st_pid:-}" ]]; then
		kill "$st_pid" 2>/dev/null
		wait "$st_pid" 2>/dev/null
	fi
	xenv stop "$env_name" >/dev/null 2>&1
	rm -rf "$tmpdir"
}
trap cleanup EXIT

command -v xenv >/dev/null 2>&1 || { echo "SKIP: xenv missing"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 missing"; exit 0; }

xenv start "$env_name" >/dev/null
display=$(xenv display -e "$env_name")

cat >"$tmpdir/child.py" <<'PY'
import os, re, select, sys, termios, time, tty
result = sys.argv[1]
fd = sys.stdin.fileno()
tty.setraw(fd)
queries = 20000
# 20000 DA1 queries (100 KB of replies, far beyond the PTY's buffer) mixed
# with output st must keep parsing meanwhile.
for i in range(queries):
    os.write(1, b"\033[c" + (b"x" if i % 50 == 0 else b""))
data = b""
deadline = time.time() + 20
while data.count(b"c") < queries and time.time() < deadline:
    if select.select([fd], [], [], 0.5)[0]:
        data += os.read(fd, 65536)
reply = re.compile(rb"\033\[\?[0-9;]*c")
replies = len(reply.findall(data))
clean = reply.sub(b"", data) == b""
with open(result + ".tmp", "w") as f:
    f.write("%d %d\n" % (replies, 1 if clean else 0))
os.rename(result + ".tmp", result)
time.sleep(30)
PY

DISPLAY=$display "$st_bin" -e python3 "$tmpdir/child.py" "$tmpdir/result" \
	>"$tmpdir/st.log" 2>&1 &
st_pid=$!

for _ in $(seq 1 1200); do
	[[ -e "$tmpdir/result" ]] && break
	kill -0 "$st_pid" 2>/dev/null || break
	sleep 0.025
done
if ! kill -0 "$st_pid" 2>/dev/null; then
	echo "FAIL: st exited while replying to an unread program"
	cat "$tmpdir/st.log"
	exit 1
fi
read -r replies clean <"$tmpdir/result" || { echo "FAIL: child never finished"; exit 1; }
if [[ "$replies" != 20000 || "$clean" != 1 ]]; then
	echo "FAIL: got $replies of 20000 DA1 replies (clean=$clean)"
	exit 1
fi
echo "tty replies to an unread program: PASS"
