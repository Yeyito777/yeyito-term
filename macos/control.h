/* See LICENSE for license details. */

#ifndef ST_MACOS_CONTROL_H
#define ST_MACOS_CONTROL_H

#include <stddef.h>

/* Read one control-socket message: everything a client sends before it
 * closes. Returns a malloc'd buffer of *length bytes, or NULL if nothing
 * arrived, the message exceeds limit bytes, or the client stalls for a
 * second (an incomplete message is dropped, never handled). */
char *control_read_message(int fd, size_t limit, size_t *length);

#endif /* ST_MACOS_CONTROL_H */
