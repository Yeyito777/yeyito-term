# Save Command Override (`_ST_SAVE_CMD`)

External programs can tell st what command to use for dwm persist restore by setting the `_ST_SAVE_CMD` X11 property on the st window. This overrides the implicit altcmd from OSC 780 (zsh preexec). The primary use case is Claude Code agent sessions specifying `agent --resume <session-id>` so they survive dwm restarts.

## Usage

### Via the `st-save-cmd` script

```bash
st-save-cmd <st-pid> "command to restore"
```

Examples:

```bash
# Claude Code agent session
st-save-cmd $$ "agent --resume bc51e649-be60-47fb-acad-c6787f8f6211"

# Any TUI program with custom restore logic
st-save-cmd 12345 "nvim --headless +SessionRestore"
```

The script finds the X window via `xdotool search --pid`, then sets `_ST_SAVE_CMD` via `xprop`. Requires `xdotool` and `xprop`.

### Via `xprop` directly

```bash
xprop -id <window-id> -f _ST_SAVE_CMD 8u -set _ST_SAVE_CMD "agent --resume <uuid>"
```

## Architecture

### Property-based IPC (same pattern as `_ST_NOTIFY`)

- Any process sets `_ST_SAVE_CMD` on the st window via `XChangeProperty`
- st detects the change via `PropertyNotify` in `propnotify()` (x.c)
- st reads the property with `delete=True` (atomic read+delete, no stale state)
- The value is stored in `persist_save_cmd_buf` via `persist_set_save_cmd()`

### Save priority

In `persist_save_generic()`:

1. If `persist_save_cmd_buf` is set → write `altcmd=<save_cmd>` (always, regardless of altscreen)
2. Else if `MODE_ALTSCREEN` is set and `persist_altcmd_buf` is set → write `altcmd=<altcmd>` (implicit OSC 780)
3. Otherwise → no altcmd written

The key difference: `save_cmd` is an **explicit** request from the program, so it is always saved. The OSC 780 `altcmd` is **implicit** (from zsh preexec) and only applies when a fullscreen program is detected via `MODE_ALTSCREEN`.

This distinction matters because some TUI programs (notably Claude Code) do not use the alternate screen buffer, so `MODE_ALTSCREEN` is never set for them.

### Restore flow

On restore (`--from-save`), st reads `altcmd=` from `generic-data.save` and types the command into the pty after the shell starts. The command runs as if the user typed it. For agent sessions, this means:

```
st --from-save dir → shell starts → types "agent --resume <uuid>\n" → start.sh forwards --resume to claude
```

## Claude Code Agent Integration

The Agent project (`~/Workspace/Agent`) has hooks on both `UserPromptSubmit` and `SessionStart` that automate this:

### Hook: `src/hooks/save-cmd.sh`

Wired in `.claude/settings.local.json` on two events:

- **`UserPromptSubmit`** — fires on every user message, keeps the save command current during normal use.
- **`SessionStart`** — fires on all sources. For `resume`/`clear`/`compact`, saves `agent --resume <session-id>`. For `startup`, saves bare `agent` (no conversation exists yet to resume).

The script:

1. Reads `session_id` from the hook's stdin JSON (all hooks receive `session_id` in the base envelope)
2. Calls `st-save-cmd $AGENT_TERMINAL_PID "agent --resume $SESSION_ID"`

`AGENT_TERMINAL_PID` is set by `src/start.sh` at launch by walking up the process tree to find the `st` ancestor.

### `start.sh --resume` support

`src/start.sh` accepts `--resume <session-id>` and forwards it to claude:

```bash
agent --resume bc51e649-be60-47fb-acad-c6787f8f6211
# expands to: start.sh --resume bc51e649-...
# which runs: claude --dangerously-skip-permissions --resume bc51e649-...
```

The zsh alias `agent='cd ... && .../start.sh'` naturally passes extra args through.

### Handling `/clear` and `/resume`

These commands change the session ID within Claude Code. The `SessionStart` hook fires immediately when the new session begins, updating the save command before any user message is needed. The `UserPromptSubmit` hook provides ongoing updates during normal use.

## Relevant Files

### st

| File | What |
|------|------|
| `persist.c` | `persist_save_cmd_buf`, `persist_set_save_cmd()`, `persist_get_save_cmd()`, save priority logic in `persist_save_generic()` |
| `persist.h` | Public API declarations |
| `x.c` | `xw.stsavecmd` atom, `propnotify()` handler, `xinit()` atom creation |
| `notif.c`, `sshind.c` | XWindow struct copies (must include `stsavecmd` field for layout parity) |
| `scripts/st-save-cmd` | Helper script (installed to `/usr/local/bin/st-save-cmd`) |
| `tests/test_persist.c` | Tests: `save_cmd_set_and_get`, `save_cmd_null_clears`, `save_cmd_overrides_altcmd`, `save_cmd_saved_without_altscreen`, `altcmd_used_when_no_save_cmd` |

### Agent project (`~/Workspace/Agent`)

| File | What |
|------|------|
| `src/hooks/save-cmd.sh` | UserPromptSubmit + SessionStart hook — reads session_id, calls st-save-cmd |
| `src/start.sh` | Accepts `--resume <id>`, forwards to claude |
| `.claude/settings.local.json` | Hook wiring (5s timeout) |

## Key Design Decisions

1. **X11 property, not OSC**: Programs running inside the terminal can't easily send OSC sequences to st from hooks (hook stdout goes to Claude Code, not the pty). X11 properties are settable from any process that knows the window ID.

2. **save_cmd ignores MODE_ALTSCREEN**: Claude Code's TUI doesn't use the alternate screen buffer. The explicit `_ST_SAVE_CMD` mechanism doesn't need altscreen detection — the program is explicitly asking to be saved.

3. **Session ID from hook stdin**: The hook reads `session_id` directly from the JSON payload (all hooks receive it), avoiding fragile filesystem-based session discovery.
