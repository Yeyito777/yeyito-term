/* See LICENSE for license details. */

#include "spare.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int
valid_assignment(const char *assignment)
{
	return assignment[0] != '=' && strchr(assignment, '=') != NULL;
}

int
spare_parse_request(const char *payload, size_t length, SpareRequest *request)
{
	memset(request, 0, sizeof(*request));
	if (length && payload[length - 1] != '\0')
		return -1;
	size_t fields = 0;
	for (size_t i = 0; i < length; i++)
		fields += payload[i] == '\0';
	request->storage = malloc(length + 1);
	request->env = calloc(fields + 1, sizeof(*request->env));
	request->argv = calloc(fields + 1, sizeof(*request->argv));
	if (!request->storage || !request->env || !request->argv)
		goto invalid;
	memcpy(request->storage, payload, length);
	request->storage[length] = '\0';

	int inenviron = 0, ended = 0;
	for (char *field = request->storage; field < request->storage + length;
	    field += strlen(field) + 1) {
		if (ended)
			goto invalid;
		if (inenviron) {
			if (!*field) {
				inenviron = 0;
			} else if (valid_assignment(field)) {
				request->env[request->nenv++] = field;
			} else {
				goto invalid;
			}
		} else if (!strcmp(field, "end")) {
			ended = 1;
		} else if (!strcmp(field, "environ")) {
			/* Only the first block replaces; it must precede env= fields. */
			if (request->replaceenv || request->nenv)
				goto invalid;
			request->replaceenv = inenviron = 1;
		} else if (!strncmp(field, "cwd=", 4)) {
			request->cwd = field + 4;
		} else if (!strncmp(field, "title=", 6)) {
			request->title = field + 6;
		} else if (!strncmp(field, "env=", 4)) {
			if (!valid_assignment(field + 4))
				goto invalid;
			request->env[request->nenv++] = field + 4;
		} else if (!strncmp(field, "arg=", 4)) {
			request->argv[request->argc++] = field + 4;
		}
	}
	if (inenviron || !ended)
		goto invalid;
	return 0;

invalid:
	spare_free_request(request);
	return -1;
}

void
spare_free_request(SpareRequest *request)
{
	free(request->storage);
	free(request->env);
	free(request->argv);
	memset(request, 0, sizeof(*request));
}

static size_t
put_field(char *buffer, size_t at, const char *key, const char *value)
{
	size_t k = strlen(key), v = strlen(value);
	if (buffer) {
		memcpy(buffer + at, key, k);
		memcpy(buffer + at + k, value, v);
		buffer[at + k + v] = '\0';
	}
	return at + k + v + 1;
}

char *
spare_build_request(const char *cwd, char *const *environment,
    char *const *env, const char *title, char *const *argv, size_t *length)
{
	/* Two passes: measure, then fill. */
	char *buffer = NULL;
	for (int pass = 0; pass < 2; pass++) {
		size_t at = 6;
		if (buffer)
			memcpy(buffer, "spawn\n", at);
		if (cwd)
			at = put_field(buffer, at, "cwd=", cwd);
		if (environment) {
			at = put_field(buffer, at, "environ", "");
			for (char *const *entry = environment; *entry; entry++) {
				if (valid_assignment(*entry))
					at = put_field(buffer, at, "", *entry);
			}
			at = put_field(buffer, at, "", "");
		}
		for (char *const *entry = env; entry && *entry; entry++)
			at = put_field(buffer, at, "env=", *entry);
		if (title && *title)
			at = put_field(buffer, at, "title=", title);
		for (char *const *word = argv; word && *word; word++)
			at = put_field(buffer, at, "arg=", *word);
		at = put_field(buffer, at, "end", "");
		if (buffer) {
			*length = at;
			return buffer;
		}
		buffer = malloc(at);
		if (!buffer)
			return NULL;
	}
	return NULL;
}

void
spare_apply_environment(const SpareRequest *request)
{
	extern char **environ;
	if (request->replaceenv) {
		while (environ && environ[0]) {
			char *entry = environ[0];
			const char *equals = strchr(entry, '=');
			size_t n = equals ? (size_t)(equals - entry) : strlen(entry);
			char name[n + 1];
			memcpy(name, entry, n);
			name[n] = '\0';
			if (unsetenv(name) < 0 || environ[0] == entry)
				break; /* malformed entry that unsetenv cannot remove */
		}
	}
	for (int i = 0; i < request->nenv; i++) {
		const char *assignment = request->env[i];
		const char *equals = strchr(assignment, '=');
		size_t n = equals - assignment;
		char name[n + 1];
		memcpy(name, assignment, n);
		name[n] = '\0';
		setenv(name, equals + 1, 1);
	}
}

int
spare_exe_stamp(const char *path, char *buffer, size_t size)
{
	struct stat info;
	if (stat(path, &info) < 0)
		return -1;
	int n = snprintf(buffer, size, "%llu:%ld",
	    (unsigned long long)info.st_ino, (long)info.st_mtime);
	return n > 0 && (size_t)n < size ? 0 : -1;
}

int
spare_runtime_dir(char *buffer, size_t size)
{
	const char *home = getenv("HOME");
	if (!home || !*home)
		home = "/tmp";
	int n = snprintf(buffer, size, "%s/.runtime", home);
	if (n <= 0 || (size_t)n >= size)
		return -1;
	if (mkdir(buffer, 0700) < 0 && errno != EEXIST)
		return -1;
	n = snprintf(buffer, size, "%s/.runtime/st", home);
	if (n <= 0 || (size_t)n >= size)
		return -1;
	if (mkdir(buffer, 0700) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static pid_t
slot_pid(const char *slot)
{
	FILE *file = fopen(slot, "r");
	if (!file)
		return -1;
	long pid = -1;
	if (fscanf(file, "%ld", &pid) != 1)
		pid = -1;
	fclose(file);
	return pid > 0 ? (pid_t)pid : -1;
}

static int
alive(pid_t pid)
{
	return kill(pid, 0) == 0 || errno == EPERM;
}

int
spare_publish(const char *slot, pid_t pid, unsigned long windowid,
    const char *stamp)
{
	char temporary[PATH_MAX], line[128];
	if (snprintf(temporary, sizeof(temporary), "%s.%d.tmp", slot, (int)pid) >=
	    (int)sizeof(temporary))
		return -1;
	int n = snprintf(line, sizeof(line), "%d %lu %s\n", (int)pid, windowid,
	    stamp);
	if (n <= 0 || n >= (int)sizeof(line))
		return -1;
	int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	int written = write(fd, line, n) == n;
	close(fd);
	if (!written) {
		unlink(temporary);
		return -1;
	}

	/* link() is the atomic "create only if absent": two spares racing for
	 * the slot cannot both win, and a launcher never reads half a line. */
	int result = -1;
	for (int attempt = 0; attempt < 2; attempt++) {
		if (link(temporary, slot) == 0) {
			result = 1;
			break;
		}
		if (errno != EEXIST)
			break;
		pid_t holder = slot_pid(slot);
		if (holder == pid) {
			result = 1;
			break;
		}
		if (holder > 0 && alive(holder)) {
			result = 0;
			break;
		}
		unlink(slot); /* left behind by a spare that died unclaimed */
	}
	unlink(temporary);
	return result;
}

int
spare_holds_slot(const char *slot, pid_t pid)
{
	return slot_pid(slot) == pid;
}

void
spare_withdraw(const char *slot, pid_t pid)
{
	if (slot_pid(slot) == pid)
		unlink(slot);
}
