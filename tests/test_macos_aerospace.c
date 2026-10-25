/* See LICENSE for license details. */
/* Tests for st-aerospace-launch's AeroSpace protocol and request helpers. */

#define _DARWIN_C_SOURCE /* mkdtemp */

#include "test.h"
#include "macos/aerospace.h"
#include "macos/spare.h"

#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

static char directory[] = "/tmp/st-aerospace-test.XXXXXX";

/* The JSON payload of the single frame in buffer. */
static const char *
only_frame(const AeroBuffer *buffer, size_t *length)
{
	const char *payload;
	long used = aero_next_frame(buffer->data, buffer->length, &payload, length);
	return used == (long)buffer->length ? payload : NULL;
}

static int
payload_is(const AeroBuffer *buffer, const char *expected)
{
	size_t length;
	const char *payload = only_frame(buffer, &length);
	return payload && length == strlen(expected) &&
	    !memcmp(payload, expected, length);
}

static void
write_file(const char *path, const char *content)
{
	FILE *file = fopen(path, "w");
	fputs(content, file);
	fclose(file);
}

TEST(request_has_explicit_null_context)
{
	const char *args[] = {"focus", "--window-id", "42"};
	AeroBuffer buffer = {0};
	ASSERT_EQ(0, aero_frame_request(&buffer, args, 3, -1, NULL));
	ASSERT(payload_is(&buffer, "{\"args\":[\"focus\",\"--window-id\",\"42\"],"
	    "\"stdin\":\"\",\"windowId\":null,\"workspace\":null}"));
	aero_buffer_free(&buffer);
}

TEST(request_forwards_context)
{
	const char *args[] = {"list-apps"};
	AeroBuffer buffer = {0};
	ASSERT_EQ(0, aero_frame_request(&buffer, args, 1, 7, "2"));
	ASSERT(payload_is(&buffer, "{\"args\":[\"list-apps\"],\"stdin\":\"\","
	    "\"windowId\":7,\"workspace\":\"2\"}"));
	aero_buffer_free(&buffer);
}

TEST(request_escapes_strings)
{
	const char *args[] = {"say \"hi\"\\", "tab\tnew\nline\x01", "wörk ✓"};
	AeroBuffer buffer = {0};
	ASSERT_EQ(0, aero_frame_request(&buffer, args, 3, -1, "a\"b"));
	ASSERT(payload_is(&buffer, "{\"args\":[\"say \\\"hi\\\"\\\\\","
	    "\"tab\\u0009new\\u000aline\\u0001\",\"wörk ✓\"],\"stdin\":\"\","
	    "\"windowId\":null,\"workspace\":\"a\\\"b\"}"));
	/* And it reads back as the same strings. */
	size_t length;
	const char *payload = only_frame(&buffer, &length);
	char *workspace;
	ASSERT_EQ(1, aero_json_string(payload, length, "workspace", &workspace));
	ASSERT_STR_EQ("a\"b", workspace);
	free(workspace);
	aero_buffer_free(&buffer);
}

TEST(requests_are_framed_back_to_back)
{
	const char *subscribe[] = {"subscribe", "focus-changed", "window-detected"};
	const char *apps[] = {"list-apps"};
	AeroBuffer buffer = {0};
	ASSERT_EQ(0, aero_frame_request(&buffer, subscribe, 3, -1, NULL));
	size_t first = buffer.length;
	ASSERT_EQ(0, aero_frame_request(&buffer, apps, 1, -1, NULL));
	uint32_t size;
	memcpy(&size, buffer.data, sizeof(size));
	ASSERT_EQ(first - 4, size);
	const char *payload;
	size_t length;
	ASSERT_EQ((long)first, aero_next_frame(buffer.data, buffer.length,
	    &payload, &length));
	ASSERT_EQ((long)(buffer.length - first), aero_next_frame(
	    buffer.data + first, buffer.length - first, &payload, &length));
	aero_buffer_free(&buffer);
}

TEST(incomplete_frames_wait_for_more)
{
	const char *args[] = {"list-apps"};
	AeroBuffer buffer = {0};
	ASSERT_EQ(0, aero_frame_request(&buffer, args, 1, -1, NULL));
	const char *payload;
	size_t length;
	for (size_t n = 0; n < buffer.length; n++)
		ASSERT_EQ(0, aero_next_frame(buffer.data, n, &payload, &length));
	ASSERT_EQ((long)buffer.length, aero_next_frame(buffer.data,
	    buffer.length, &payload, &length));
	aero_buffer_free(&buffer);
}

TEST(oversized_frames_are_rejected)
{
	char data[8] = {0};
	uint32_t size = AEROSPACE_MAX_FRAME + 1;
	memcpy(data, &size, sizeof(size));
	const char *payload;
	size_t length;
	ASSERT_EQ(-1, aero_next_frame(data, sizeof(data), &payload, &length));
}

TEST(parses_window_detected_event)
{
	static const char json[] = "{\"_event\":\"window-detected\","
	    "\"appBundleId\":\"io.yeyito.st\",\"appName\":\"st\","
	    "\"windowId\":4242,\"workspace\":\"1\"}";
	AeroEvent event;
	ASSERT_EQ(0, aero_parse_event(json, sizeof(json) - 1, &event));
	ASSERT_STR_EQ("window-detected", event.event);
	ASSERT_EQ(1, event.haswindow);
	ASSERT(event.windowid == 4242);
	ASSERT_STR_EQ("1", event.workspace);
	aero_free_event(&event);
}

TEST(focus_on_empty_workspace_has_no_window)
{
	/* The server leaves out nil members; null is accepted too. */
	static const char absent[] =
	    "{\"_event\":\"focus-changed\",\"workspace\":\"web\"}";
	static const char null[] = " { \"_event\" : \"focus-changed\" , "
	    "\"windowId\" : null , \"workspace\" : \"web\" } \n";
	AeroEvent event;
	ASSERT_EQ(0, aero_parse_event(absent, sizeof(absent) - 1, &event));
	ASSERT_EQ(0, event.haswindow);
	ASSERT_STR_EQ("web", event.workspace);
	aero_free_event(&event);
	ASSERT_EQ(0, aero_parse_event(null, sizeof(null) - 1, &event));
	ASSERT_EQ(0, event.haswindow);
	ASSERT_STR_EQ("web", event.workspace);
	aero_free_event(&event);
}

TEST(window_detected_without_workspace)
{
	static const char json[] = "{\"_event\":\"window-detected\",\"windowId\":3}";
	AeroEvent event;
	ASSERT_EQ(0, aero_parse_event(json, sizeof(json) - 1, &event));
	ASSERT(event.workspace == NULL);
	aero_free_event(&event);
}

TEST(skips_nested_and_unknown_members)
{
	static const char json[] = "{\"extra\":{\"a\":[1,2.5e3,{\"b\":\"}\"}],"
	    "\"c\":true},\"_event\":\"binding-triggered\",\"list\":[],"
	    "\"f\":false,\"n\":-1.5,\"windowId\":9}";
	AeroEvent event;
	ASSERT_EQ(0, aero_parse_event(json, sizeof(json) - 1, &event));
	ASSERT_STR_EQ("binding-triggered", event.event);
	ASSERT(event.windowid == 9);
	aero_free_event(&event);
}

TEST(decodes_string_escapes)
{
	static const char json[] = "{\"workspace\":\"q\\\" b\\\\ s\\/ "
	    "\\b\\f\\n\\r\\t \\u00e9 \\u2713 \\ud83d\\ude00 \\udc00\"}";
	char *workspace;
	ASSERT_EQ(1, aero_json_string(json, sizeof(json) - 1, "workspace",
	    &workspace));
	ASSERT_STR_EQ("q\" b\\ s/ \b\f\n\r\t é ✓ 😀 \xef\xbf\xbd", workspace);
	free(workspace);
}

TEST(member_types_are_checked)
{
	static const char json[] = "{\"windowId\":\"7\",\"workspace\":7,"
	    "\"exitCode\":1.5}";
	long long number;
	char *text;
	ASSERT_EQ(-1, aero_json_integer(json, sizeof(json) - 1, "windowId", &number));
	ASSERT_EQ(-1, aero_json_string(json, sizeof(json) - 1, "workspace", &text));
	ASSERT_EQ(-1, aero_json_integer(json, sizeof(json) - 1, "exitCode", &number));
	ASSERT_EQ(0, aero_json_string(json, sizeof(json) - 1, "missing", &text));
	ASSERT(text == NULL);
}

TEST(rejects_malformed_json)
{
	static const char *const bad[] = {
		"", "[]", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{\"a\":1} x",
		"{\"a\":\"unterminated}", "{\"a\":\"bad \\x escape\"}",
		"{\"a\":\"raw \n newline\"}", "{\"a\":\"\\u12\"}", "{\"a\":nul}",
		"{\"a\":01x}", "{\"a\":-}", "{\"a\":\"\\u0000\"}",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		char *text;
		if (aero_json_string(bad[i], strlen(bad[i]), "a", &text) != -1) {
			printf("accepted <%s> ", bad[i]);
			ASSERT(0);
		}
	}
	AeroEvent event;
	static const char noevent[] = "{\"windowId\":1}";
	ASSERT_EQ(-1, aero_parse_event(noevent, sizeof(noevent) - 1, &event));
	static const char negative[] = "{\"_event\":\"x\",\"windowId\":-1}";
	ASSERT_EQ(-1, aero_parse_event(negative, sizeof(negative) - 1, &event));
}

TEST(parses_server_answer)
{
	static const char json[] = "{\"exitCode\":2,\"serverVersionAndHash\":"
	    "\"0.20 abc\",\"stderr\":\"no such window\",\"stdout\":\"a\\nb\"}";
	AeroAnswer answer;
	ASSERT_EQ(0, aero_parse_answer(json, sizeof(json) - 1, &answer));
	ASSERT_EQ(2, answer.exitcode);
	ASSERT_STR_EQ("a\nb", answer.out);
	ASSERT_STR_EQ("no such window", answer.err);
	aero_free_answer(&answer);
	/* An event is not an answer. */
	static const char event[] = "{\"_event\":\"focus-changed\",\"workspace\":\"1\"}";
	ASSERT_EQ(-1, aero_parse_answer(event, sizeof(event) - 1, &answer));
}

TEST(parses_slot_lines)
{
	pid_t pid;
	unsigned long long window;
	char stamp[64];
	ASSERT_EQ(0, launch_parse_slot("123 456 789:1700000000\n", &pid, &window,
	    stamp, sizeof(stamp)));
	ASSERT_EQ(123, pid);
	ASSERT(window == 456);
	ASSERT_STR_EQ("789:1700000000", stamp);
	ASSERT_EQ(-1, launch_parse_slot("", &pid, &window, stamp, sizeof(stamp)));
	ASSERT_EQ(-1, launch_parse_slot("123 456\n", &pid, &window, stamp,
	    sizeof(stamp)));
	ASSERT_EQ(-1, launch_parse_slot("abc 456 1:2\n", &pid, &window, stamp,
	    sizeof(stamp)));
	/* Never a PID that kill() would treat as a process group. */
	ASSERT_EQ(-1, launch_parse_slot("0 456 1:2\n", &pid, &window, stamp,
	    sizeof(stamp)));
	ASSERT_EQ(-1, launch_parse_slot("1 456 1:2\n", &pid, &window, stamp,
	    sizeof(stamp)));
	ASSERT_EQ(-1, launch_parse_slot("-5 456 1:2\n", &pid, &window, stamp,
	    sizeof(stamp)));
	ASSERT_EQ(-1, launch_parse_slot("123 456 1:2\n", &pid, &window, stamp, 3));
}

TEST(focused_terminal_pid)
{
	ASSERT_EQ(321, launch_focused_pid("io.yeyito.st 321", "io.yeyito.st"));
	ASSERT_EQ(321, launch_focused_pid("io.yeyito.st 321\n", "io.yeyito.st"));
	ASSERT_EQ(-1, launch_focused_pid("", "io.yeyito.st"));
	ASSERT_EQ(-1, launch_focused_pid("com.apple.finder 321", "io.yeyito.st"));
	ASSERT_EQ(-1, launch_focused_pid("io.yeyito.stx 321", "io.yeyito.st"));
	ASSERT_EQ(-1, launch_focused_pid("io.yeyito.st 32a", "io.yeyito.st"));
	ASSERT_EQ(-1, launch_focused_pid("io.yeyito.st ", "io.yeyito.st"));
	ASSERT_EQ(-1, launch_focused_pid("io.yeyito.st 1\nio.yeyito.st 2",
	    "io.yeyito.st"));
}

TEST(window_workspace_from_query)
{
	char *workspace;
	ASSERT_EQ(1, launch_window_workspace("77 my space\n88 other", 77,
	    &workspace));
	ASSERT_STR_EQ("my space", workspace);
	free(workspace);
	ASSERT_EQ(1, launch_window_workspace("77 ", 77, &workspace));
	ASSERT_STR_EQ("", workspace);
	free(workspace);
	ASSERT_EQ(0, launch_window_workspace("770 1", 77, &workspace));
	ASSERT_EQ(0, launch_window_workspace("", 77, &workspace));
	ASSERT_EQ(0, launch_window_workspace("88 1\n77 1", 77, &workspace));
	ASSERT(workspace == NULL);
}

TEST(reads_recorded_venv)
{
	char venv[512], context[512], link[512], activate[600];
	snprintf(venv, sizeof(venv), "%s/venv with spaces", directory);
	snprintf(context, sizeof(context), "%s/context", directory);
	snprintf(link, sizeof(link), "%s/link", directory);
	snprintf(activate, sizeof(activate), "%s/bin", venv);
	mkdir(venv, 0700);
	mkdir(activate, 0700);
	strcat(activate, "/activate");
	write_file(activate, "");

	char line[600], *found;
	snprintf(line, sizeof(line), "%s\n", venv);
	write_file(context, line);
	ASSERT_EQ(1, launch_read_venv(context, &found));
	ASSERT_STR_EQ(venv, found);
	free(found);
	/* Symlinked contexts, deactivation, cut-short lines and venvs that
	 * no longer exist all mean no venv. */
	symlink(context, link);
	ASSERT_EQ(0, launch_read_venv(link, &found));
	write_file(context, "\n");
	ASSERT_EQ(0, launch_read_venv(context, &found));
	write_file(context, venv);
	ASSERT_EQ(0, launch_read_venv(context, &found));
	snprintf(line, sizeof(line), "%s/missing\n", venv);
	write_file(context, line);
	ASSERT_EQ(0, launch_read_venv(context, &found));
	ASSERT_EQ(0, launch_read_venv("/nonexistent/context", &found));
	ASSERT(found == NULL);
	unlink(link);
	unlink(context);
	unlink(activate);
	activate[strlen(activate) - strlen("/activate")] = '\0';
	rmdir(activate);
	rmdir(venv);
}

TEST(spawn_request_round_trips)
{
	char *environment[] = {"PATH=/usr/bin:/bin", "EMPTY=", "noequals",
	    "=nameless", "LANG=C", NULL};
	char *env[] = {"ST_INHERIT_VIRTUAL_ENV=", NULL};
	char *argv[] = {"/bin/echo", "two words", "", NULL};
	size_t length;
	char *message = spare_build_request("/tmp/a dir", environment, env,
	    "a title", argv, &length);
	ASSERT_NOT_NULL(message);
	ASSERT(length > 6 && !memcmp(message, "spawn\n", 6));
	SpareRequest request;
	ASSERT_EQ(0, spare_parse_request(message + 6, length - 6, &request));
	ASSERT_STR_EQ("/tmp/a dir", request.cwd);
	ASSERT_STR_EQ("a title", request.title);
	ASSERT_EQ(1, request.replaceenv);
	/* Entries st would reject are left out instead of losing the launch. */
	ASSERT_EQ(4, request.nenv);
	ASSERT_STR_EQ("PATH=/usr/bin:/bin", request.env[0]);
	ASSERT_STR_EQ("EMPTY=", request.env[1]);
	ASSERT_STR_EQ("LANG=C", request.env[2]);
	ASSERT_STR_EQ("ST_INHERIT_VIRTUAL_ENV=", request.env[3]);
	ASSERT_EQ(3, request.argc);
	ASSERT_STR_EQ("two words", request.argv[1]);
	ASSERT_STR_EQ("", request.argv[2]);
	spare_free_request(&request);
	free(message);
}

TEST(plain_spawn_request_starts_the_shell)
{
	char *environment[] = {NULL};
	size_t length;
	/* No title (empty means none, like the shell launcher) and no args. */
	char *message = spare_build_request("/home", environment, NULL, "", NULL,
	    &length);
	static const char expected[] = "spawn\ncwd=/home\0environ\0\0end";
	ASSERT_EQ(sizeof(expected), length);
	ASSERT(!memcmp(expected, message, length));
	SpareRequest request;
	ASSERT_EQ(0, spare_parse_request(message + 6, length - 6, &request));
	ASSERT(request.title == NULL);
	ASSERT_EQ(0, request.argc);
	spare_free_request(&request);
	free(message);
}

TEST(parses_unsigned_numbers)
{
	ASSERT_EQ(0, (int)launch_parse_uint("0", 10));
	ASSERT_EQ(4294967295LL, launch_parse_uint("4294967295", UINT32_MAX));
	ASSERT_EQ(-1, (int)launch_parse_uint("4294967296", UINT32_MAX));
	ASSERT_EQ(-1, (int)launch_parse_uint("99999999999999999999999", UINT32_MAX));
	ASSERT_EQ(-1, (int)launch_parse_uint("", 10));
	ASSERT_EQ(-1, (int)launch_parse_uint(NULL, 10));
	ASSERT_EQ(-1, (int)launch_parse_uint("+1", 10));
	ASSERT_EQ(-1, (int)launch_parse_uint("1 ", 10));
}

static size_t
procargs(char *buffer, int argc, const char *const *argv)
{
	size_t length = sizeof(argc);
	memcpy(buffer, &argc, sizeof(argc));
	memcpy(buffer + length, "/bin/sh\0\0\0", 10); /* path + padding */
	length += 10;
	for (int i = 0; i < argc; i++) {
		size_t n = strlen(argv[i]) + 1;
		memcpy(buffer + length, argv[i], n);
		length += n;
	}
	memcpy(buffer + length, "HOME=/x\0", 8); /* the environment follows */
	return length + 8;
}

TEST(recognizes_spare_processes_by_arguments)
{
	char buffer[256];
	static const char *const spare[] = {"/bin/sh", "/tmp/st", "--spare"};
	static const char *const privateslot[] = {"st", "--spare-slot", "/tmp/slot"};
	static const char *const other[] = {"vim", "notes.txt"};
	static const char *const envlike[] = {"sh"};
	ASSERT_EQ(1, launch_args_name_spare(buffer, procargs(buffer, 3, spare)));
	ASSERT_EQ(1, launch_args_name_spare(buffer, procargs(buffer, 3, privateslot)));
	ASSERT_EQ(0, launch_args_name_spare(buffer, procargs(buffer, 2, other)));
	/* "--spare" in the environment is not an argument. */
	size_t n = procargs(buffer, 1, envlike);
	memcpy(buffer + n - 8, "--spare\0", 8);
	ASSERT_EQ(0, launch_args_name_spare(buffer, n));
	/* Truncated buffers are not spares. */
	ASSERT_EQ(0, launch_args_name_spare(buffer, 3));
	ASSERT_EQ(0, launch_args_name_spare(buffer, procargs(buffer, 3, spare) - 30));
}

TEST_SUITE(macos_aerospace)
{
	RUN_TEST(request_has_explicit_null_context);
	RUN_TEST(request_forwards_context);
	RUN_TEST(request_escapes_strings);
	RUN_TEST(requests_are_framed_back_to_back);
	RUN_TEST(incomplete_frames_wait_for_more);
	RUN_TEST(oversized_frames_are_rejected);
	RUN_TEST(parses_window_detected_event);
	RUN_TEST(focus_on_empty_workspace_has_no_window);
	RUN_TEST(window_detected_without_workspace);
	RUN_TEST(skips_nested_and_unknown_members);
	RUN_TEST(decodes_string_escapes);
	RUN_TEST(member_types_are_checked);
	RUN_TEST(rejects_malformed_json);
	RUN_TEST(parses_server_answer);
	RUN_TEST(parses_slot_lines);
	RUN_TEST(focused_terminal_pid);
	RUN_TEST(window_workspace_from_query);
	RUN_TEST(reads_recorded_venv);
	RUN_TEST(spawn_request_round_trips);
	RUN_TEST(plain_spawn_request_starts_the_shell);
	RUN_TEST(parses_unsigned_numbers);
	RUN_TEST(recognizes_spare_processes_by_arguments);
}

int
main(void)
{
	if (!mkdtemp(directory)) return 1;
	RUN_SUITE(macos_aerospace);
	rmdir(directory);
	return test_summary();
}
