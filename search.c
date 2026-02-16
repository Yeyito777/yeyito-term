#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "st.h"
#include "vimnav.h"
#include "search.h"

/* Term struct redeclaration (same pattern as cmdline.c, sshind.c) */
typedef struct {
	int row;
	int col;
	int maxcol;
	Line *line;
	Line *alt;
	Line hist[(1 << 15)]; /* HISTSIZE */
	int histi;
	int scr;
	int *dirty;
	/* TCursor inline */
	struct { Glyph attr; int x; int y; char state; } c;
	int ocx;
	int ocy;
	int top;
	int bot;
	int mode;
	int esc;
	char trantbl[4];
	int charset;
	int icharset;
	int *tabs;
	Rune lastc;
	int histn;
} Term_search;

extern Term_search term;
extern int debug_mode;

#ifndef HISTSIZE
#define HISTSIZE (1 << 15)
#endif
#define TLINE_S(y) ((y) < term.scr ? term.hist[((y) + term.histi - \
		term.scr + HISTSIZE + 1) % HISTSIZE] : \
		term.line[(y) - term.scr])

#define SEARCH_MAX_LINE_MATCHES 64
#define SEARCH_MAX_LINE_BYTES   (4 * 4096)

static struct {
	char pattern[256];
	regex_t regex;
	int compiled;
	int active;
	int direction; /* 1=forward (/), -1=backward (?) */
	/* Per-line render cache */
	int cache_y;
	struct { int start, end; } cache_ranges[SEARCH_MAX_LINE_MATCHES];
	int cache_count;
} sr;

/*
 * Convert a Line of glyphs to a UTF-8 string.
 * Builds col_to_byte[] mapping: col_to_byte[i] = byte offset where column i starts.
 * col_to_byte[cols] = total byte length (sentinel).
 * Returns total byte length.
 */
static int
line_to_text(Line line, int cols, char *buf, int bufsize, int *col_to_byte)
{
	int len = 0;
	int i;

	for (i = 0; i < cols && len < bufsize - 4; i++) {
		col_to_byte[i] = len;
		if (line[i].u == 0 || line[i].u == ' ') {
			buf[len++] = ' ';
		} else {
			len += utf8encode(line[i].u, buf + len);
		}
	}
	/* Fill remaining columns if we stopped early */
	for (; i <= cols; i++)
		col_to_byte[i] = len;
	buf[len] = '\0';
	return len;
}

/*
 * Get the content length of a line (strip trailing spaces/nulls).
 * Works on a Line pointer directly (no TLINE dependency).
 */
static int
linelen(Line line, int cols)
{
	int i = cols;

	if (line[i - 1].mode & ATTR_WRAP)
		return i;
	while (i > 0 && (line[i - 1].u == ' ' || line[i - 1].u == 0))
		i--;
	return i;
}

/*
 * Convert byte offset to column index using col_to_byte mapping.
 */
static int
byte_to_col(int byte_off, int *col_to_byte, int cols)
{
	int i;

	for (i = 0; i < cols; i++) {
		if (col_to_byte[i + 1] > byte_off)
			return i;
	}
	return cols;
}

/*
 * Compute regex matches for a screen line and cache results.
 * Returns number of matches found.
 */
static int
compute_line_matches(int screen_y)
{
	char buf[SEARCH_MAX_LINE_BYTES];
	int col_to_byte[4097]; /* max cols + 1 */
	Line line;
	int len, cols;
	regmatch_t match;
	int offset;

	sr.cache_y = screen_y;
	sr.cache_count = 0;

	cols = term.col;
	if (cols > 4096)
		cols = 4096;

	line = TLINE_S(screen_y);
	len = line_to_text(line, cols, buf, sizeof(buf), col_to_byte);
	if (len == 0)
		return 0;

	/* Find all non-overlapping matches */
	offset = 0;
	while (offset < len && sr.cache_count < SEARCH_MAX_LINE_MATCHES) {
		if (regexec(&sr.regex, buf + offset, 1, &match, offset > 0 ? REG_NOTBOL : 0) != 0)
			break;
		if (match.rm_so == match.rm_eo) {
			/* Zero-length match — advance past it */
			offset += match.rm_eo + 1;
			continue;
		}
		sr.cache_ranges[sr.cache_count].start =
			byte_to_col(offset + match.rm_so, col_to_byte, cols);
		sr.cache_ranges[sr.cache_count].end =
			byte_to_col(offset + match.rm_eo - 1, col_to_byte, cols) + 1;
		sr.cache_count++;
		offset += match.rm_eo;
	}

	return sr.cache_count;
}

int
search_matched(int x, int y)
{
	int i;

	if (!sr.active)
		return 0;

	if (y != sr.cache_y)
		compute_line_matches(y);

	for (i = 0; i < sr.cache_count; i++) {
		if (x >= sr.cache_ranges[i].start && x < sr.cache_ranges[i].end)
			return 1;
	}
	return 0;
}

void
search_invalidate_cache(void)
{
	sr.cache_y = -1;
}

int
search_active(void)
{
	return sr.active;
}

void
search_clear(void)
{
	if (sr.compiled) {
		regfree(&sr.regex);
		sr.compiled = 0;
	}
	sr.active = 0;
	sr.pattern[0] = '\0';
	sr.cache_y = -1;
	sr.cache_count = 0;
	tfulldirt();
}

/*
 * Scroll so that absline is centered on screen.
 * absline = term.histn - term.scr + screen_y
 * (0 = oldest history, histn+row-1 = screen bottom)
 */
static void
scroll_to_absline(int absline)
{
	int new_scr;

	new_scr = term.row / 2 - absline + term.histn;
	if (new_scr < 0)
		new_scr = 0;
	if (new_scr > term.histn)
		new_scr = term.histn;
	term.scr = new_scr;
	tfulldirt();
}

/*
 * Convert (screen_y at current term.scr) to absline.
 */
static int
screen_to_abs(int screen_y)
{
	return term.histn - term.scr + screen_y;
}

/*
 * Get the Line pointer for an absline.
 */
static Line
absline_to_line(int absline)
{
	if (absline >= term.histn) {
		/* Screen line */
		return term.line[absline - term.histn];
	} else {
		/* History line */
		int hi = (term.histi - term.histn + 1 + absline + HISTSIZE) % HISTSIZE;
		return term.hist[hi];
	}
}

/*
 * Find first regex match on a line starting at or after column start_col.
 * Returns 1 if found, sets *match_col to the start column.
 * If start_col < 0, searches entire line.
 */
static int
find_match_on_line(int absline, int start_col, int *match_col)
{
	char buf[SEARCH_MAX_LINE_BYTES];
	int col_to_byte[4097];
	Line line;
	int len, cols;
	regmatch_t match;
	int offset, found_col;

	cols = term.col;
	if (cols > 4096)
		cols = 4096;

	line = absline_to_line(absline);
	len = line_to_text(line, cols, buf, sizeof(buf), col_to_byte);
	if (len == 0)
		return 0;

	/* Search from start_col byte offset */
	offset = 0;
	if (start_col > 0 && start_col < cols)
		offset = col_to_byte[start_col];

	while (offset < len) {
		if (regexec(&sr.regex, buf + offset, 1, &match,
					offset > 0 ? REG_NOTBOL : 0) != 0)
			return 0;
		if (match.rm_so == match.rm_eo) {
			offset += match.rm_eo + 1;
			continue;
		}
		found_col = byte_to_col(offset + match.rm_so, col_to_byte, cols);
		if (start_col < 0 || found_col >= start_col) {
			*match_col = found_col;
			return 1;
		}
		offset += match.rm_eo;
	}
	return 0;
}

/*
 * Find last regex match on a line ending at or before column end_col.
 * Returns 1 if found, sets *match_col to the start column.
 * If end_col < 0, searches entire line and returns last match.
 */
static int
find_match_on_line_reverse(int absline, int end_col, int *match_col)
{
	char buf[SEARCH_MAX_LINE_BYTES];
	int col_to_byte[4097];
	Line line;
	int len, cols;
	regmatch_t match;
	int offset, last_col;
	int found = 0;

	cols = term.col;
	if (cols > 4096)
		cols = 4096;

	line = absline_to_line(absline);
	len = line_to_text(line, cols, buf, sizeof(buf), col_to_byte);
	if (len == 0)
		return 0;

	/* Find all matches, keep the last one at or before end_col */
	offset = 0;
	last_col = -1;
	while (offset < len) {
		if (regexec(&sr.regex, buf + offset, 1, &match,
					offset > 0 ? REG_NOTBOL : 0) != 0)
			break;
		if (match.rm_so == match.rm_eo) {
			offset += match.rm_eo + 1;
			continue;
		}
		int col = byte_to_col(offset + match.rm_so, col_to_byte, cols);
		if (end_col < 0 || col < end_col) {
			last_col = col;
			found = 1;
		}
		offset += match.rm_eo;
	}
	if (found)
		*match_col = last_col;
	return found;
}

/*
 * Scan forward for next match from (cursor_abs, cursor_x).
 * Wraps around. Returns 1 if found, sets *result_abs and *result_x.
 */
static int
scan_forward(int cursor_abs, int cursor_x, int *result_abs, int *result_x)
{
	int total = term.histn + term.row;
	int abs, col;
	int i;

	/* First: current line, after cursor */
	if (find_match_on_line(cursor_abs, cursor_x + 1, &col)) {
		*result_abs = cursor_abs;
		*result_x = col;
		return 1;
	}

	/* Then: subsequent lines, wrapping */
	for (i = 1; i < total; i++) {
		abs = (cursor_abs + i) % total;
		/* Skip invalid history lines */
		if (abs < term.histn && abs >= term.histn)
			continue; /* can't happen, but safety */
		if (find_match_on_line(abs, -1, &col)) {
			*result_abs = abs;
			*result_x = col;
			return 1;
		}
	}
	return 0;
}

/*
 * Scan backward for next match from (cursor_abs, cursor_x).
 * Wraps around. Returns 1 if found, sets *result_abs and *result_x.
 */
static int
scan_backward(int cursor_abs, int cursor_x, int *result_abs, int *result_x)
{
	int total = term.histn + term.row;
	int abs, col;
	int i;

	/* First: current line, before cursor */
	if (find_match_on_line_reverse(cursor_abs, cursor_x, &col)) {
		*result_abs = cursor_abs;
		*result_x = col;
		return 1;
	}

	/* Then: previous lines, wrapping */
	for (i = 1; i < total; i++) {
		abs = (cursor_abs - i + total) % total;
		if (find_match_on_line_reverse(abs, -1, &col)) {
			*result_abs = abs;
			*result_x = col;
			return 1;
		}
	}
	return 0;
}

/*
 * Move vimnav cursor and scroll to center a match at (absline, col).
 */
static void
goto_match(int absline, int col)
{
	int screen_y;

	scroll_to_absline(absline);
	screen_y = absline - term.histn + term.scr;

	/* Clamp to screen bounds */
	if (screen_y < 0)
		screen_y = 0;
	if (screen_y >= term.row)
		screen_y = term.row - 1;

	vimnav.ox = vimnav.x;
	vimnav.oy = vimnav.y;
	vimnav.x = col;
	vimnav.y = screen_y;
	vimnav.savedx = col;
}

void
search_execute(const char *pattern, int direction)
{
	int cursor_abs, result_abs, result_x;
	int ret;

	/* Clear previous search */
	if (sr.compiled) {
		regfree(&sr.regex);
		sr.compiled = 0;
	}

	if (pattern[0] == '\0')
		return;

	/* Compile regex */
	ret = regcomp(&sr.regex, pattern, REG_EXTENDED);
	if (ret != 0) {
		/* Caller handles error display */
		return;
	}
	sr.compiled = 1;

	strncpy(sr.pattern, pattern, sizeof(sr.pattern) - 1);
	sr.pattern[sizeof(sr.pattern) - 1] = '\0';
	sr.direction = direction;
	sr.active = 1;
	sr.cache_y = -1;

	if (debug_mode)
		fprintf(stderr, "search: execute '%s' dir=%d\n", pattern, direction);

	/* Find first match from vimnav cursor position */
	cursor_abs = screen_to_abs(vimnav.y);

	if (direction > 0) {
		if (scan_forward(cursor_abs, vimnav.x, &result_abs, &result_x))
			goto_match(result_abs, result_x);
	} else {
		if (scan_backward(cursor_abs, vimnav.x, &result_abs, &result_x))
			goto_match(result_abs, result_x);
	}

	tfulldirt();
}

void
search_next(int direction)
{
	int effective_dir;
	int cursor_abs, result_abs, result_x;

	if (!sr.active || !sr.compiled)
		return;

	effective_dir = sr.direction * direction;
	cursor_abs = screen_to_abs(vimnav.y);

	if (effective_dir > 0) {
		if (scan_forward(cursor_abs, vimnav.x, &result_abs, &result_x))
			goto_match(result_abs, result_x);
	} else {
		if (scan_backward(cursor_abs, vimnav.x, &result_abs, &result_x))
			goto_match(result_abs, result_x);
	}

	tfulldirt();
}
