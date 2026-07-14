#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pty.h"

#define PTY_PENDING_LIMIT (16U * 1024U * 1024U)

typedef struct PtyChunk PtyChunk;
struct PtyChunk {
	PtyChunk *next;
	size_t offset;
	size_t length;
	char data[];
};

static dispatch_queue_t writerQueue;
static dispatch_source_t writerSource;
static int writerFD = -1;
static int sourceActive;
static int overflowReported;
static int writeErrorReported;
static PtyChunk *head;
static PtyChunk *tail;
static size_t pendingBytes;
static char writerQueueKey;

static void
clearPending(void)
{
	while (head) {
		PtyChunk *next = head->next;
		free(head);
		head = next;
	}
	tail = NULL;
	pendingBytes = 0;
}

static void
updateSource(void)
{
	if (!writerSource)
		return;
	if (pendingBytes && !sourceActive) {
		dispatch_resume(writerSource);
		sourceActive = 1;
	} else if (!pendingBytes && sourceActive) {
		dispatch_suspend(writerSource);
		sourceActive = 0;
	}
}

static void
flushPending(void)
{
	while (head && writerFD >= 0) {
		ssize_t written = write(writerFD, head->data + head->offset,
		    head->length - head->offset);
		if (written > 0) {
			head->offset += (size_t)written;
			pendingBytes -= (size_t)written;
			if (head->offset == head->length) {
				PtyChunk *finished = head;
				head = head->next;
				if (!head)
					tail = NULL;
				free(finished);
			}
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (!writeErrorReported) {
			fprintf(stderr, "st: PTY write failed: %s\n",
			    written < 0 ? strerror(errno) : "zero-length write");
			writeErrorReported = 1;
		}
		clearPending();
		break;
	}
	updateSource();
}

int
macos_pty_start(int fd)
{
	int flags;

	macos_pty_stop();
	if ((flags = fcntl(fd, F_GETFL)) < 0 ||
	    fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;

	writerFD = fd;
	sourceActive = 0;
	overflowReported = 0;
	writeErrorReported = 0;
	writerQueue = dispatch_queue_create("io.yeyito.st.pty-writer",
	    DISPATCH_QUEUE_SERIAL);
	if (!writerQueue) {
		writerFD = -1;
		errno = ENOMEM;
		return -1;
	}
	dispatch_queue_set_specific(writerQueue, &writerQueueKey,
	    &writerQueueKey, NULL);
	writerSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE,
	    (uintptr_t)fd, 0, writerQueue);
	if (!writerSource) {
		macos_pty_stop();
		errno = ENOMEM;
		return -1;
	}
	dispatch_source_set_event_handler(writerSource, ^{
		flushPending();
	});
	/* The write source stays suspended until backpressure queues data. */
	return 0;
}

void
macos_pty_write(const char *data, size_t length)
{
	dispatch_queue_t queue = writerQueue;
	PtyChunk *chunk;
	size_t copyLength;

	if (!queue || !data || !length)
		return;
	copyLength = length > PTY_PENDING_LIMIT ? PTY_PENDING_LIMIT : length;
	if (!(chunk = malloc(sizeof(*chunk) + copyLength))) {
		fprintf(stderr, "st: unable to queue %zu PTY output bytes\n", length);
		return;
	}
	chunk->next = NULL;
	chunk->offset = 0;
	chunk->length = copyLength;
	memcpy(chunk->data, data, copyLength);

	dispatch_async(queue, ^{
		if (writerFD < 0) {
			free(chunk);
			return;
		}
		size_t available = PTY_PENDING_LIMIT - pendingBytes;
		if (chunk->length > available) {
			chunk->length = available;
			if (!overflowReported) {
				fprintf(stderr, "st: PTY output queue full; dropping input\n");
				overflowReported = 1;
			}
		}
		if (!chunk->length) {
			free(chunk);
			return;
		}
		if (tail)
			tail->next = chunk;
		else
			head = chunk;
		tail = chunk;
		pendingBytes += chunk->length;
		flushPending();
	});
}

size_t
macos_pty_pending(void)
{
	__block size_t result = 0;
	dispatch_queue_t queue = writerQueue;
	if (!queue)
		return 0;
	if (dispatch_get_specific(&writerQueueKey))
		return pendingBytes;
	dispatch_sync(queue, ^{
		result = pendingBytes;
	});
	return result;
}

void
macos_pty_stop(void)
{
	dispatch_queue_t queue = writerQueue;
	if (!queue) {
		writerFD = -1;
		return;
	}
	void (^stop)(void) = ^{
		writerFD = -1;
		clearPending();
		if (writerSource) {
			/* A suspended source must be resumed before cancellation. */
			if (!sourceActive)
				dispatch_resume(writerSource);
			dispatch_source_cancel(writerSource);
			writerSource = nil;
		}
		sourceActive = 0;
	};
	if (dispatch_get_specific(&writerQueueKey))
		stop();
	else
		dispatch_sync(queue, stop);
	writerQueue = nil;
}
