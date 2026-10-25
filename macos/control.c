/* See LICENSE for license details. */

#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

char *
control_read_message(int fd, size_t limit, size_t *length)
{
	/* accept() inherits the listener's O_NONBLOCK: reads would stop at the
	 * first gap and cut longer messages short. Block, but bounded. */
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	size_t capacity = 16 * 1024, total = 0;
	char *buffer = malloc(capacity);
	while (buffer) {
		if (total == capacity) {
			char *grown = capacity < limit ? realloc(buffer, capacity * 2) : NULL;
			if (!grown) break;
			buffer = grown;
			capacity *= 2;
		}
		ssize_t got = read(fd, buffer + total, capacity - total);
		if (got > 0) {
			total += got;
		} else if (got < 0 && errno == EINTR) {
			continue;
		} else {
			if (got == 0 && total && total <= limit) {
				*length = total;
				return buffer;
			}
			break;
		}
	}
	free(buffer);
	return NULL;
}
