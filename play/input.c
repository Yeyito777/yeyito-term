/* st-play input parser (see input.h). */
#include "input.h"

#include <string.h>

enum {
	S_GROUND,
	S_ESC,
	S_CSI,
	S_SS3,
	S_APC,
	S_APC_ESC,
	S_STRING,	/* OSC, DCS, PM, SOS: skipped */
	S_STRING_ESC,
};

void
input_init(InputParser *parser)
{
	memset(parser, 0, sizeof(*parser));
}

static void
emit_key(int key, InputCallback callback, void *context)
{
	InputEvent event = { INPUT_KEY, key, 0, 0, NULL, 0 };
	callback(&event, context);
}

static void
finish_csi(InputParser *parser, unsigned char final, InputCallback callback,
		void *context)
{
	InputEvent event;
	int *p = parser->params, n = parser->nparams;

	memset(&event, 0, sizeof(event));
	if (parser->private_marker == '?' && final == 'c') {
		event.type = INPUT_DA1;
		callback(&event, context);
		return;
	}
	if (parser->private_marker)
		return;
	switch (final) {
	case 'A': emit_key(KEY_UP, callback, context); break;
	case 'B': emit_key(KEY_DOWN, callback, context); break;
	case 'C': emit_key(KEY_RIGHT, callback, context); break;
	case 'D': emit_key(KEY_LEFT, callback, context); break;
	case 'H': emit_key(KEY_HOME, callback, context); break;
	case 'F': emit_key(KEY_END, callback, context); break;
	case '~':
		if (n >= 1 && p[0] == 5)
			emit_key(KEY_PAGE_UP, callback, context);
		else if (n >= 1 && p[0] == 6)
			emit_key(KEY_PAGE_DOWN, callback, context);
		else if (n >= 1 && (p[0] == 1 || p[0] == 7))
			emit_key(KEY_HOME, callback, context);
		else if (n >= 1 && (p[0] == 4 || p[0] == 8))
			emit_key(KEY_END, callback, context);
		break;
	case 't':
		if (n >= 3 && (p[0] == 6 || p[0] == 4)) {
			event.type = p[0] == 6 ? INPUT_CELL_SIZE : INPUT_TEXT_AREA;
			event.height = p[1];
			event.width = p[2];
			callback(&event, context);
		}
		break;
	}
}

void
input_feed(InputParser *parser, const unsigned char *data, size_t length,
		InputCallback callback, void *context)
{
	size_t i;

	for (i = 0; i < length; i++) {
		unsigned char c = data[i];

		switch (parser->state) {
		case S_GROUND:
			if (c == 0x1b) {
				parser->state = S_ESC;
				parser->esc_alone = length == 1;
			} else {
				emit_key(c, callback, context);
			}
			break;
		case S_ESC:
			parser->nparams = 0;
			parser->private_marker = 0;
			memset(parser->params, 0, sizeof(parser->params));
			parser->length = 0;
			parser->overflow = 0;
			if (c == '[') {
				parser->state = S_CSI;
			} else if (c == 'O') {
				parser->state = S_SS3;
			} else if (c == '_') {
				parser->state = S_APC;
			} else if (c == ']' || c == 'P' || c == '^' || c == 'X') {
				parser->state = S_STRING;
			} else if (c == 0x1b) {
				/* ESC ESC: the first was a key. */
				emit_key(KEY_ESCAPE, callback, context);
				parser->esc_alone = 0;
			} else {
				/* Alt+key: deliver the key itself. */
				parser->state = S_GROUND;
				emit_key(c, callback, context);
			}
			break;
		case S_CSI:
			if (c >= '0' && c <= '9') {
				if (!parser->nparams)
					parser->nparams = 1;
				if (parser->nparams <= 8) {
					int *p = &parser->params[parser->nparams - 1];
					if (*p < 100000)
						*p = *p * 10 + (c - '0');
				}
			} else if (c == ';' || c == ':') {
				if (!parser->nparams)
					parser->nparams = 1;
				parser->nparams++;
			} else if (c >= 0x3c && c <= 0x3f) {
				parser->private_marker = c;
			} else if (c >= 0x40 && c <= 0x7e) {
				parser->state = S_GROUND;
				if (parser->nparams > 8)
					parser->nparams = 8;
				finish_csi(parser, c, callback, context);
			} else if (c == 0x1b) {
				parser->state = S_ESC;
				parser->esc_alone = 0;
			} else if (c < 0x20 || c > 0x7e) {
				parser->state = S_GROUND;	/* malformed */
			}
			break;
		case S_SS3:
			parser->state = S_GROUND;
			switch (c) {
			case 'A': emit_key(KEY_UP, callback, context); break;
			case 'B': emit_key(KEY_DOWN, callback, context); break;
			case 'C': emit_key(KEY_RIGHT, callback, context); break;
			case 'D': emit_key(KEY_LEFT, callback, context); break;
			case 'H': emit_key(KEY_HOME, callback, context); break;
			case 'F': emit_key(KEY_END, callback, context); break;
			}
			break;
		case S_APC:
			if (c == 0x1b) {
				parser->state = S_APC_ESC;
			} else if (c == 0x07) {
				goto apc_done;	/* tolerate BEL-terminated */
			} else if (parser->length < INPUT_APC_MAX) {
				parser->buf[parser->length++] = (char)c;
			} else {
				parser->overflow = 1;
			}
			break;
		case S_APC_ESC:
			if (c != '\\') {
				/* Not ST: drop the APC, reparse this byte. */
				parser->state = S_ESC;
				parser->esc_alone = 0;
				i--;
				break;
			}
apc_done:
			parser->state = S_GROUND;
			if (!parser->overflow && parser->length >= 1 &&
			    parser->buf[0] == 'G') {
				InputEvent event;
				memset(&event, 0, sizeof(event));
				event.type = INPUT_APC;
				parser->buf[parser->length] = '\0';
				event.text = parser->buf + 1;
				event.length = parser->length - 1;
				callback(&event, context);
			}
			break;
		case S_STRING:
			if (c == 0x1b)
				parser->state = S_STRING_ESC;
			else if (c == 0x07)
				parser->state = S_GROUND;
			break;
		case S_STRING_ESC:
			parser->state = c == '\\' ? S_GROUND : S_STRING;
			break;
		}
	}
}

int
input_pending_escape(const InputParser *parser)
{
	return parser->state == S_ESC;
}

void
input_timeout(InputParser *parser, InputCallback callback, void *context)
{
	if (parser->state != S_ESC)
		return;
	parser->state = S_GROUND;
	if (parser->esc_alone)
		emit_key(KEY_ESCAPE, callback, context);
	parser->esc_alone = 0;
}

static int
word_is(const char *s, size_t n, const char *word)
{
	size_t w = strlen(word);
	return n == w && !memcmp(s, word, w);
}

static long long
number(const char *s, size_t n, int *ok)
{
	long long v = 0;
	size_t i = 0;
	int negative = 0;

	if (n && s[0] == '-') {
		negative = 1;
		i = 1;
	}
	if (i == n || n - i > 18) {
		*ok = 0;
		return 0;
	}
	for (; i < n; i++) {
		if (s[i] < '0' || s[i] > '9') {
			*ok = 0;
			return 0;
		}
		v = v * 10 + (s[i] - '0');
	}
	*ok = 1;
	return negative ? -v : v;
}

int
reply_parse(const char *text, size_t length, Reply *reply)
{
	const char *end = text + length, *p = text, *semi;
	static const char *states[] = { "buffering", "playing", "paused", "ended" };

	memset(reply, 0, sizeof(*reply));
	reply->state = -1;
	reply->pos = reply->vbuf = reply->abuf = -1;
	reply->shown = reply->dropped = reply->rx = -1;
	semi = memchr(text, ';', length);
	if (!semi)
		return 0;
	/* Controls: comma-separated key=value; we need i. */
	while (p < semi) {
		const char *comma = memchr(p, ',', (size_t)(semi - p));
		const char *field_end = comma ? comma : semi;
		if (field_end - p > 2 && p[0] == 'i' && p[1] == '=') {
			int ok;
			long long v = number(p + 2, (size_t)(field_end - p - 2), &ok);
			if (ok && v > 0)
				reply->id = (unsigned long)v;
		}
		p = field_end + (comma ? 1 : 0);
		if (!comma)
			break;
	}
	if (!reply->id)
		return 0;
	reply->message = semi + 1;
	reply->message_length = (size_t)(end - semi - 1);
	reply->ok = reply->message_length >= 2 && !memcmp(reply->message, "OK", 2);
	/* Message fields: space-separated words, key=value. */
	p = reply->message;
	while (p < end) {
		const char *word, *equals;
		size_t n, keylen;
		int ok;
		long long v;

		while (p < end && *p == ' ')
			p++;
		word = p;
		while (p < end && *p != ' ')
			p++;
		n = (size_t)(p - word);
		if (!n)
			continue;
		equals = memchr(word, '=', n);
		if (!equals)
			continue;
		keylen = (size_t)(equals - word);
		if (word_is(word, keylen, "state")) {
			size_t i;
			for (i = 0; i < 4; i++)
				if (word_is(equals + 1, n - keylen - 1, states[i]))
					reply->state = (int)i;
			continue;
		}
		if (word_is(word, keylen, "error")) {
			size_t m = n - keylen - 1;
			if (m >= sizeof(reply->error))
				m = sizeof(reply->error) - 1;
			memcpy(reply->error, equals + 1, m);
			reply->error[m] = '\0';
			continue;
		}
		if (word_is(word, keylen, "video")) {
			const char *v = equals + 1;
			size_t m = n - keylen - 1;
			/* comma-separated codec list */
			while (m) {
				const char *c = memchr(v, ',', m);
				size_t k = c ? (size_t)(c - v) : m;
				if (word_is(v, k, "h264"))
					reply->video_h264 = 1;
				if (!c)
					break;
				m -= k + 1;
				v = c + 1;
			}
			continue;
		}
		if (word_is(word, keylen, "audio")) {
			const char *v = equals + 1;
			size_t m = n - keylen - 1;
			while (m) {
				const char *c = memchr(v, ',', m);
				size_t k = c ? (size_t)(c - v) : m;
				if (word_is(v, k, "aac"))
					reply->audio_aac = 1;
				if (!c)
					break;
				m -= k + 1;
				v = c + 1;
			}
			continue;
		}
		v = number(equals + 1, n - keylen - 1, &ok);
		if (!ok)
			continue;
		if (word_is(word, keylen, "seq")) {
			reply->has_seq = 1;
			reply->seq = (unsigned long)v;
		} else if (word_is(word, keylen, "pos")) {
			reply->pos = v;
		} else if (word_is(word, keylen, "vbuf")) {
			reply->vbuf = v;
		} else if (word_is(word, keylen, "abuf")) {
			reply->abuf = v;
		} else if (word_is(word, keylen, "shown")) {
			reply->shown = v;
		} else if (word_is(word, keylen, "dropped")) {
			reply->dropped = v;
		} else if (word_is(word, keylen, "rx")) {
			reply->rx = v;
		}
	}
	return 1;
}
