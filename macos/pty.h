#ifndef ST_MACOS_PTY_H
#define ST_MACOS_PTY_H

#include <stddef.h>

/*
 * Native macOS PTY output bridge. Writes are copied into an ordered,
 * nonblocking queue so AppKit event handlers can never stall on a child that
 * has temporarily stopped reading its terminal input.
 */
int macos_pty_start(int fd);
void macos_pty_stop(void);
void macos_pty_write(const char *data, size_t length);

/* Exposed for diagnostics and the backpressure regression test. */
size_t macos_pty_pending(void);

#endif
