#ifndef ST_MACOS_NATIVE_H
#define ST_MACOS_NATIVE_H

#include <stddef.h>
#include "renderer.h"

typedef struct {
	int tw, th;
	int w, h;
	int ch;
	int cw;
	int mode;
	int cursor;
} TermWindow;

extern TermWindow win;
extern char *usedfont;
extern double usedfontsize;

void macos_request_redraw(void);
void macos_draw_overlay_rect(double x, double y, double width,
		double height, MacColor color);
void macos_draw_overlay_text(const char *text, size_t len, double x,
		double baseline, double font_scale, MacColor color);
double macos_measure_text(const char *text, size_t len, double font_scale);
MacColor macos_parse_color(const char *value, MacColor fallback);

/* Read-only command-line presentation snapshot supplied by cmdline.c. */
typedef struct {
	int state;
	int mode;
	char prefix;
	const char *input;
	int input_len;
	int cursor;
	int anchor;
	const char *error;
	int x, y, width, height;
	int content_y, content_height, baseline;
} MacCmdlineSnapshot;

void macos_draw_cmdline_snapshot(const MacCmdlineSnapshot *snapshot);

#endif
