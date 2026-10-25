/* See LICENSE for license details. */
/*
 * Launch diagnostics: one key=value line per launch, appended to
 * $HOME/Library/Logs/st/launch.log by st-aerospace-launch (src=launcher) and
 * by a claimed spare ~3 s after its claim (src=st), so that an occasional
 * slow launch can be explained afterwards. See reference/warm-spare.md.
 */

#ifndef ST_MACOS_LAUNCHLOG_H
#define ST_MACOS_LAUNCHLOG_H

#include <stddef.h>
#include <sys/types.h>

/* Past this size the log is renamed to launch.log.1 (replacing it). */
#define LAUNCHLOG_LIMIT (1024 * 1024)

typedef struct {
	char text[1024];
	size_t length;
} LaunchLine;

/* Start a line: "<local time with ms and offset> st=<pid> src=<source>".
 * wallclock is seconds since the epoch. */
void launchlog_begin(LaunchLine *line, double wallclock, pid_t stpid,
    const char *source);

/* Append " key=value". Control characters become spaces; a value that is
 * empty or contains spaces, quotes or '=' is double-quoted, with its own
 * double quotes turned into single ones. A field that does not fit is left
 * out. */
void launchlog_put(LaunchLine *line, const char *key, const char *value);
void launchlog_putf(LaunchLine *line, const char *key, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

/* Append " key=<ms with one decimal>", or nothing if ms < 0 (not reached). */
void launchlog_ms(LaunchLine *line, const char *key, double ms);

/* Whether a log of size bytes must be rotated before adding bytes more. */
int launchlog_should_rotate(long long size, size_t adding, long long limit);

/* $HOME/Library/Logs/st/launch.log. Returns 0, or -1 without HOME. */
int launchlog_path(char *buffer, size_t size);

/* Append the line and a newline to path in one write (lines of concurrent
 * writers never interleave), creating its directory and rotating it past
 * limit. Returns 0 or -1. */
int launchlog_append(const char *path, const LaunchLine *line, long long limit);

#endif /* ST_MACOS_LAUNCHLOG_H */
