/* See LICENSE for license details. */
/* Tests for the warm spare's request parsing and its single-slot protocol. */

#define _DARWIN_C_SOURCE /* mkdtemp */

#include "test.h"
#include "macos/spare.h"

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char directory[] = "/tmp/st-spare-test.XXXXXX";
static char slot[256];

static void
read_slot(char *buffer, size_t size)
{
	FILE *file = fopen(slot, "r");
	buffer[0] = '\0';
	if (!file) return;
	if (!fgets(buffer, size, file)) buffer[0] = '\0';
	fclose(file);
}

static pid_t
dead_pid(void)
{
	pid_t child = fork();
	if (child == 0) _exit(0);
	waitpid(child, NULL, 0);
	return child;
}

TEST(parses_fields_in_order)
{
	static const char payload[] =
	    "cwd=/tmp/a dir\0title=my title\0env=PATH=/bin:/usr/bin\0"
	    "env=ST_INHERIT_VIRTUAL_ENV=\0arg=/bin/sh\0arg=-c\0arg=echo a=b\0";
	SpareRequest request;
	ASSERT_EQ(0, spare_parse_request(payload, sizeof(payload) - 1, &request));
	ASSERT_STR_EQ("/tmp/a dir", request.cwd);
	ASSERT_STR_EQ("my title", request.title);
	ASSERT_EQ(2, request.nenv);
	ASSERT_STR_EQ("PATH=/bin:/usr/bin", request.env[0]);
	ASSERT_STR_EQ("ST_INHERIT_VIRTUAL_ENV=", request.env[1]);
	ASSERT_EQ(3, request.argc);
	ASSERT_STR_EQ("/bin/sh", request.argv[0]);
	ASSERT_STR_EQ("-c", request.argv[1]);
	ASSERT_STR_EQ("echo a=b", request.argv[2]);
	ASSERT(request.argv[3] == NULL);
	spare_free_request(&request);
}

TEST(empty_request_starts_the_shell)
{
	SpareRequest request;
	ASSERT_EQ(0, spare_parse_request("", 0, &request));
	ASSERT(request.cwd == NULL);
	ASSERT(request.title == NULL);
	ASSERT_EQ(0, request.nenv);
	ASSERT_EQ(0, request.argc);
	ASSERT(request.argv[0] == NULL);
	spare_free_request(&request);
}

TEST(ignores_unknown_fields)
{
	static const char payload[] = "future=1\0cwd=/\0";
	SpareRequest request;
	ASSERT_EQ(0, spare_parse_request(payload, sizeof(payload) - 1, &request));
	ASSERT_STR_EQ("/", request.cwd);
	spare_free_request(&request);
}

TEST(rejects_malformed_requests)
{
	SpareRequest request;
	/* Truncated: the last field is not terminated. */
	ASSERT_EQ(-1, spare_parse_request("cwd=/tmp", 8, &request));
	/* env= needs NAME=VALUE with a non-empty name. */
	ASSERT_EQ(-1, spare_parse_request("env=PATH\0", 9, &request));
	ASSERT_EQ(-1, spare_parse_request("env==x\0", 7, &request));
}

TEST(rejects_too_many_arguments)
{
	char payload[(SPARE_MAX_ARGS + 1) * 6];
	size_t length = 0;
	for (int i = 0; i <= SPARE_MAX_ARGS; i++) {
		memcpy(payload + length, "arg=x", 6); /* includes the NUL */
		length += 6;
	}
	SpareRequest request;
	ASSERT_EQ(-1, spare_parse_request(payload, length, &request));
	ASSERT_EQ(0, spare_parse_request(payload, length - 6, &request));
	ASSERT_EQ(SPARE_MAX_ARGS, request.argc);
	spare_free_request(&request);
}

TEST(publishes_pid_window_and_stamp)
{
	char line[128];
	unlink(slot);
	ASSERT_EQ(1, spare_publish(slot, getpid(), 4242, "7:99"));
	read_slot(line, sizeof(line));
	char expected[64];
	snprintf(expected, sizeof(expected), "%d 4242 7:99\n", (int)getpid());
	ASSERT_STR_EQ(expected, line);
	ASSERT(spare_holds_slot(slot, getpid()));
	/* No temporary file is left next to the slot. */
	char temporary[300];
	snprintf(temporary, sizeof(temporary), "%s.%d.tmp", slot, (int)getpid());
	ASSERT(access(temporary, F_OK) != 0);
}

TEST(second_live_spare_is_redundant)
{
	unlink(slot);
	ASSERT_EQ(1, spare_publish(slot, getppid(), 1, "s"));
	ASSERT_EQ(0, spare_publish(slot, getpid(), 2, "s"));
	ASSERT(spare_holds_slot(slot, getppid()));
	ASSERT(!spare_holds_slot(slot, getpid()));
}

TEST(replaces_slot_of_dead_spare)
{
	unlink(slot);
	pid_t dead = dead_pid();
	ASSERT_EQ(1, spare_publish(slot, dead, 1, "s"));
	ASSERT_EQ(1, spare_publish(slot, getpid(), 2, "s"));
	ASSERT(spare_holds_slot(slot, getpid()));
}

TEST(withdraw_only_removes_own_slot)
{
	unlink(slot);
	ASSERT_EQ(1, spare_publish(slot, getppid(), 1, "s"));
	spare_withdraw(slot, getpid());
	ASSERT(spare_holds_slot(slot, getppid()));
	spare_withdraw(slot, getppid());
	ASSERT(access(slot, F_OK) != 0);
	/* A claimed (renamed) slot is simply absent. */
	spare_withdraw(slot, getpid());
	ASSERT(!spare_holds_slot(slot, getpid()));
}

TEST(stamp_is_inode_and_mtime)
{
	char file[300], stamp[64], expected[64];
	struct stat info;
	snprintf(file, sizeof(file), "%s/binary", directory);
	FILE *created = fopen(file, "w");
	ASSERT(created != NULL);
	fclose(created);
	ASSERT_EQ(0, stat(file, &info));
	snprintf(expected, sizeof(expected), "%llu:%ld",
	    (unsigned long long)info.st_ino, (long)info.st_mtime);
	ASSERT_EQ(0, spare_exe_stamp(file, stamp, sizeof(stamp)));
	ASSERT_STR_EQ(expected, stamp);
	ASSERT_EQ(-1, spare_exe_stamp("/nonexistent/st", stamp, sizeof(stamp)));
	unlink(file);
}

TEST(runtime_dir_is_created_under_home)
{
	char home[300], runtime[300], expected[300];
	snprintf(home, sizeof(home), "%s/home", directory);
	ASSERT_EQ(0, mkdir(home, 0700));
	ASSERT_EQ(0, setenv("HOME", home, 1));
	ASSERT_EQ(0, spare_runtime_dir(runtime, sizeof(runtime)));
	snprintf(expected, sizeof(expected), "%s/.runtime/st", home);
	ASSERT_STR_EQ(expected, runtime);
	struct stat info;
	ASSERT_EQ(0, stat(runtime, &info));
	ASSERT(S_ISDIR(info.st_mode));
	/* Existing directories are fine. */
	ASSERT_EQ(0, spare_runtime_dir(runtime, sizeof(runtime)));
	rmdir(runtime);
	snprintf(expected, sizeof(expected), "%s/.runtime", home);
	rmdir(expected);
	rmdir(home);
}

TEST_SUITE(macos_spare)
{
	RUN_TEST(parses_fields_in_order);
	RUN_TEST(empty_request_starts_the_shell);
	RUN_TEST(ignores_unknown_fields);
	RUN_TEST(rejects_malformed_requests);
	RUN_TEST(rejects_too_many_arguments);
	RUN_TEST(publishes_pid_window_and_stamp);
	RUN_TEST(second_live_spare_is_redundant);
	RUN_TEST(replaces_slot_of_dead_spare);
	RUN_TEST(withdraw_only_removes_own_slot);
	RUN_TEST(stamp_is_inode_and_mtime);
	RUN_TEST(runtime_dir_is_created_under_home);
}

int
main(void)
{
	if (!mkdtemp(directory)) return 1;
	snprintf(slot, sizeof(slot), "%s/spare", directory);
	RUN_SUITE(macos_spare);
	unlink(slot);
	rmdir(directory);
	return test_summary();
}
