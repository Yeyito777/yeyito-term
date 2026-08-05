/* Kitty OSC 5522 clipboard-read and paste-event protocol helpers. */
#include "clipboard5522.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CLIP5522_MAX_PAYLOAD 4096
#define CLIP5522_MAX_METADATA 1024

static const char b64digits[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int
b64value(unsigned char c)
{
	const char *p = strchr(b64digits, c);
	return p ? (int)(p - b64digits) : -1;
}

/* Strict RFC 4648 decoder.  Clipboard protocol inputs are ASCII. */
static int
b64decode(const char *src, unsigned char **out, size_t *outlen)
{
	size_t len = strlen(src), i, used = 0;
	unsigned char *dst;

	if (!len || len % 4 || len > CLIP5522_MAX_PAYLOAD)
		return 0;
	dst = malloc(len / 4 * 3 + 1);
	if (!dst)
		return 0;
	for (i = 0; i < len; i += 4) {
		int a = b64value((unsigned char)src[i]);
		int b = b64value((unsigned char)src[i + 1]);
		int c = src[i + 2] == '=' ? -2 : b64value((unsigned char)src[i + 2]);
		int d = src[i + 3] == '=' ? -2 : b64value((unsigned char)src[i + 3]);
		if (a < 0 || b < 0 || c == -1 || d == -1 ||
		    (c == -2 && d != -2) ||
		    ((c == -2 || d == -2) && i + 4 != len))
			goto invalid;
		dst[used++] = (a << 2) | (b >> 4);
		if (c != -2) {
			dst[used++] = (b << 4) | (c >> 2);
			if (d != -2)
				dst[used++] = (c << 6) | d;
		}
	}
	dst[used] = '\0';
	*out = dst;
	*outlen = used;
	return 1;
invalid:
	free(dst);
	return 0;
}

int
clip5522_base64(const unsigned char *src, size_t len, char **out)
{
	size_t i, n = ((len + 2) / 3) * 4;
	char *dst = malloc(n + 1);

	if (!dst)
		return 0;
	for (i = 0; i + 2 < len; i += 3) {
		*dst++ = b64digits[src[i] >> 2];
		*dst++ = b64digits[((src[i] & 3) << 4) | (src[i + 1] >> 4)];
		*dst++ = b64digits[((src[i + 1] & 15) << 2) | (src[i + 2] >> 6)];
		*dst++ = b64digits[src[i + 2] & 63];
	}
	if (len - i == 1) {
		*dst++ = b64digits[src[i] >> 2];
		*dst++ = b64digits[(src[i] & 3) << 4];
		*dst++ = '=';
		*dst++ = '=';
	} else if (len - i == 2) {
		*dst++ = b64digits[src[i] >> 2];
		*dst++ = b64digits[((src[i] & 3) << 4) | (src[i + 1] >> 4)];
		*dst++ = b64digits[(src[i + 1] & 15) << 2];
		*dst++ = '=';
	}
	*dst = '\0';
	/* dst was advanced; recover the allocation without relying on callers. */
	*out = dst - n;
	return 1;
}

int
clip5522_mime_is_valid(const char *mime)
{
	const unsigned char *p = (const unsigned char *)mime;
	int slash = 0;

	if (!mime || !*mime || strlen(mime) > 255)
		return 0;
	if (!strcmp(mime, "."))
		return 1;
	for (; *p; p++) {
		if (*p == '/') {
			if (p == (const unsigned char *)mime || p[1] == '\0' || slash)
				return 0;
			slash = 1;
		} else if (!isprint(*p) || isspace(*p))
			return 0;
	}
	return slash;
}

void
clip5522_request_free(Clip5522Request *request)
{
	size_t i;
	for (i = 0; i < request->nmimes; i++)
		free(request->mimes[i]);
	free(request->mimes);
	free(request->password);
	memset(request, 0, sizeof(*request));
}

int
clip5522_parse_read(const char *metadata, const char *payload,
		Clip5522Request *request)
{
	char *copy, *field, *next, *list;
	unsigned char *decoded = NULL;
	size_t length, i;
	int type_read = 0;

	memset(request, 0, sizeof(*request));
	if (!metadata || !payload || strlen(metadata) > CLIP5522_MAX_METADATA)
		return 0;
	copy = strdup(metadata);
	if (!copy)
		return 0;
	for (field = copy; field; field = next) {
		next = strchr(field, ':');
		if (next)
			*next++ = '\0';
		if (!strcmp(field, "type=read"))
			type_read = 1;
		else if (!strcmp(field, "loc=primary"))
			request->primary = 1;
		else if (!strncmp(field, "pw=", 3) && !request->password) {
			if (!field[3] || strlen(field + 3) > 256)
				goto invalid;
			request->password = strdup(field + 3);
			if (!request->password)
				goto invalid;
		} else if (!strncmp(field, "name=", 5)) {
			unsigned char *name;
			size_t namelen;
			if (!b64decode(field + 5, &name, &namelen))
				goto invalid;
			request->paste_event_name = namelen == 11 &&
				!memcmp(name, "Paste event", 11);
			free(name);
		}
	}
	free(copy);
	if (!type_read || !b64decode(payload, &decoded, &length) ||
	    memchr(decoded, '\0', length))
		goto invalid_no_copy;
	list = (char *)decoded;
	while (*list) {
		char *end;
		while (isspace((unsigned char)*list)) list++;
		if (!*list) break;
		end = list;
		while (*end && !isspace((unsigned char)*end)) end++;
		if (*end) *end++ = '\0';
		if (!clip5522_mime_is_valid(list) || request->nmimes == CLIP5522_MAX_MIMES)
			goto invalid_no_copy;
		for (i = 0; i < request->nmimes; i++)
			if (!strcmp(request->mimes[i], list))
				break;
		if (i == request->nmimes) {
			char **mimes = realloc(request->mimes,
				(request->nmimes + 1) * sizeof(*mimes));
			if (!mimes) goto invalid_no_copy;
			request->mimes = mimes;
			request->mimes[request->nmimes] = strdup(list);
			if (!request->mimes[request->nmimes]) goto invalid_no_copy;
			request->nmimes++;
		}
		list = end;
	}
	free(decoded);
	return request->nmimes != 0;
invalid:
	free(copy);
invalid_no_copy:
	free(decoded);
	clip5522_request_free(request);
	return 0;
}

char *
clip5522_join_mimes(char *const *mimes, size_t nmimes)
{
	size_t i, size = 1;
	char *result, *p;
	for (i = 0; i < nmimes; i++) size += strlen(mimes[i]) + (i != 0);
	result = malloc(size);
	if (!result) return NULL;
	for (p = result, i = 0; i < nmimes; i++) {
		if (i) *p++ = ' ';
		p += strlen(strcpy(p, mimes[i]));
	}
	*p = '\0';
	return result;
}

static void
emit(Clip5522Write write, void *context, const char *metadata,
	 const char *payload)
{
	size_t size = 7 + strlen(metadata) + 2 + (payload ? strlen(payload) : 0) + 2;
	char *message = malloc(size + 1);
	if (!message) return;
	if (payload)
		snprintf(message, size + 1, "\033]5522;%s;%s\033\\", metadata, payload);
	else
		snprintf(message, size + 1, "\033]5522;%s\033\\", metadata);
	write(message, strlen(message), context);
	free(message);
}

void
clip5522_status(Clip5522Write write, void *context, const char *status,
		int primary, const char *password)
{
	char metadata[512];
	snprintf(metadata, sizeof(metadata), "type=read:status=%s%s%s%s%s",
		status, primary ? ":loc=primary" : "", password ? ":pw=" : "",
		password ? password : "", "");
	emit(write, context, metadata, NULL);
}

static void
clip5522_data_one(Clip5522Write write, void *context, const char *mime,
		const unsigned char *data, size_t len, const char *password)
{
	char *encoded = NULL, *encoded_mime = NULL, *metadata = NULL;
	size_t size;
	if (!clip5522_base64((const unsigned char *)mime, strlen(mime), &encoded_mime) ||
	    !clip5522_base64(data, len, &encoded))
		goto out;
	size = strlen(encoded_mime) + (password ? strlen(password) : 0) + 64;
	metadata = malloc(size);
	if (metadata) {
		snprintf(metadata, size, "type=read:status=DATA:mime=%s%s%s",
			encoded_mime, password ? ":pw=" : "", password ? password : "");
		emit(write, context, metadata, encoded);
	}
out:
	free(metadata);
	free(encoded_mime);
	free(encoded);
}

void
clip5522_data(Clip5522Write write, void *context, const char *mime,
		const unsigned char *data, size_t len, const char *password)
{
	size_t offset = 0;
	/* The protocol limit is on raw bytes, not their base64 representation. */
	do {
		size_t chunk = len - offset;
		if (chunk > CLIP5522_RAW_CHUNK)
			chunk = CLIP5522_RAW_CHUNK;
		clip5522_data_one(write, context, mime, data + offset, chunk, password);
		offset += chunk;
	} while (offset < len);
}
