/* Pure protocol tests: no X server is required. */
#include "test.h"
#include "../clipboard5522.h"

typedef struct {
	char data[20000];
	size_t length;
} Output;

static void
capture(const char *data, size_t length, void *context)
{
	Output *output = context;
	ASSERT(output->length + length < sizeof(output->data));
	memcpy(output->data + output->length, data, length);
	output->length += length;
	output->data[output->length] = '\0';
}

TEST(parses_paste_event_read)
{
	Clip5522Request request;
	ASSERT(clip5522_parse_read(
		"type=read:pw=c2VjcmV0:name=UGFzdGUgZXZlbnQ=",
		"aW1hZ2UvcG5nIHRleHQvcGxhaW4=", &request));
	ASSERT(!request.primary);
	ASSERT(request.paste_event_name);
	ASSERT_STR_EQ("c2VjcmV0", request.password);
	ASSERT_EQ(2, request.nmimes);
	ASSERT_STR_EQ("image/png", request.mimes[0]);
	ASSERT_STR_EQ("text/plain", request.mimes[1]);
	clip5522_request_free(&request);
}

TEST(rejects_malformed_or_non_mime_read)
{
	Clip5522Request request;
	ASSERT(!clip5522_parse_read("type=read", "not base64!", &request));
	ASSERT(!clip5522_parse_read("type=read", "VVRGOF9TVFJJTkc=", &request));
	ASSERT(!clip5522_mime_is_valid("image/"));
	ASSERT(!clip5522_mime_is_valid("image png"));
	ASSERT(clip5522_mime_is_valid("text/plain;charset=utf-8"));
}

TEST(emits_current_paste_event_wire_form)
{
	Output output = {0};
	const unsigned char list[] = "image/png text/plain";
	clip5522_status(capture, &output, "OK", 1, "cHc=");
	clip5522_data(capture, &output, ".", list, sizeof(list) - 1, "cHc=");
	clip5522_status(capture, &output, "DONE", 0, "cHc=");
	ASSERT_STR_EQ("\033]5522;type=read:status=OK:loc=primary:pw=cHc=\033\\"
		"\033]5522;type=read:status=DATA:mime=Lg==:pw=cHc=;"
		"aW1hZ2UvcG5nIHRleHQvcGxhaW4=\033\\"
		"\033]5522;type=read:status=DONE:pw=cHc=\033\\", output.data);
}

TEST(chunks_raw_binary_at_4096_bytes)
{
	Output output = {0};
	unsigned char data[CLIP5522_RAW_CHUNK + 1] = {0};
	clip5522_data(capture, &output, "image/png", data, sizeof(data), NULL);
	/* Two DATA packets, and padding belongs to each independently encoded chunk. */
	ASSERT_EQ(2, (int)((strstr(output.data, "\033]5522;") != NULL) +
		(strstr(strstr(output.data, "\033]5522;") + 1, "\033]5522;") != NULL)));
	ASSERT(strstr(output.data, "AAAAAA==\033\\"));
}

TEST_SUITE(clipboard5522)
{
	RUN_TEST(parses_paste_event_read);
	RUN_TEST(rejects_malformed_or_non_mime_read);
	RUN_TEST(emits_current_paste_event_wire_form);
	RUN_TEST(chunks_raw_binary_at_4096_bytes);
}

int
main(void)
{
	RUN_SUITE(clipboard5522);
	return test_summary();
}
