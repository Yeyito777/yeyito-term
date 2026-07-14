/* See LICENSE for license details. */
/* Regression test for native macOS PTY write backpressure. */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "test.h"
#include "../macos/pty.h"

#define PAYLOAD_SIZE (1024U * 1024U)

static double
monotonicSeconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec + now.tv_nsec / 1e9;
}

TEST(nonblocking_write_survives_backpressure)
{
	int sockets[2], bufferSize = 4096;
	unsigned char *payload, *received;
	size_t receivedLength = 0;
	double started, deadline;

	ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
	ASSERT_EQ(0, setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
	    &bufferSize, sizeof(bufferSize)));
	ASSERT_EQ(0, macos_pty_start(sockets[0]));
	ASSERT(fcntl(sockets[0], F_GETFL) & O_NONBLOCK);
	ASSERT((payload = malloc(PAYLOAD_SIZE)) != NULL);
	ASSERT((received = malloc(PAYLOAD_SIZE)) != NULL);
	for (size_t i = 0; i < PAYLOAD_SIZE; i++)
		payload[i] = (unsigned char)((i * 37U + 11U) & 0xffU);

	started = monotonicSeconds();
	macos_pty_write((const char *)payload, PAYLOAD_SIZE);
	ASSERT(monotonicSeconds() - started < 0.5);
	/* Synchronizes with the writer queue after the first nonblocking flush. */
	ASSERT(macos_pty_pending() > 0);
	ASSERT_EQ(0, fcntl(sockets[1], F_SETFL,
	    fcntl(sockets[1], F_GETFL) | O_NONBLOCK));

	deadline = monotonicSeconds() + 5.0;
	while (receivedLength < PAYLOAD_SIZE && monotonicSeconds() < deadline) {
		ssize_t count = read(sockets[1], received + receivedLength,
		    PAYLOAD_SIZE - receivedLength);
		if (count > 0) {
			receivedLength += (size_t)count;
			continue;
		}
		if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
		    errno != EINTR)
			break;
		usleep(1000);
	}
	ASSERT_EQ(PAYLOAD_SIZE, receivedLength);
	ASSERT(memcmp(payload, received, PAYLOAD_SIZE) == 0);
	deadline = monotonicSeconds() + 1.0;
	while (macos_pty_pending() && monotonicSeconds() < deadline)
		usleep(1000);
	ASSERT_EQ(0, macos_pty_pending());

	macos_pty_stop();
	close(sockets[0]);
	close(sockets[1]);
	free(payload);
	free(received);
}

TEST_SUITE(macos_pty)
{
	RUN_TEST(nonblocking_write_survives_backpressure);
}

int
main(void)
{
	RUN_SUITE(macos_pty);
	return test_summary();
}
