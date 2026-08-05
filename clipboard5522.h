/* Kitty OSC 5522 clipboard-read and paste-event protocol helpers. */
#ifndef CLIPBOARD5522_H
#define CLIPBOARD5522_H

#include <stddef.h>

#define CLIP5522_RAW_CHUNK 4096
#define CLIP5522_MAX_MIMES 32

typedef struct {
	int primary;
	int paste_event_name;
	char *password;          /* encoded pw metadata value, if present */
	char **mimes;
	size_t nmimes;
} Clip5522Request;

typedef void (*Clip5522Write)(const char *, size_t, void *);

int clip5522_parse_read(const char *metadata, const char *payload,
		Clip5522Request *request);
void clip5522_request_free(Clip5522Request *request);
int clip5522_mime_is_valid(const char *mime);
int clip5522_base64(const unsigned char *src, size_t len, char **out);
char *clip5522_join_mimes(char *const *mimes, size_t nmimes);

void clip5522_status(Clip5522Write write, void *context, const char *status,
		int primary, const char *password);
void clip5522_data(Clip5522Write write, void *context, const char *mime,
		const unsigned char *data, size_t len, const char *password);

#endif
