/* See LICENSE for license details. */

#include "launchlog.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void
append(LaunchLine *line, const char *text, size_t n)
{
	/* Keep room for the newline and terminator launchlog_append adds. */
	if (line->length + n + 2 > sizeof(line->text))
		return;
	memcpy(line->text + line->length, text, n);
	line->length += n;
	line->text[line->length] = '\0';
}

void
launchlog_begin(LaunchLine *line, double wallclock, pid_t stpid,
    const char *source)
{
	time_t seconds = (time_t)wallclock;
	int ms = (int)((wallclock - seconds) * 1e3);
	struct tm local;
	char date[32], zone[8], start[96];
	localtime_r(&seconds, &local);
	strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &local);
	strftime(zone, sizeof(zone), "%z", &local);
	int n = snprintf(start, sizeof(start), "%s.%03d%s st=%d src=%s", date,
	    ms < 0 ? 0 : ms > 999 ? 999 : ms, zone, (int)stpid, source);
	line->length = 0;
	line->text[0] = '\0';
	if (n > 0)
		append(line, start, (size_t)n < sizeof(start) ? (size_t)n
		    : sizeof(start) - 1);
}

void
launchlog_put(LaunchLine *line, const char *key, const char *value)
{
	char field[512];
	size_t n = 0, k = strlen(key);
	int quote = !*value || strpbrk(value, " \"=") != NULL;
	if (k + 4 > sizeof(field))
		return;
	field[n++] = ' ';
	memcpy(field + n, key, k);
	n += k;
	field[n++] = '=';
	if (quote)
		field[n++] = '"';
	for (const unsigned char *c = (const unsigned char *)value; *c; c++) {
		if (n + 2 >= sizeof(field))
			return;
		field[n++] = *c < 0x20 || *c == 0x7f ? ' ' : *c == '"' ? '\'' : *c;
	}
	if (quote)
		field[n++] = '"';
	append(line, field, n);
}

void
launchlog_putf(LaunchLine *line, const char *key, const char *format, ...)
{
	char value[256];
	va_list arguments;
	va_start(arguments, format);
	int n = vsnprintf(value, sizeof(value), format, arguments);
	va_end(arguments);
	if (n >= 0)
		launchlog_put(line, key, value);
}

void
launchlog_ms(LaunchLine *line, const char *key, double ms)
{
	if (ms >= 0)
		launchlog_putf(line, key, "%.1f", ms);
}

int
launchlog_should_rotate(long long size, size_t adding, long long limit)
{
	return size > 0 && size + (long long)adding > limit;
}

int
launchlog_path(char *buffer, size_t size)
{
	const char *home = getenv("HOME");
	if (!home || !*home)
		return -1;
	int n = snprintf(buffer, size, "%s/Library/Logs/st/launch.log", home);
	return n > 0 && (size_t)n < size ? 0 : -1;
}

/* mkdir -p of path's directory. */
static int
make_parent(const char *path)
{
	char directory[PATH_MAX];
	if (strlen(path) >= sizeof(directory))
		return -1;
	strcpy(directory, path);
	char *slash = strrchr(directory, '/');
	if (!slash || slash == directory)
		return 0;
	*slash = '\0';
	for (char *p = directory + 1; ; p++) {
		if (*p == '/' || !*p) {
			char saved = *p;
			*p = '\0';
			if (mkdir(directory, 0755) < 0 && errno != EEXIST)
				return -1;
			if (!(*p = saved))
				break;
		}
	}
	return 0;
}

int
launchlog_append(const char *path, const LaunchLine *line, long long limit)
{
	char text[sizeof(line->text) + 1], rotated[PATH_MAX];
	memcpy(text, line->text, line->length);
	text[line->length] = '\n';
	size_t n = line->length + 1;

	int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0 && errno == ENOENT && make_parent(path) == 0)
		fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	struct stat info;
	if (fstat(fd, &info) == 0 && launchlog_should_rotate(info.st_size, n,
	    limit) && snprintf(rotated, sizeof(rotated), "%s.1", path) <
	    (int)sizeof(rotated) && rename(path, rotated) == 0) {
		close(fd);
		fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
		if (fd < 0)
			return -1;
	}
	int ok = write(fd, text, n) == (ssize_t)n;
	close(fd);
	return ok ? 0 : -1;
}
