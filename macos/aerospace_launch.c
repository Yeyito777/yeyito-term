/* See LICENSE for license details. */
/*
 * st-aerospace-launch: open an st window on the focused AeroSpace workspace,
 * focused and revealed.
 *
 * Usage: st-aerospace-launch [--inherit-cwd | --cwd DIR] [-T TITLE]
 *                            [-e COMMAND [ARG ...]]
 *
 * The expensive part of a launch (AppKit, Metal, fonts, window; ~0.5 s) is
 * done ahead of time by a warm spare: `st --spare` waits, windowless and
 * shell-less, in $HOME/.runtime/st/spare. The launcher claims it, sends the
 * directory/command/title over its control socket, waits for AeroSpace's
 * window-detected event and focuses it. Afterwards it boots the next spare.
 * Without a spare it boots a private one and continues the same way.
 *
 * It talks to AeroSpace's server socket directly (one connection subscribed
 * to events, one for commands) and runs no other process on the way to a
 * focused window: the Swift CLI alone costs tens of milliseconds per call.
 * See reference/warm-spare.md.
 *
 * Environment: ST_BINARY (the st executable), ST_AEROSPACE_SOCKET (the
 * server socket, default /tmp/bobko.aerospace-$USER.sock),
 * ST_AEROSPACE_KEEP_SPARE=0 (no spare), ST_AEROSPACE_TICK and
 * ST_AEROSPACE_MAX_TICKS (the fallback query unit and the wait bound, in
 * ticks), ST_AEROSPACE_TRACE=1 (step timings on stderr).
 */

#include "aerospace.h"
#include "spare.h"

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define ST_BUNDLE_ID "io.yeyito.st"
#define SUN_PATH_SIZE sizeof(((struct sockaddr_un *)0)->sun_path)

typedef struct {
	int fd;
	int greeted;     /* the server's protocol version has arrived */
	AeroBuffer in;
} Connection;

static char socketpath[SUN_PATH_SIZE];
static Connection events = {.fd = -1}, commands = {.fd = -1};
static long long envwindow = -1;      /* AEROSPACE_WINDOW_ID, like the CLI */
static const char *envworkspace;      /* AEROSPACE_WORKSPACE */
static double tick = 0.25;
static int maxticks = 24;
static int tracing;
static double started;                /* process start, wall clock */

/* What fail() and the signal handler clean up. */
static volatile sig_atomic_t claimed; /* st to kill on failure */
static char privateslot[PATH_MAX];
static volatile sig_atomic_t hasprivateslot;

static double
now(void)
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec + time.tv_nsec / 1e9;
}

static double
bound(void)
{
	return tick * maxticks;
}

static void
trace(const char *step)
{
	if (!tracing)
		return;
	struct timeval time;
	gettimeofday(&time, NULL);
	fprintf(stderr, "st-aerospace-launch: %8.2f ms  %s\n",
	    (time.tv_sec + time.tv_usec / 1e6 - started) * 1e3, step);
}

static void
cleanup(void)
{
	if (events.fd >= 0) {
		close(events.fd);
		events.fd = -1;
	}
	if (hasprivateslot) {
		unlink(privateslot);
		hasprivateslot = 0;
	}
}

static void __attribute__((noreturn, format(printf, 1, 2)))
fail(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	if (claimed > 0)
		kill(claimed, SIGTERM);
	cleanup();
	exit(1);
}

static void
interrupted(int signal)
{
	(void)signal;
	static const char message[] = "st-aerospace-launch interrupted\n";
	write(STDERR_FILENO, message, sizeof(message) - 1);
	if (claimed > 0)
		kill(claimed, SIGTERM);
	if (hasprivateslot)
		unlink(privateslot);
	_exit(1);
}

static void
handle_interrupts(void (*handler)(int))
{
	struct sigaction action = {.sa_handler = handler};
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
}

static int
write_all(int fd, const char *data, size_t length)
{
	while (length) {
		ssize_t written = write(fd, data, length);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		data += written;
		length -= written;
	}
	return 0;
}

static int
connect_unix(const char *path, double timeout)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	if (strlen(path) >= sizeof(address.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(address.sun_path, path);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	int one = 1;
	struct timeval limit = {.tv_sec = (time_t)timeout,
	    .tv_usec = (suseconds_t)((timeout - (time_t)timeout) * 1e6)};
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		int error = errno;
		close(fd);
		errno = error;
		return -1;
	}
	return fd;
}

static void
connection_close(Connection *connection)
{
	if (connection->fd >= 0)
		close(connection->fd);
	connection->fd = -1;
	connection->greeted = 0;
	connection->in.length = 0;
}

/* Connect and send the protocol version and, pipelined behind it, the
 * first request. The server's version is read with the first answer. */
static int
connection_open(Connection *connection, const AeroBuffer *request)
{
	connection_close(connection);
	int fd = connect_unix(socketpath, bound());
	if (fd < 0)
		return -1;
	uint32_t version = AEROSPACE_PROTOCOL_VERSION;
	AeroBuffer out = {0};
	int ok = aero_buffer_append(&out, &version, sizeof(version)) == 0 &&
	    (!request || aero_buffer_append(&out, request->data,
	    request->length) == 0) && write_all(fd, out.data, out.length) == 0;
	aero_buffer_free(&out);
	if (!ok) {
		close(fd);
		return -1;
	}
	connection->fd = fd;
	return 0;
}

static void
consume(AeroBuffer *buffer, size_t n)
{
	memmove(buffer->data, buffer->data + n, buffer->length - n);
	buffer->length -= n;
}

/* The next message. Returns 1 with *payload (malloc'd, NUL-terminated), 0
 * if the deadline passed, -1 if the connection is closed or broken. */
static int
connection_next(Connection *connection, double deadline, char **payload)
{
	for (;;) {
		if (!connection->greeted && connection->in.length >= 4) {
			uint32_t version;
			memcpy(&version, connection->in.data, sizeof(version));
			if (version != AEROSPACE_PROTOCOL_VERSION) {
				fprintf(stderr, "AeroSpace server protocol version %u "
				    "is incompatible with %u (restart AeroSpace)\n",
				    version, AEROSPACE_PROTOCOL_VERSION);
				return -1;
			}
			consume(&connection->in, sizeof(version));
			connection->greeted = 1;
		}
		if (connection->greeted) {
			const char *frame;
			size_t length;
			long used = aero_next_frame(connection->in.data,
			    connection->in.length, &frame, &length);
			if (used < 0)
				return -1;
			if (used > 0) {
				*payload = malloc(length + 1);
				if (!*payload)
					return -1;
				memcpy(*payload, frame, length);
				(*payload)[length] = '\0';
				consume(&connection->in, used);
				return 1;
			}
		}
		double left = deadline - now();
		if (left <= 0)
			return 0;
		struct pollfd ready = {.fd = connection->fd, .events = POLLIN};
		int polled = poll(&ready, 1, (int)(left * 1e3) + 1);
		if (polled < 0 && errno != EINTR)
			return -1;
		if (polled <= 0)
			continue;
		char chunk[16384];
		ssize_t got = read(connection->fd, chunk, sizeof(chunk));
		if (got < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (got <= 0 || aero_buffer_append(&connection->in, chunk, got) < 0)
			return -1;
	}
}

/* Run an AeroSpace command (NULL-terminated arguments) on the command
 * connection, reconnecting if it broke. Returns 0 with *answer, or -1. */
static int __attribute__((sentinel))
command(AeroAnswer *answer, ...)
{
	const char *args[16];
	int n = 0;
	va_list arguments;
	va_start(arguments, answer);
	for (const char *arg; (arg = va_arg(arguments, const char *)) &&
	    n < (int)(sizeof(args) / sizeof(*args));)
		args[n++] = arg;
	va_end(arguments);

	AeroBuffer request = {0};
	if (aero_frame_request(&request, args, n, envwindow, envworkspace) < 0)
		return -1;
	int sent = commands.fd >= 0 &&
	    write_all(commands.fd, request.data, request.length) == 0;
	if (!sent)
		sent = connection_open(&commands, &request) == 0;
	aero_buffer_free(&request);
	if (!sent)
		return -1;
	char *payload;
	if (connection_next(&commands, now() + bound(), &payload) <= 0) {
		/* A late answer would be mistaken for the next one's. */
		connection_close(&commands);
		return -1;
	}
	int parsed = aero_parse_answer(payload, strlen(payload), answer);
	free(payload);
	return parsed;
}

/* Wait for the event name (about window, if not NULL; with a workspace, if
 * needworkspace). Returns 1 with *workspace (malloc'd or NULL), 0 if the
 * deadline passed, -1 if the subscription is gone. */
static int
wait_event(const char *name, const unsigned long long *window,
    int needworkspace, double deadline, char **workspace)
{
	for (;;) {
		char *payload;
		int got = connection_next(&events, deadline, &payload);
		if (got <= 0)
			return got;
		size_t length = strlen(payload);
		AeroEvent event;
		AeroAnswer answer;
		if (aero_parse_event(payload, length, &event) < 0) {
			/* An answer instead of events: the subscription failed. */
			int refused = aero_parse_answer(payload, length, &answer) == 0;
			if (refused)
				aero_free_answer(&answer);
			free(payload);
			if (refused)
				return -1;
			continue;
		}
		free(payload);
		if (!strcmp(event.event, name) &&
		    (!window || (event.haswindow && event.windowid == *window)) &&
		    (!needworkspace || event.workspace)) {
			*workspace = event.workspace;
			event.workspace = NULL;
			aero_free_event(&event);
			return 1;
		}
		aero_free_event(&event);
	}
}

static void
sleep_until(double deadline)
{
	double left;
	while ((left = deadline - now()) > 0)
		poll(NULL, 0, (int)(left * 1e3) + 1);
}

static int
is_directory(const char *path)
{
	struct stat info;
	return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/* The focused native st window's PTY child: its directory and the venv its
 * shell recorded. Each st window owns one process and one direct PTY
 * child; inspect that child, not st's launch-time cwd or unrelated jobs. */
static void
inherit_cwd(const char *home, char **directory, char **venv)
{
	AeroAnswer answer;
	if (command(&answer, "list-windows", "--focused", "--format",
	    "%{app-bundle-id} %{app-pid}", NULL) < 0)
		return;
	pid_t terminal = answer.out ? launch_focused_pid(answer.out,
	    ST_BUNDLE_ID) : -1;
	aero_free_answer(&answer);
	if (terminal <= 0)
		return;

	pid_t children[256], child = 0;
	int n = proc_listchildpids(terminal, children, sizeof(children));
	for (int i = 0; i < n && i < (int)(sizeof(children) / sizeof(*children));
	    i++) {
		if (children[i] > 0 && (!child || children[i] < child))
			child = children[i];
	}
	if (!child)
		return;
	struct proc_vnodepathinfo paths;
	if (proc_pidinfo(child, PROC_PIDVNODEPATHINFO, 0, &paths,
	    sizeof(paths)) == sizeof(paths) && paths.pvi_cdir.vip_path[0] &&
	    is_directory(paths.pvi_cdir.vip_path))
		*directory = strdup(paths.pvi_cdir.vip_path);
	char context[PATH_MAX];
	if (snprintf(context, sizeof(context),
	    "%s/.cache/st-shell-context/%d-%d.venv", home, (int)terminal,
	    (int)child) < (int)sizeof(context))
		launch_read_venv(context, venv);
}

static int
read_line(const char *path, char *line, size_t size)
{
	FILE *file = fopen(path, "r");
	line[0] = '\0';
	if (!file)
		return -1;
	if (!fgets(line, size, file))
		line[0] = '\0';
	fclose(file);
	return 0;
}

/* Claim the warm spare. rename(2) is atomic, so concurrent launchers can
 * never claim the same one; a dead or outdated spare is discarded. */
/* Whether pid runs with a --spare/--spare-slot argument (KERN_PROCARGS2) */
static int
is_spare_process(pid_t pid)
{
	int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
	size_t size = 0;
	if (sysctl(mib, 3, NULL, &size, NULL, 0) < 0 || !size)
		return 0;
	char *arguments = malloc(size);
	if (!arguments)
		return 0;
	int spare = sysctl(mib, 3, arguments, &size, NULL, 0) == 0 &&
	    launch_args_name_spare(arguments, size);
	free(arguments);
	return spare;
}

static int
claim_spare(const char *runtime, const char *slot, const char *stbinary,
    pid_t *pid, unsigned long long *window)
{
	char claim[PATH_MAX], line[256], stamp[128], sparestamp[128];
	if (spare_exe_stamp(stbinary, stamp, sizeof(stamp)) < 0)
		stamp[0] = '\0';
	snprintf(claim, sizeof(claim), "%s/spare.claim.%d", runtime,
	    (int)getpid());
	if (rename(slot, claim) < 0)
		return 0;
	read_line(claim, line, sizeof(line));
	unlink(claim);

	pid_t spare;
	unsigned long long id;
	if (launch_parse_slot(line, &spare, &id, sparestamp,
	    sizeof(sparestamp)) == 0 && *stamp && !strcmp(sparestamp, stamp) &&
	    kill(spare, 0) == 0) {
		char socket[PATH_MAX];
		struct stat info;
		snprintf(socket, sizeof(socket), "%s/st-%d/control.sock", runtime,
		    (int)spare);
		if (stat(socket, &info) == 0 && S_ISSOCK(info.st_mode)) {
			*pid = spare;
			*window = id;
			return 1;
		}
	}
	/* An outdated or broken spare is stopped, but only if the pid still
	 * runs an st: the slot may outlive its spare and the pid be reused. */
	int stale;
	if (sscanf(line, "%d", &stale) == 1 && stale > 1 && is_spare_process(stale))
		kill(stale, SIGTERM);
	return 0;
}

/* Start st detached: its own session, no inherited descriptors or signal
 * state, in $HOME. */
static pid_t
spawn_st(const char *stbinary, const char *home, const char *option,
    const char *slot)
{
	const char *argv[] = {stbinary, option, slot, NULL};
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	sigset_t none, all;
	sigemptyset(&none);
	sigfillset(&all);
	sigdelset(&all, SIGKILL);
	sigdelset(&all, SIGSTOP);
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, 1, 2);
	posix_spawn_file_actions_addchdir_np(&actions, home);
	posix_spawnattr_init(&attributes);
	posix_spawnattr_setsigmask(&attributes, &none);
	posix_spawnattr_setsigdefault(&attributes, &all);
	posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID |
	    POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
	    POSIX_SPAWN_CLOEXEC_DEFAULT);
	pid_t pid;
	int error = posix_spawn(&pid, stbinary, &actions, &attributes,
	    (char *const *)argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attributes);
	return error ? -1 : pid;
}

static int
send_request(const char *runtime, pid_t pid, const char *request,
    size_t length)
{
	char path[PATH_MAX];
	if (snprintf(path, sizeof(path), "%s/st-%d/control.sock", runtime,
	    (int)pid) >= (int)sizeof(path))
		return -1;
	int fd = connect_unix(path, bound());
	if (fd < 0)
		return -1;
	int ok = write_all(fd, request, length) == 0;
	close(fd);
	return ok ? 0 : -1;
}

static void
usage(const char *argv0)
{
	fprintf(stderr, "Usage: %s [--inherit-cwd | --cwd DIR] [-T TITLE] "
	    "[-e COMMAND [ARG ...]]\n", argv0);
	exit(64);
}

int
main(int argc, char **argv)
{
	const char *cwd = NULL, *title = NULL;
	char **commandargv = NULL;
	int inherit = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--inherit-cwd")) {
			inherit = 1;
		} else if (!strcmp(argv[i], "--cwd") && i + 1 < argc) {
			cwd = argv[++i];
		} else if (!strcmp(argv[i], "-T") && i + 1 < argc) {
			title = argv[++i];
		} else if (!strcmp(argv[i], "-e")) {
			if (i + 1 < argc)
				commandargv = argv + i + 1;
			break;
		} else {
			usage(argv[0]);
		}
	}

	const char *value;
	if ((value = getenv("ST_AEROSPACE_TRACE")) && *value && strcmp(value, "0")) {
		struct proc_bsdinfo info;
		struct timeval time;
		tracing = 1;
		gettimeofday(&time, NULL);
		started = time.tv_sec + time.tv_usec / 1e6;
		if (proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &info,
		    sizeof(info)) == sizeof(info))
			started = info.pbi_start_tvsec + info.pbi_start_tvusec / 1e6;
		trace("main");
	}
	if ((value = getenv("ST_AEROSPACE_TICK")) && atof(value) > 0)
		tick = atof(value);
	if ((value = getenv("ST_AEROSPACE_MAX_TICKS")) && atoi(value) > 0)
		maxticks = atoi(value);
	value = getenv("ST_AEROSPACE_KEEP_SPARE");
	int keepspare = !value || !strcmp(value, "1");
	envwindow = launch_parse_uint(getenv("AEROSPACE_WINDOW_ID"), UINT32_MAX);
	envworkspace = getenv("AEROSPACE_WORKSPACE");

	const char *home = getenv("HOME");
	if (!home || !*home) {
		fprintf(stderr, "HOME is not set\n");
		return 1;
	}
	char defaultbinary[PATH_MAX];
	const char *stbinary = getenv("ST_BINARY");
	if (!stbinary || !*stbinary) {
		snprintf(defaultbinary, sizeof(defaultbinary),
		    "%s/Applications/st.app/Contents/MacOS/st", home);
		stbinary = defaultbinary;
	}
	if (access(stbinary, X_OK) < 0) {
		fprintf(stderr, "st is not installed at %s\n", stbinary);
		return 1;
	}

	/* Subscribe first: the first event is the focus at invocation time,
	 * the workspace this terminal belongs on even if the user switches
	 * meanwhile. Both connections are answered while we claim the spare. */
	value = getenv("ST_AEROSPACE_SOCKET");
	if (value && *value) {
		if (strlen(value) >= sizeof(socketpath)) {
			fprintf(stderr, "AeroSpace socket path is too long: %s\n", value);
			return 1;
		}
		strcpy(socketpath, value);
	} else {
		struct passwd *user = getpwuid(getuid());
		const char *name = user ? user->pw_name : getenv("USER");
		if (snprintf(socketpath, sizeof(socketpath),
		    "/tmp/bobko.aerospace-%s.sock", name ? name : "") >=
		    (int)sizeof(socketpath)) {
			fprintf(stderr, "AeroSpace socket path is too long\n");
			return 1;
		}
	}
	static const char *const subscribe[] = {"subscribe", "focus-changed",
	    "window-detected"};
	AeroBuffer request = {0};
	if (aero_frame_request(&request, subscribe, 3, envwindow,
	    envworkspace) < 0 || connection_open(&events, &request) < 0 ||
	    connection_open(&commands, NULL) < 0) {
		fprintf(stderr, "Can't connect to the AeroSpace server at %s (%s). "
		    "Is AeroSpace.app running?\n", socketpath, strerror(errno));
		return 1;
	}
	aero_buffer_free(&request);
	trace("connected");

	char runtime[PATH_MAX], slot[PATH_MAX];
	if (spare_runtime_dir(runtime, sizeof(runtime)) < 0 ||
	    snprintf(slot, sizeof(slot), "%s/spare", runtime) >=
	    (int)sizeof(slot)) {
		fprintf(stderr, "Could not create %s/.runtime/st\n", home);
		return 1;
	}
	handle_interrupts(interrupted);

	char *directory = NULL, *venv = NULL;
	if (cwd)
		directory = strdup(cwd);
	if (inherit) {
		inherit_cwd(home, &directory, &venv);
		trace("inherited cwd");
	}
	if (!directory || !is_directory(directory)) {
		free(directory);
		directory = strdup(home);
	}

	pid_t pid = 0;
	unsigned long long window = 0;
	if (claim_spare(runtime, slot, stbinary, &pid, &window)) {
		claimed = pid;
		trace("claimed spare");
	} else {
		/* Cold path: boot a private spare and wait until it serves
		 * requests. */
		snprintf(privateslot, sizeof(privateslot), "%s/spare.private.%d",
		    runtime, (int)getpid());
		hasprivateslot = 1;
		pid_t booting = spawn_st(stbinary, home, "--spare-slot",
		    privateslot);
		if (booting < 0)
			fail("st did not start");
		claimed = booting;
		struct stat info;
		for (int attempt = 0; attempt < 800; attempt++) {
			if ((stat(privateslot, &info) == 0 && info.st_size > 0) ||
			    waitpid(booting, NULL, WNOHANG) != 0)
				break;
			usleep(5000);
		}
		char line[256], stamp[128];
		read_line(privateslot, line, sizeof(line));
		unlink(privateslot);
		hasprivateslot = 0;
		if (launch_parse_slot(line, &pid, &window, stamp, sizeof(stamp)) < 0)
			fail("st did not start");
		claimed = pid;
		trace("booted private spare");
		/* AeroSpace gives up registering an app that is busy launching;
		 * ask again now that it answers, so it sees the window as soon as
		 * it opens. */
		AeroAnswer answer;
		if (command(&answer, "list-apps", NULL) == 0)
			aero_free_answer(&answer);
	}

	char *target = NULL;
	int got = wait_event("focus-changed", NULL, 1, now() + bound(), &target);
	if (got < 0)
		connection_close(&events);
	if (got != 1) {
		AeroAnswer answer;
		if (command(&answer, "list-workspaces", "--focused", NULL) == 0) {
			if (answer.out)
				target = strndup(answer.out, strcspn(answer.out, "\n"));
			aero_free_answer(&answer);
		}
	}
	if (!target || !*target)
		fail("Could not determine the focused AeroSpace workspace");
	trace("focused workspace");

	/* The terminal gets this launcher's environment, as if it had started
	 * st itself; the spare's own is just whatever booted it. */
	char *venvassignment;
	if (asprintf(&venvassignment, "ST_INHERIT_VIRTUAL_ENV=%s",
	    venv ? venv : "") < 0)
		fail("Out of memory");
	char *env[] = {venvassignment, NULL};
	size_t length;
	char *spawn = spare_build_request(directory, environ, env, title,
	    commandargv, &length);
	if (!spawn || send_request(runtime, pid, spawn, length) < 0)
		fail("Could not reach st process %d", (int)pid);
	free(spawn);
	trace("sent spawn request");

	/* Wait for AeroSpace to report the window. Without an event, query
	 * after 2, 4, 8, ... ticks: each query's session ends in the full
	 * refresh that registers apps and detects windows, and also cancels a
	 * refresh still running, so polling faster delays detection. */
	char *workspace = NULL, pidtext[24];
	snprintf(pidtext, sizeof(pidtext), "%d", (int)pid);
	double start = now(), deadline = start + bound();
	int detected = 0;
	for (int nextquery = 2; !detected;) {
		double query = start + tick * nextquery;
		double wake = query < deadline ? query : deadline;
		if (events.fd >= 0) {
			got = wait_event("window-detected", &window, 0, wake, &workspace);
			if (got == 1) {
				detected = 1;
				break;
			}
			if (got < 0)
				connection_close(&events);
		}
		if (events.fd < 0)
			sleep_until(wake);
		if (wake >= deadline)
			break;
		nextquery *= 2;
		AeroAnswer answer;
		if (command(&answer, "list-windows", "--monitor", "all", "--pid",
		    pidtext, "--format", "%{window-id} %{workspace}", NULL) == 0) {
			detected = answer.out &&
			    launch_window_workspace(answer.out, window, &workspace);
			aero_free_answer(&answer);
		}
	}
	if (!detected)
		fail("AeroSpace did not register the st window for PID %d", (int)pid);
	trace("window detected");

	char windowtext[24];
	snprintf(windowtext, sizeof(windowtext), "%llu", window);
	int focus = workspace && !strcmp(workspace, target);
	AeroAnswer answer;
	int ok = focus
	    ? command(&answer, "focus", "--window-id", windowtext, NULL) == 0
	    : command(&answer, "move-node-to-workspace", "--focus-follows-window",
	      "--window-id", windowtext, target, NULL) == 0;
	if (ok) {
		ok = answer.exitcode == 0;
		aero_free_answer(&answer);
	}
	if (!ok && focus)
		fail("AeroSpace could not focus st window %s", windowtext);
	if (!ok)
		fail("AeroSpace could not move st window %s to workspace %s",
		    windowtext, target);
	trace("focused window");

	/* Managed st windows stay transparent until AeroSpace has finished
	 * both layout and native focus. SIGUSR1 completes that handshake
	 * without a timing guess. */
	if (kill(pid, SIGUSR1) < 0)
		fail("st process %d exited before its window could be revealed",
		    (int)pid);
	claimed = 0;
	cleanup();
	handle_interrupts(SIG_DFL);
	trace("revealed");

	/* Boot the next spare. Once it serves requests, ask AeroSpace to look
	 * at the running apps again: it skipped this one while it was busy
	 * launching, and a registered app's new window is reported at once. */
	struct stat info;
	if (keepspare && stat(slot, &info) < 0) {
		pid_t spare = spawn_st(stbinary, home, "--spare", NULL);
		if (spare > 0) {
			for (int attempt = 0; attempt < 1000; attempt++) {
				if (waitpid(spare, NULL, WNOHANG) != 0 ||
				    spare_holds_slot(slot, spare))
					break;
				usleep(5000);
			}
			if (command(&answer, "list-apps", NULL) == 0)
				aero_free_answer(&answer);
			trace("spare ready");
		}
	}
	return 0;
}
