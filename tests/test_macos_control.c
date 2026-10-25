/* See LICENSE for license details. */
/* Tests for reading control-socket messages (st-notify, spare spawn). */

#define _DARWIN_C_SOURCE

#include "test.h"
#include "macos/control.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int listener = -1;

typedef struct {
	const char *first;
	size_t firstLength;
	useconds_t gap;
	const char *second;
	size_t secondLength;
	useconds_t holdOpen;
} Sender;

static void *
send_message(void *argument)
{
	Sender *sender = argument;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	strlcpy(address.sun_path, path, sizeof(address.sun_path));
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
		if (sender->firstLength)
			write(fd, sender->first, sender->firstLength);
		usleep(sender->gap);
		if (sender->secondLength)
			write(fd, sender->second, sender->secondLength);
		usleep(sender->holdOpen);
	}
	close(fd);
	return NULL;
}

/* Like st: a non-blocking listener, whose accepted sockets inherit that. */
static int
accept_from(Sender *sender, pthread_t *thread)
{
	pthread_create(thread, NULL, send_message, sender);
	for (int attempt = 0; attempt < 1000; attempt++) {
		int client = accept(listener, NULL, NULL);
		if (client >= 0) return client;
		usleep(1000);
	}
	return -1;
}

static char *
receive(Sender *sender, size_t limit, size_t *length, int *inherited)
{
	pthread_t thread;
	int client = accept_from(sender, &thread);
	if (client < 0) return NULL;
	*inherited = (fcntl(client, F_GETFL) & O_NONBLOCK) != 0;
	char *message = control_read_message(client, limit, length);
	close(client);
	pthread_join(thread, NULL);
	return message;
}

TEST(reads_message_split_by_a_gap)
{
	/* A spawn request arrives in pieces; the first gap is not the end. */
	static char environment[3000];
	memset(environment, 'e', sizeof(environment));
	Sender sender = {environment, sizeof(environment), 100000, "end", 3, 0};
	size_t length = 0;
	int inherited = 0;
	char *message = receive(&sender, 1024 * 1024, &length, &inherited);
	ASSERT(inherited);
	ASSERT(message != NULL);
	ASSERT_EQ(3003, (int)length);
	ASSERT(!memcmp(message + 3000, "end", 3));
	free(message);
}

TEST(grows_past_its_initial_buffer)
{
	size_t size = 200 * 1024;
	char *big = malloc(size);
	for (size_t i = 0; i < size; i++) big[i] = 'a' + i % 26;
	Sender sender = {big, size, 0, NULL, 0, 0};
	size_t length = 0;
	int inherited;
	char *message = receive(&sender, 1024 * 1024, &length, &inherited);
	ASSERT(message != NULL);
	ASSERT_EQ((int)size, (int)length);
	ASSERT(!memcmp(message, big, size));
	free(message);
	free(big);
}

TEST(drops_oversized_messages)
{
	static char big[4000];
	memset(big, 'x', sizeof(big));
	Sender sender = {big, sizeof(big), 0, NULL, 0, 0};
	size_t length;
	int inherited;
	ASSERT(receive(&sender, 1000, &length, &inherited) == NULL);
}

TEST(ignores_empty_connections)
{
	Sender sender = {NULL, 0, 0, NULL, 0, 0};
	size_t length;
	int inherited;
	ASSERT(receive(&sender, 1024, &length, &inherited) == NULL);
}

TEST(drops_a_stalled_partial_message)
{
	/* Half a request, then silence past the one-second limit. */
	Sender sender = {"spawn\ncwd=/", 11, 0, NULL, 0, 1500000};
	size_t length;
	int inherited;
	ASSERT(receive(&sender, 1024, &length, &inherited) == NULL);
}

TEST_SUITE(macos_control)
{
	RUN_TEST(reads_message_split_by_a_gap);
	RUN_TEST(grows_past_its_initial_buffer);
	RUN_TEST(drops_oversized_messages);
	RUN_TEST(ignores_empty_connections);
	RUN_TEST(drops_a_stalled_partial_message);
}

int
main(void)
{
	/* A reader that gives up early must fail the test, not kill it. */
	signal(SIGPIPE, SIG_IGN);
	snprintf(path, sizeof(path), "/tmp/st-control-test.%d.sock", (int)getpid());
	unlink(path);
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	strlcpy(address.sun_path, path, sizeof(address.sun_path));
	if (listener < 0 || bind(listener, (struct sockaddr *)&address,
	    sizeof(address)) < 0 || listen(listener, 8) < 0)
		return 1;
	fcntl(listener, F_SETFL, O_NONBLOCK);
	RUN_SUITE(macos_control);
	close(listener);
	unlink(path);
	return test_summary();
}
