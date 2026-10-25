/* See LICENSE for license details. */

#include "aerospace.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int
aero_buffer_append(AeroBuffer *buffer, const void *data, size_t length)
{
	if (buffer->length + length > buffer->capacity) {
		size_t capacity = buffer->capacity ? buffer->capacity : 256;
		while (capacity < buffer->length + length)
			capacity *= 2;
		char *grown = realloc(buffer->data, capacity);
		if (!grown)
			return -1;
		buffer->data = grown;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->length, data, length);
	buffer->length += length;
	return 0;
}

void
aero_buffer_free(AeroBuffer *buffer)
{
	free(buffer->data);
	memset(buffer, 0, sizeof(*buffer));
}

static int
append_string(AeroBuffer *buffer, const char *text)
{
	return aero_buffer_append(buffer, text, strlen(text));
}

static int
append_json_string(AeroBuffer *buffer, const char *text)
{
	if (aero_buffer_append(buffer, "\"", 1) < 0)
		return -1;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		char escape[8];
		if (*p == '"' || *p == '\\') {
			escape[0] = '\\';
			escape[1] = *p;
			escape[2] = '\0';
		} else if (*p < 0x20) {
			snprintf(escape, sizeof(escape), "\\u%04x", *p);
		} else {
			if (aero_buffer_append(buffer, p, 1) < 0)
				return -1;
			continue;
		}
		if (append_string(buffer, escape) < 0)
			return -1;
	}
	return aero_buffer_append(buffer, "\"", 1);
}

int
aero_frame_request(AeroBuffer *buffer, const char *const *args, int nargs,
    long long windowid, const char *workspace)
{
	size_t start = buffer->length;
	uint32_t length = 0;
	if (aero_buffer_append(buffer, &length, sizeof(length)) < 0 ||
	    append_string(buffer, "{\"args\":[") < 0)
		goto fail;
	for (int i = 0; i < nargs; i++) {
		if ((i && append_string(buffer, ",") < 0) ||
		    append_json_string(buffer, args[i]) < 0)
			goto fail;
	}
	if (append_string(buffer, "],\"stdin\":\"\",\"windowId\":") < 0)
		goto fail;
	if (windowid >= 0) {
		char number[32];
		snprintf(number, sizeof(number), "%lld", windowid);
		if (append_string(buffer, number) < 0)
			goto fail;
	} else if (append_string(buffer, "null") < 0) {
		goto fail;
	}
	if (append_string(buffer, ",\"workspace\":") < 0 ||
	    (workspace ? append_json_string(buffer, workspace)
	               : append_string(buffer, "null")) < 0 ||
	    append_string(buffer, "}") < 0)
		goto fail;
	size_t payload = buffer->length - start - sizeof(length);
	if (payload > AEROSPACE_MAX_FRAME)
		goto fail;
	length = (uint32_t)payload;
	memcpy(buffer->data + start, &length, sizeof(length));
	return 0;

fail:
	buffer->length = start;
	return -1;
}

long
aero_next_frame(const char *data, size_t length, const char **payload,
    size_t *payloadlength)
{
	uint32_t size;
	if (length < sizeof(size))
		return 0;
	memcpy(&size, data, sizeof(size));
	if (size > AEROSPACE_MAX_FRAME)
		return -1;
	if (length - sizeof(size) < size)
		return 0;
	*payload = data + sizeof(size);
	*payloadlength = size;
	return (long)(sizeof(size) + size);
}

/* JSON: just enough to read the members of a flat object, skipping any
 * nested value. */

static const char *
skip_space(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

static int
hex4(const char *p, const char *end, unsigned *value)
{
	if (end - p < 4)
		return -1;
	*value = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		unsigned digit;
		if (c >= '0' && c <= '9') digit = c - '0';
		else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
		else return -1;
		*value = *value << 4 | digit;
	}
	return 0;
}

static int
append_utf8(AeroBuffer *buffer, unsigned code)
{
	unsigned char bytes[4];
	size_t n;
	if (code < 0x80) {
		bytes[0] = code;
		n = 1;
	} else if (code < 0x800) {
		bytes[0] = 0xc0 | code >> 6;
		bytes[1] = 0x80 | (code & 0x3f);
		n = 2;
	} else if (code < 0x10000) {
		bytes[0] = 0xe0 | code >> 12;
		bytes[1] = 0x80 | (code >> 6 & 0x3f);
		bytes[2] = 0x80 | (code & 0x3f);
		n = 3;
	} else {
		bytes[0] = 0xf0 | code >> 18;
		bytes[1] = 0x80 | (code >> 12 & 0x3f);
		bytes[2] = 0x80 | (code >> 6 & 0x3f);
		bytes[3] = 0x80 | (code & 0x3f);
		n = 4;
	}
	return aero_buffer_append(buffer, bytes, n);
}

/* p points at the opening quote. Returns the position after the closing
 * one; decodes into out (NUL-terminated) unless out is NULL. */
static const char *
parse_string(const char *p, const char *end, AeroBuffer *out)
{
	if (p >= end || *p != '"')
		return NULL;
	for (p++; p < end; p++) {
		unsigned char c = *p;
		if (c == '"') {
			if (out && aero_buffer_append(out, "", 1) < 0)
				return NULL;
			return p + 1;
		}
		if (c < 0x20)
			return NULL;
		if (c != '\\') {
			if (out && aero_buffer_append(out, p, 1) < 0)
				return NULL;
			continue;
		}
		if (++p >= end)
			return NULL;
		unsigned code;
		switch (*p) {
		case '"': code = '"'; break;
		case '\\': code = '\\'; break;
		case '/': code = '/'; break;
		case 'b': code = '\b'; break;
		case 'f': code = '\f'; break;
		case 'n': code = '\n'; break;
		case 'r': code = '\r'; break;
		case 't': code = '\t'; break;
		case 'u':
			if (hex4(p + 1, end, &code) < 0)
				return NULL;
			p += 4;
			if (code >= 0xd800 && code < 0xdc00) {
				unsigned low;
				if (end - p > 6 && p[1] == '\\' && p[2] == 'u' &&
				    hex4(p + 3, end, &low) == 0 &&
				    low >= 0xdc00 && low < 0xe000) {
					code = 0x10000 + ((code - 0xd800) << 10) +
					    (low - 0xdc00);
					p += 6;
				} else {
					code = 0xfffd;
				}
			} else if (code >= 0xdc00 && code < 0xe000) {
				code = 0xfffd;
			}
			break;
		default:
			return NULL;
		}
		/* A NUL would silently cut the string short. */
		if (code == 0)
			return NULL;
		if (out && append_utf8(out, code) < 0)
			return NULL;
	}
	return NULL;
}

static const char *
skip_number(const char *p, const char *end)
{
	const char *start = p;
	if (p < end && *p == '-')
		p++;
	const char *digits = p;
	while (p < end && *p >= '0' && *p <= '9')
		p++;
	if (p == digits)
		return NULL;
	if (p < end && *p == '.') {
		const char *fraction = ++p;
		while (p < end && *p >= '0' && *p <= '9')
			p++;
		if (p == fraction)
			return NULL;
	}
	if (p < end && (*p == 'e' || *p == 'E')) {
		p++;
		if (p < end && (*p == '+' || *p == '-'))
			p++;
		const char *exponent = p;
		while (p < end && *p >= '0' && *p <= '9')
			p++;
		if (p == exponent)
			return NULL;
	}
	return p > start ? p : NULL;
}

static const char *
skip_value(const char *p, const char *end, int depth)
{
	p = skip_space(p, end);
	if (p >= end || depth > 64)
		return NULL;
	if (*p == '"')
		return parse_string(p, end, NULL);
	if (*p == '{' || *p == '[') {
		char close = *p == '{' ? '}' : ']';
		p = skip_space(p + 1, end);
		if (p < end && *p == close)
			return p + 1;
		for (;;) {
			if (close == '}') {
				p = parse_string(skip_space(p, end), end, NULL);
				if (!p)
					return NULL;
				p = skip_space(p, end);
				if (p >= end || *p++ != ':')
					return NULL;
			}
			p = skip_value(p, end, depth + 1);
			if (!p)
				return NULL;
			p = skip_space(p, end);
			if (p >= end)
				return NULL;
			if (*p == close)
				return p + 1;
			if (*p++ != ',')
				return NULL;
		}
	}
	static const char *const literals[] = {"true", "false", "null"};
	for (size_t i = 0; i < sizeof(literals) / sizeof(*literals); i++) {
		size_t n = strlen(literals[i]);
		if ((size_t)(end - p) >= n && !memcmp(p, literals[i], n))
			return p + n;
	}
	return skip_number(p, end);
}

/* The value of a top-level member: 1 and *value, 0 if absent, -1 if the
 * object is malformed. */
static int
find_member(const char *json, size_t length, const char *key,
    const char **value)
{
	const char *p = json, *end = json + length;
	p = skip_space(p, end);
	if (p >= end || *p++ != '{')
		return -1;
	p = skip_space(p, end);
	if (p < end && *p == '}')
		return 0;
	int found = 0;
	AeroBuffer name = {0};
	for (;;) {
		name.length = 0;
		p = parse_string(skip_space(p, end), end, &name);
		if (!p)
			goto malformed;
		p = skip_space(p, end);
		if (p >= end || *p++ != ':')
			goto malformed;
		p = skip_space(p, end);
		if (!found && !strcmp(name.data, key)) {
			*value = p;
			found = 1;
		}
		p = skip_value(p, end, 0);
		if (!p)
			goto malformed;
		p = skip_space(p, end);
		if (p >= end)
			goto malformed;
		if (*p == '}')
			break;
		if (*p++ != ',')
			goto malformed;
	}
	/* Only trailing whitespace may follow the object. */
	if (skip_space(p + 1, end) != end)
		goto malformed;
	aero_buffer_free(&name);
	return found;

malformed:
	aero_buffer_free(&name);
	return -1;
}

static int
is_null(const char *p, const char *end)
{
	return end - p >= 4 && !memcmp(p, "null", 4);
}

int
aero_json_string(const char *json, size_t length, const char *key, char **out)
{
	const char *value, *end = json + length;
	*out = NULL;
	int found = find_member(json, length, key, &value);
	if (found <= 0)
		return found;
	if (is_null(value, end))
		return 0;
	AeroBuffer decoded = {0};
	if (!parse_string(value, end, &decoded)) {
		aero_buffer_free(&decoded);
		return -1;
	}
	*out = decoded.data;
	return 1;
}

int
aero_json_integer(const char *json, size_t length, const char *key,
    long long *out)
{
	const char *value, *end = json + length;
	int found = find_member(json, length, key, &value);
	if (found <= 0)
		return found;
	if (is_null(value, end))
		return 0;
	const char *after = skip_number(value, end);
	char digits[32];
	if (!after || (size_t)(after - value) >= sizeof(digits))
		return -1;
	memcpy(digits, value, after - value);
	digits[after - value] = '\0';
	char *stop;
	errno = 0;
	long long number = strtoll(digits, &stop, 10);
	if (errno || *stop)
		return -1; /* a fraction, exponent or overflow */
	*out = number;
	return 1;
}

int
aero_parse_event(const char *json, size_t length, AeroEvent *event)
{
	memset(event, 0, sizeof(*event));
	long long window;
	int found;
	if (aero_json_string(json, length, "_event", &event->event) != 1)
		goto invalid;
	found = aero_json_integer(json, length, "windowId", &window);
	if (found < 0 || (found && window < 0))
		goto invalid;
	event->haswindow = found;
	event->windowid = found ? (unsigned long long)window : 0;
	if (aero_json_string(json, length, "workspace", &event->workspace) < 0)
		goto invalid;
	return 0;

invalid:
	aero_free_event(event);
	return -1;
}

void
aero_free_event(AeroEvent *event)
{
	free(event->event);
	free(event->workspace);
	memset(event, 0, sizeof(*event));
}

int
aero_parse_answer(const char *json, size_t length, AeroAnswer *answer)
{
	memset(answer, 0, sizeof(*answer));
	if (aero_json_integer(json, length, "exitCode", &answer->exitcode) != 1 ||
	    aero_json_string(json, length, "stdout", &answer->out) < 0 ||
	    aero_json_string(json, length, "stderr", &answer->err) < 0) {
		aero_free_answer(answer);
		return -1;
	}
	return 0;
}

void
aero_free_answer(AeroAnswer *answer)
{
	free(answer->out);
	free(answer->err);
	memset(answer, 0, sizeof(*answer));
}

long long
launch_parse_uint(const char *text, unsigned long long max)
{
	if (!text || !*text)
		return -1;
	unsigned long long value = 0;
	for (const char *p = text; *p; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		value = value * 10 + (*p - '0');
		if (value > max)
			return -1;
	}
	return (long long)value;
}

int
launch_parse_slot(const char *line, pid_t *pid, unsigned long long *window,
    char *stamp, size_t stampsize)
{
	char pidtext[24], windowtext[24], stamptext[128];
	if (sscanf(line, "%23s %23s %127s", pidtext, windowtext, stamptext) != 3)
		return -1;
	long long p = launch_parse_uint(pidtext, INT32_MAX);
	long long w = launch_parse_uint(windowtext, UINT32_MAX);
	if (p <= 1 || w < 0 || strlen(stamptext) >= stampsize)
		return -1;
	*pid = (pid_t)p;
	*window = (unsigned long long)w;
	strcpy(stamp, stamptext);
	return 0;
}

pid_t
launch_focused_pid(const char *output, const char *bundle)
{
	size_t n = strlen(bundle);
	if (strncmp(output, bundle, n) || output[n] != ' ')
		return -1;
	char text[24];
	size_t length = strcspn(output + n + 1, "\n");
	/* Only one line, and nothing but trailing newlines after it. */
	if (length >= sizeof(text) || output[n + 1 + length + strspn(
	    output + n + 1 + length, "\n")])
		return -1;
	memcpy(text, output + n + 1, length);
	text[length] = '\0';
	long long pid = launch_parse_uint(text, INT32_MAX);
	return pid > 0 ? (pid_t)pid : -1;
}

int
launch_window_workspace(const char *output, unsigned long long window,
    char **workspace)
{
	*workspace = NULL;
	size_t line = strcspn(output, "\n");
	const char *space = memchr(output, ' ', line);
	if (!space)
		return 0;
	char text[24];
	size_t n = space - output;
	if (n >= sizeof(text))
		return 0;
	memcpy(text, output, n);
	text[n] = '\0';
	long long id = launch_parse_uint(text, UINT32_MAX);
	if (id < 0 || (unsigned long long)id != window)
		return 0;
	*workspace = strndup(space + 1, line - n - 1);
	return *workspace != NULL;
}

int
launch_read_venv(const char *context, char **venv)
{
	*venv = NULL;
	struct stat info;
	if (lstat(context, &info) < 0 || !S_ISREG(info.st_mode))
		return 0;
	FILE *file = fopen(context, "r");
	if (!file)
		return 0;
	char *line = NULL;
	size_t capacity = 0;
	ssize_t length = getline(&line, &capacity, file);
	fclose(file);
	/* A line without its newline was cut short (the shell replaces the
	 * file atomically, so this is not one of its own). */
	if (length <= 1 || line[length - 1] != '\n') {
		free(line);
		return 0;
	}
	line[length - 1] = '\0';
	char activate[4096];
	if (snprintf(activate, sizeof(activate), "%s/bin/activate", line) >=
	    (int)sizeof(activate) || stat(activate, &info) < 0 ||
	    !S_ISREG(info.st_mode)) {
		free(line);
		return 0;
	}
	*venv = line;
	return 1;
}

int
launch_args_name_spare(const char *buffer, size_t size)
{
	int argc;
	if (size < sizeof(argc))
		return 0;
	memcpy(&argc, buffer, sizeof(argc));
	const char *p = buffer + sizeof(argc), *end = buffer + size;
	p = memchr(p, '\0', end - p); /* the executable path */
	if (!p)
		return 0;
	while (p < end && !*p)
		p++;
	for (int i = 0; i < argc && p < end; i++) {
		const char *next = memchr(p, '\0', end - p);
		if (!next)
			return 0;
		if (!strcmp(p, "--spare") || !strcmp(p, "--spare-slot"))
			return 1;
		p = next + 1;
	}
	return 0;
}
