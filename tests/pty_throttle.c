/* Run a command behind a slow link inside a real terminal.
 *
 *   pty_throttle [--bw KBIT] [--rtt MS] [--buffer BYTES] -- command...
 *
 * The command gets its own PTY; what it writes reaches our terminal at
 * KBIT kbit/s and RTT/2 later, what the terminal sends (keys, replies)
 * reaches it RTT/2 later, as over SSH.  At most BYTES (default 4 MiB) wait
 * in the link before the command's writes back up.  Window size changes
 * are passed on.  For st-play against a real st:
 *   st -e tests/pty_throttle --bw 3000 --rtt 150 -- ./st-play clip.mp4 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../play/selectpoll.h"

typedef struct Chunk {
	struct Chunk *next;
	double at;
	size_t length, offset;
	unsigned char data[];
} Chunk;

typedef struct {
	Chunk *head, *tail;
} Fifo;

static volatile sig_atomic_t winch;

static double
now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
push(Fifo *f, double at, const unsigned char *data, size_t length)
{
	Chunk *c = malloc(sizeof(*c) + length);
	if (!c)
		return;
	c->next = NULL;
	c->at = at;
	c->length = length;
	c->offset = 0;
	memcpy(c->data, data, length);
	if (f->tail)
		f->tail->next = c;
	else
		f->head = c;
	f->tail = c;
}

/* Write what is due; returns -1 if the fd is gone. */
static int
deliver(Fifo *f, int fd, double now)
{
	while (f->head && f->head->at <= now) {
		Chunk *c = f->head;
		ssize_t n = write(fd, c->data + c->offset, c->length - c->offset);
		if (n < 0)
			return errno == EAGAIN || errno == EINTR ? 0 : -1;
		c->offset += (size_t)n;
		if (c->offset < c->length)
			return 0;
		f->head = c->next;
		if (!f->head)
			f->tail = NULL;
		free(c);
	}
	return 0;
}

static void on_winch(int sig) { (void)sig; winch = 1; }

int
main(int argc, char **argv)
{
	double kbit = 0, owd = 0, buffer = 4 * 1024 * 1024, link_free = 0;
	int master, i, status = 0, exited = 0, raw = 0;
	struct termios saved, t;
	struct winsize ws;
	Fifo down = { 0 }, up = { 0 };
	char *slave;
	pid_t pid;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}
		if (i + 1 >= argc)
			break;
		if (!strcmp(argv[i], "--bw"))
			kbit = atof(argv[++i]);
		else if (!strcmp(argv[i], "--rtt"))
			owd = atof(argv[++i]) / 2000.0;
		else if (!strcmp(argv[i], "--buffer"))
			buffer = atof(argv[++i]);
		else
			break;
	}
	if (i >= argc) {
		fprintf(stderr, "usage: pty_throttle [--bw KBIT] [--rtt MS] "
		    "[--buffer BYTES] -- command...\n");
		return 2;
	}
	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0 ||
	    !(slave = ptsname(master))) {
		perror("pty_throttle: pty");
		return 1;
	}
	if (ioctl(0, TIOCGWINSZ, &ws) == 0)
		ioctl(master, TIOCSWINSZ, &ws);
	pid = fork();
	if (pid < 0) {
		perror("pty_throttle: fork");
		return 1;
	}
	if (pid == 0) {
		int s;
		setsid();
		s = open(slave, O_RDWR);
		if (s < 0)
			_exit(127);
#ifdef TIOCSCTTY
		ioctl(s, TIOCSCTTY, 0);
#endif
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		if (s > 2)
			close(s);
		close(master);
		execvp(argv[i], argv + i);
		_exit(127);
	}
	if (tcgetattr(0, &saved) == 0) {
		t = saved;
		t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
		    IGNCR | ICRNL | IXON);
		t.c_oflag &= ~(tcflag_t)OPOST;
		t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
		raw = tcsetattr(0, TCSANOW, &t) == 0;
	}
	signal(SIGWINCH, on_winch);
	signal(SIGPIPE, SIG_IGN);
	fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
	fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK);

	for (;;) {
		double now = now_s(), next = now + 0.05, backlog;
		struct pollfd fds[3];
		int timeout;
		unsigned char buf[65536];
		ssize_t n;

		if (winch) {
			winch = 0;
			if (ioctl(0, TIOCGWINSZ, &ws) == 0)
				ioctl(master, TIOCSWINSZ, &ws);
		}
		if (deliver(&down, 1, now) < 0 || deliver(&up, master, now) < 0)
			break;
		if (exited && !down.head)
			break;
		backlog = kbit > 0 ? (link_free - now) * kbit * 125 : 0;
		fds[0].fd = master;
		fds[0].events = !exited && backlog < buffer ? POLLIN : 0;
		fds[1].fd = 0;
		fds[1].events = POLLIN;
		fds[2].fd = 1;
		fds[2].events = down.head && down.head->at <= now ? POLLOUT : 0;
		if (down.head && down.head->at < next)
			next = down.head->at;
		if (up.head && up.head->at < next)
			next = up.head->at;
		timeout = (int)((next - now_s()) * 1000) + 1;
		if (timeout < 0)
			timeout = 0;
		if (select_poll(fds, 3, timeout) < 0 && errno != EINTR)
			break;
		now = now_s();
		if (fds[0].revents & POLLIN) {
			n = read(master, buf, sizeof(buf));
			if (n > 0) {
				double start = link_free > now ? link_free : now;
				link_free = kbit > 0 ? start + n * 8 / (kbit * 1000) : now;
				push(&down, link_free + owd, buf, (size_t)n);
			}
		}
		if (fds[1].revents & POLLIN) {
			n = read(0, buf, sizeof(buf));
			if (n > 0)
				push(&up, now + owd, buf, (size_t)n);
		}
		if (!exited && waitpid(pid, &status, WNOHANG) == pid) {
			exited = 1;
			while ((n = read(master, buf, sizeof(buf))) > 0)
				push(&down, now_s() + owd, buf, (size_t)n);
		}
	}
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) & ~O_NONBLOCK);
	fcntl(1, F_SETFL, fcntl(1, F_GETFL) & ~O_NONBLOCK);
	if (raw)
		tcsetattr(0, TCSAFLUSH, &saved);
	if (!exited) {
		kill(-pid, SIGHUP);
		waitpid(pid, &status, 0);
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
