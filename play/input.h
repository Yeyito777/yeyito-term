/* st-play input: splits the bytes read from the terminal into keypresses
 * and the terminal's replies (graphics APCs, DA1, XTWINOPS sizes). */
#ifndef PLAY_INPUT_H
#define PLAY_INPUT_H

#include <stddef.h>

#define INPUT_APC_MAX 4096U
/* A lone ESC is the Escape key only if nothing follows it this long. */
#define INPUT_ESC_TIMEOUT 0.1

enum {
	KEY_UP = 0x110000,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_PAGE_UP,
	KEY_PAGE_DOWN,
	KEY_HOME,
	KEY_END,
	KEY_ESCAPE,
};

enum {
	INPUT_KEY,	/* key: a byte, or KEY_* */
	INPUT_APC,	/* text/length: the APC body after "G" */
	INPUT_DA1,
	INPUT_CELL_SIZE,	/* CSI 6 ; height ; width t */
	INPUT_TEXT_AREA,	/* CSI 4 ; height ; width t */
};

typedef struct {
	int type;
	int key;
	int width, height;
	const char *text;
	size_t length;
} InputEvent;

typedef void (*InputCallback)(const InputEvent *event, void *context);

typedef struct {
	int state;
	int esc_alone;		/* the pending ESC arrived as a read by itself */
	char buf[INPUT_APC_MAX + 1];
	size_t length;
	int overflow;
	int params[8];
	int nparams;
	int private_marker;
} InputParser;

void input_init(InputParser *parser);
/* Feed one read().  Events are delivered through the callback. */
void input_feed(InputParser *parser, const unsigned char *data, size_t length,
		InputCallback callback, void *context);
/* 1 if an ESC is pending (call input_timeout after INPUT_ESC_TIMEOUT). */
int input_pending_escape(const InputParser *parser);
/* No more bytes came: a lone ESC becomes KEY_ESCAPE, anything else that
 * was half parsed is dropped. */
void input_timeout(InputParser *parser, InputCallback callback, void *context);

/* A graphics reply "i=<id>[,...];<message>" (the APC body after "G"). */
typedef struct {
	unsigned long id;
	const char *message;
	size_t message_length;
	int ok;			/* message starts with "OK" */
	/* k=q status fields (absent: has_seq 0, numbers -1, state -1). */
	int has_seq;
	unsigned long seq;
	int state;		/* VIDEO_STATE_* order: buffering playing paused ended */
	long long pos, vbuf, abuf, shown, dropped, rx;
	char error[48];
	/* k=c capabilities */
	int video_h264, audio_aac;
} Reply;

int reply_parse(const char *text, size_t length, Reply *reply);

#endif
