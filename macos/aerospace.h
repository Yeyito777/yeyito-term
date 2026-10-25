/* See LICENSE for license details. */
/*
 * AeroSpace's server protocol and the pure parts of st-aerospace-launch.
 *
 * The server listens on /tmp/<app-id>-<user>.sock. A client sends its
 * protocol version as a native-endian UInt32 and reads the server's; after
 * that every message in either direction is a native-endian UInt32 length
 * followed by that many bytes of JSON. A request is
 * {"args":[...],"stdin":"","windowId":ID|null,"workspace":NAME|null} and its
 * answer {"exitCode":N,"stdout":"...","stderr":"...",...}. After a
 * "subscribe" request the connection only streams event objects, e.g.
 * {"_event":"window-detected","windowId":N,"workspace":"1",...}; the first
 * one (focus-changed) describes the focus at subscription time.
 */

#ifndef ST_MACOS_AEROSPACE_H
#define ST_MACOS_AEROSPACE_H

#include <stddef.h>
#include <sys/types.h>

#define AEROSPACE_PROTOCOL_VERSION 1u
#define AEROSPACE_MAX_FRAME (16u << 20)

typedef struct {
	char *data;
	size_t length, capacity;
} AeroBuffer;

int aero_buffer_append(AeroBuffer *buffer, const void *data, size_t length);
void aero_buffer_free(AeroBuffer *buffer);

/* Append one framed request. windowid < 0 and workspace NULL are sent as
 * explicit nulls, like the CLI when AEROSPACE_WINDOW_ID/AEROSPACE_WORKSPACE
 * are unset (the server warns about missing fields). Returns 0 or -1. */
int aero_frame_request(AeroBuffer *buffer, const char *const *args, int nargs,
    long long windowid, const char *workspace);

/* The next complete frame in data[0, length): returns the bytes it spans
 * (header included) and points *payload at its JSON, 0 if it is incomplete,
 * -1 if it is larger than AEROSPACE_MAX_FRAME. */
long aero_next_frame(const char *data, size_t length, const char **payload,
    size_t *payloadlength);

/* A top-level member of a JSON object. Return 1 when found (string: *out is
 * malloc'd and NUL-terminated; number: an integer), 0 when absent or null,
 * -1 when the JSON is malformed or the member has another type. */
int aero_json_string(const char *json, size_t length, const char *key,
    char **out);
int aero_json_integer(const char *json, size_t length, const char *key,
    long long *out);

typedef struct {
	char *event;      /* "_event", e.g. "window-detected" */
	int haswindow;
	unsigned long long windowid;
	char *workspace;  /* NULL if absent */
} AeroEvent;

typedef struct {
	long long exitcode;
	char *out;        /* "stdout" */
	char *err;        /* "stderr" */
} AeroAnswer;

int aero_parse_event(const char *json, size_t length, AeroEvent *event);
void aero_free_event(AeroEvent *event);
int aero_parse_answer(const char *json, size_t length, AeroAnswer *answer);
void aero_free_answer(AeroAnswer *answer);

/* A slot file line, "PID WINDOW-ID STAMP". Returns 0 or -1. */
int launch_parse_slot(const char *line, pid_t *pid, unsigned long long *window,
    char *stamp, size_t stampsize);

/* `list-windows --focused --format '%{app-bundle-id} %{app-pid}'` output:
 * the PID if the focused window belongs to bundle, else -1. */
pid_t launch_focused_pid(const char *output, const char *bundle);

/* `list-windows --format '%{window-id} %{workspace}'` output: 1 and the
 * first line's workspace (malloc'd) if its window is window, else 0. */
int launch_window_workspace(const char *output, unsigned long long window,
    char **workspace);

/* The venv recorded by st-shell-context.zsh in context: its first complete
 * line, if the context is a regular file (not a symlink) and the line names
 * a directory with bin/activate. Returns 1 and the path (malloc'd), else 0. */
int launch_read_venv(const char *context, char **venv);

/* A positive decimal integer that fits in max, or -1. */
long long launch_parse_uint(const char *text, unsigned long long max);

#endif /* ST_MACOS_AEROSPACE_H */
