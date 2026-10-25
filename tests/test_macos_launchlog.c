/* See LICENSE for license details. */
/* Tests for the launch log's line format, rotation and append. */

#define _DARWIN_C_SOURCE /* mkdtemp */

#include "test.h"
#include "macos/launchlog.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char directory[] = "/tmp/st-launchlog-test.XXXXXX";

static long
file_size(const char *path)
{
	struct stat info;
	return stat(path, &info) == 0 ? (long)info.st_size : -1;
}

static void
read_file(const char *path, char *buffer, size_t size)
{
	FILE *file = fopen(path, "r");
	size_t n = file ? fread(buffer, 1, size - 1, file) : 0;
	buffer[n] = '\0';
	if (file) fclose(file);
}

TEST(begins_with_local_time_pid_and_source)
{
	LaunchLine line;
	/* 2026-10-25 14:03:12.345 UTC */
	launchlog_begin(&line, 1792936992.345, 4321, "launcher");
	ASSERT_STR_EQ("2026-10-25T14:03:12.345+0000 st=4321 src=launcher",
	    line.text);
	ASSERT_EQ(strlen(line.text), line.length);
}

TEST(formats_fields)
{
	LaunchLine line;
	launchlog_begin(&line, 0, 7, "st");
	launchlog_put(&line, "path", "warm");
	launchlog_ms(&line, "claimed", 1.25);
	launchlog_ms(&line, "detected", -1); /* not reached: left out */
	launchlog_putf(&line, "frames", "%u", 42u);
	ASSERT_STR_EQ("1970-01-01T00:00:00.000+0000 st=7 src=st path=warm "
	    "claimed=1.2 frames=42", line.text);
}

TEST(quotes_values_with_spaces_quotes_or_equals)
{
	LaunchLine line;
	launchlog_begin(&line, 0, 0, "launcher");
	line.length = 0;
	line.text[0] = '\0';
	launchlog_put(&line, "error", "Can't \"connect\"\nnow");
	launchlog_put(&line, "cmd", "a=b");
	launchlog_put(&line, "title", "");
	ASSERT_STR_EQ(" error=\"Can't 'connect' now\" cmd=\"a=b\" title=\"\"",
	    line.text);
}

TEST(leaves_out_fields_that_do_not_fit)
{
	LaunchLine line;
	char value[400];
	memset(value, 'x', sizeof(value) - 1);
	value[sizeof(value) - 1] = '\0';
	launchlog_begin(&line, 0, 0, "st");
	for (int i = 0; i < 10; i++)
		launchlog_put(&line, "long", value);
	launchlog_put(&line, "end", "1");
	ASSERT(line.length + 2 <= sizeof(line.text));
	ASSERT_EQ(strlen(line.text), line.length);
	/* Whole fields only: two long ones fit, then the short one still does. */
	int fields = 0;
	for (const char *at = line.text; (at = strstr(at, " long=")); at++)
		fields++;
	ASSERT_EQ(2, fields);
	ASSERT_STR_EQ(" end=1", line.text + line.length - 6);
}

TEST(rotation_decision)
{
	ASSERT(!launchlog_should_rotate(0, 2000, 1000));   /* never an empty log */
	ASSERT(!launchlog_should_rotate(900, 100, 1000));
	ASSERT(launchlog_should_rotate(901, 100, 1000));
	ASSERT(launchlog_should_rotate(5000, 1, 1000));
}

TEST(path_is_under_library_logs)
{
	char path[512], expected[512];
	ASSERT_EQ(0, launchlog_path(path, sizeof(path)));
	snprintf(expected, sizeof(expected), "%s/Library/Logs/st/launch.log",
	    getenv("HOME"));
	ASSERT_STR_EQ(expected, path);
	ASSERT_EQ(-1, launchlog_path(path, 10));
}

TEST(appends_lines_creating_directories)
{
	char path[512], text[256];
	snprintf(path, sizeof(path), "%s/Library/Logs/st/launch.log", directory);
	LaunchLine line;
	launchlog_begin(&line, 0, 1, "launcher");
	ASSERT_EQ(0, launchlog_append(path, &line, LAUNCHLOG_LIMIT));
	launchlog_begin(&line, 0, 2, "st");
	ASSERT_EQ(0, launchlog_append(path, &line, LAUNCHLOG_LIMIT));
	read_file(path, text, sizeof(text));
	ASSERT_STR_EQ("1970-01-01T00:00:00.000+0000 st=1 src=launcher\n"
	    "1970-01-01T00:00:00.000+0000 st=2 src=st\n", text);
}

TEST(rotates_past_the_limit)
{
	char path[512], rotated[512], text[256];
	snprintf(path, sizeof(path), "%s/rotate.log", directory);
	snprintf(rotated, sizeof(rotated), "%s.1", path);
	LaunchLine line;
	launchlog_begin(&line, 0, 1, "st"); /* 41 bytes with the newline */
	ASSERT_EQ(0, launchlog_append(path, &line, 100));
	ASSERT_EQ(0, launchlog_append(path, &line, 100));
	ASSERT_EQ(82, file_size(path));
	ASSERT_EQ(-1, file_size(rotated));
	launchlog_begin(&line, 0, 3, "st");
	ASSERT_EQ(0, launchlog_append(path, &line, 100));
	ASSERT_EQ(82, file_size(rotated));
	read_file(path, text, sizeof(text));
	ASSERT_STR_EQ("1970-01-01T00:00:00.000+0000 st=3 src=st\n", text);
	unlink(rotated);
	unlink(path);
}

TEST_SUITE(macos_launchlog)
{
	RUN_TEST(begins_with_local_time_pid_and_source);
	RUN_TEST(formats_fields);
	RUN_TEST(quotes_values_with_spaces_quotes_or_equals);
	RUN_TEST(leaves_out_fields_that_do_not_fit);
	RUN_TEST(rotation_decision);
	RUN_TEST(path_is_under_library_logs);
	RUN_TEST(appends_lines_creating_directories);
	RUN_TEST(rotates_past_the_limit);
}

int
main(void)
{
	setenv("TZ", "UTC", 1);
	tzset();
	if (!mkdtemp(directory)) return 1;
	setenv("HOME", directory, 1);
	RUN_SUITE(macos_launchlog);
	char path[512];
	snprintf(path, sizeof(path), "rm -rf '%s'", directory);
	system(path);
	return test_summary();
}
