/* See LICENSE for license details. */
/* Mock implementations for st unit tests */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "mocks.h"
#include "../vimnav.h"

#define TLINE(y)	((y) < term.scr ? term.hist[((y) + term.histi - \
			term.scr + HISTSIZE + 1) % HISTSIZE] : \
			term.line[(y) - term.scr])

/* Global mock state */
Term term;
Selection sel;
wchar_t *worddelimiters = L" \t";
MockState mock_state;

void
mock_reset(void)
{
	memset(&mock_state, 0, sizeof(mock_state));
	memset(&sel, 0, sizeof(sel));
	/* Note: don't reset vimnav here - only in mock_term_init */
}

void
mock_term_init(int rows, int cols)
{
	int i;

	memset(&term, 0, sizeof(term));
	memset(&vimnav, 0, sizeof(vimnav));
	term.row = rows;
	term.col = cols;
	term.maxcol = cols;
	term.top = 0;
	term.bot = rows - 1;

	/* Allocate lines */
	term.line = malloc(rows * sizeof(Line));
	term.dirty = malloc(rows * sizeof(int));

	for (i = 0; i < rows; i++) {
		term.line[i] = calloc(cols, sizeof(Glyph));
		term.dirty[i] = 1;
	}

	/* Allocate history buffer */
	for (i = 0; i < HISTSIZE; i++) {
		term.hist[i] = calloc(cols, sizeof(Glyph));
	}
	term.histi = 0;
	term.scr = 0;

	/* Initialize cursor at origin */
	term.c.x = 0;
	term.c.y = 0;

	mock_reset();
}

void
mock_term_free(void)
{
	int i;

	if (term.line) {
		for (i = 0; i < term.row; i++) {
			free(term.line[i]);
		}
		free(term.line);
		term.line = NULL;
	}

	if (term.dirty) {
		free(term.dirty);
		term.dirty = NULL;
	}

	/* Free history buffer */
	for (i = 0; i < HISTSIZE; i++) {
		if (term.hist[i]) {
			free(term.hist[i]);
			term.hist[i] = NULL;
		}
	}
}

void
mock_set_line(int y, const char *content)
{
	int i;
	int len;

	if (y < 0 || y >= term.row)
		return;

	len = strlen(content);
	if (len > term.col)
		len = term.col;

	/* Clear line first */
	memset(term.line[y], 0, term.col * sizeof(Glyph));

	/* Set content */
	for (i = 0; i < len; i++) {
		term.line[y][i].u = content[i];
	}
}

void
mock_set_hist(int idx, const char *content)
{
	int i;
	int len;

	idx = (idx + HISTSIZE) % HISTSIZE;
	if (!term.hist[idx])
		return;

	len = strlen(content);
	if (len > term.col)
		len = term.col;

	/* Clear line first */
	memset(term.hist[idx], 0, term.col * sizeof(Glyph));

	/* Set content */
	for (i = 0; i < len; i++) {
		term.hist[idx][i].u = content[i];
	}
}

/* Stub implementations of st.c functions */

int
tlinelen(int y)
{
	int i;
	Line line;

	if (y < 0 || y >= term.row)
		return 0;

	line = TLINE(y);
	i = term.col;

	/* Strip trailing nulls (mock-specific: unwritten cells are 0) */
	while (i > 0 && line[i - 1].u == 0)
		--i;

	/* Strip trailing spaces (matches real st behavior) */
	while (i > 0 && line[i - 1].u == ' ')
		--i;

	return i;
}

void
tfulldirt(void)
{
	int i;

	mock_state.tfulldirt_calls++;

	for (i = 0; i < term.row; i++)
		term.dirty[i] = 1;
}

void
selclear(void)
{
	mock_state.selclear_calls++;
	sel.mode = SEL_IDLE;
}

void
selinit(void)
{
	sel.mode = SEL_IDLE;
	sel.snap = 0;
	sel.ob.x = -1;
}

void
selstart(int x, int y, int snap)
{
	mock_state.selstart_calls++;
	mock_state.last_selstart.x = x;
	mock_state.last_selstart.y = y;
	mock_state.last_selstart.snap = snap;

	sel.mode = SEL_EMPTY;
	sel.type = SEL_REGULAR;
	sel.snap = snap;
	sel.oe.x = sel.ob.x = x;
	sel.oe.y = sel.ob.y = y;
}

void
selextend(int x, int y, int type, int done)
{
	mock_state.selextend_calls++;
	mock_state.last_selextend.x = x;
	mock_state.last_selextend.y = y;
	mock_state.last_selextend.type = type;
	mock_state.last_selextend.done = done;

	sel.oe.x = x;
	sel.oe.y = y;
	sel.type = type;

	if (done)
		sel.mode = SEL_READY;
	else
		sel.mode = SEL_EMPTY;
}

int
selected(int x, int y)
{
	int linelen, maxcol;

	/* Simplified selection check */
	if (sel.mode == SEL_IDLE)
		return 0;

	/* Match the real selected() logic for empty lines in vimnav mode:
	 * Allow at least column 0 to be selected (like nvim's virtual newline) */
	linelen = tlinelen(y);
	maxcol = tisvimnav() ? (linelen > 0 ? linelen : 1) : linelen + 1;
	if (x >= maxcol)
		return 0;

	return (y >= sel.ob.y && y <= sel.oe.y &&
	        (y != sel.ob.y || x >= sel.ob.x) &&
	        (y != sel.oe.y || x <= sel.oe.x));
}

char *
getsel(void)
{
	/* Return a simple string for testing */
	static char buf[256];
	int y, x, len;

	if (sel.mode == SEL_IDLE)
		return NULL;

	len = 0;
	for (y = sel.ob.y; y <= sel.oe.y && len < 255; y++) {
		int startx = (y == sel.ob.y) ? sel.ob.x : 0;
		int endx = (y == sel.oe.y) ? sel.oe.x : tlinelen(y) - 1;

		for (x = startx; x <= endx && len < 255; x++) {
			if (term.line[y][x].u)
				buf[len++] = term.line[y][x].u;
		}
		if (y < sel.oe.y && len < 255)
			buf[len++] = '\n';
	}
	buf[len] = '\0';

	return buf;
}

void
kscrolldown(const Arg *a)
{
	int n;

	mock_state.kscrolldown_calls++;
	n = a->i;
	mock_state.last_kscrolldown.n = n;

	if (n > term.scr)
		n = term.scr;
	term.scr -= n;
	tfulldirt();
}

void
kscrollup(const Arg *a)
{
	int n;

	mock_state.kscrollup_calls++;
	n = a->i;
	mock_state.last_kscrollup.n = n;

	term.scr += n;
	if (term.scr > HISTSIZE - 1)
		term.scr = HISTSIZE - 1;
	tfulldirt();
}

/* X11 stubs */
void
xsetsel(char *str)
{
	mock_state.xsetsel_calls++;
	mock_state.last_xsetsel = str;
}

void
xclipcopy(void)
{
	mock_state.xclipcopy_calls++;
}

void
clippaste(const Arg *a)
{
	(void)a;
	mock_state.clippaste_calls++;
}

/* Memory allocation wrappers */
void *
xmalloc(size_t len)
{
	void *p = malloc(len);
	if (!p) {
		fprintf(stderr, "malloc: out of memory\n");
		exit(1);
	}
	return p;
}

void *
xrealloc(void *p, size_t len)
{
	p = realloc(p, len);
	if (!p) {
		fprintf(stderr, "realloc: out of memory\n");
		exit(1);
	}
	return p;
}

char *
xstrdup(const char *s)
{
	char *p = strdup(s);
	if (!p) {
		fprintf(stderr, "strdup: out of memory\n");
		exit(1);
	}
	return p;
}

void
die(const char *fmt, ...)
{
	(void)fmt;
	exit(1);
}

void
redraw(void)
{
}

void
draw(void)
{
}

void
ttywrite(const char *s, size_t n, int may_echo)
{
	(void)may_echo;
	mock_state.ttywrite_calls++;

	/* Append to buffer for inspection */
	for (size_t i = 0; i < n && mock_state.ttywrite_buf_len < 255; i++) {
		mock_state.ttywrite_buf[mock_state.ttywrite_buf_len++] = s[i];
	}
	mock_state.ttywrite_buf[mock_state.ttywrite_buf_len] = '\0';
}

void
tscrollup(int orig, int n, int copyhist)
{
	int i;
	Line temp;

	mock_state.tscrollup_calls++;

	if (n > term.bot - orig + 1)
		n = term.bot - orig + 1;
	if (n < 0)
		n = 0;

	if (copyhist && term.hist[0]) {
		term.histi = (term.histi + 1) % HISTSIZE;
		temp = term.hist[term.histi];
		term.hist[term.histi] = term.line[orig];
		term.line[orig] = temp;
	}

	/* Clear the region */
	for (i = 0; i < term.col && term.line[orig]; i++) {
		term.line[orig][i].u = ' ';
		term.line[orig][i].mode = 0;
	}

	/* Scroll lines up */
	for (i = orig; i <= term.bot - n && term.line[i]; i++) {
		temp = term.line[i];
		term.line[i] = term.line[i + n];
		term.line[i + n] = temp;
	}
}

void
tclearregion(int x1, int y1, int x2, int y2)
{
	int x, y;

	mock_state.tclearregion_calls++;

	for (y = y1; y <= y2 && y < term.row; y++) {
		for (x = x1; x <= x2 && x < term.col; x++) {
			if (term.line[y]) {
				term.line[y][x].u = ' ';
				term.line[y][x].mode = 0;
			}
		}
	}
}

void
selscroll(int orig, int n)
{
	(void)orig;
	(void)n;
	/* Stub for selection scroll */
}
