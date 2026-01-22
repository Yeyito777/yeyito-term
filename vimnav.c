/* See LICENSE for license details. */
#include <wchar.h>
#include <wctype.h>

#include "st.h"
#include "vimnav.h"

/* X11 modifier masks (from X11/X.h) */
#define ShiftMask   (1<<0)
#define ControlMask (1<<2)

/* Access to st.c internals */
#define HISTSIZE      (1 << 15)
#define IS_SET(flag)  ((term.mode & (flag)) != 0)
#define ISDELIM(u)    (u && wcschr(worddelimiters, u))
#define TLINE(y)      ((y) < term.scr ? term.hist[((y) + term.histi - \
                      term.scr + HISTSIZE + 1) % HISTSIZE] : \
                      term.line[(y) - term.scr])

/* Vim navigation mode states (internal use) */
enum vimnav_mode {
	VIMNAV_INACTIVE = 0,
	VIMNAV_NORMAL = 1,
	VIMNAV_VISUAL = 2,
	VIMNAV_VISUAL_LINE = 3,
};

/* Terminal mode flags from st.c */
enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_INSERT      = 1 << 1,
	MODE_ALTSCREEN   = 1 << 2,
	MODE_CRLF        = 1 << 3,
	MODE_ECHO        = 1 << 4,
	MODE_PRINT       = 1 << 5,
	MODE_UTF8        = 1 << 6,
};

/* Cursor structure */
typedef struct {
	Glyph attr;
	int x;
	int y;
	char state;
} TCursor;

/* Selection structure */
typedef struct {
	int mode;
	int type;
	int snap;
	struct {
		int x, y;
	} nb, ne, ob, oe;
	int alt;
} Selection;

/* Terminal structure */
typedef struct {
	int row;
	int col;
	int maxcol;
	Line *line;
	Line *alt;
	Line hist[HISTSIZE];
	int histi;
	int scr;
	int *dirty;
	TCursor c;
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
} Term;

/* Extern declarations for st.c globals */
extern Term term;
extern Selection sel;
extern wchar_t *worddelimiters;

/* Extern declarations for functions in st.c */
extern int tlinelen(int y);
extern void tfulldirt(void);

/* Extern declarations for functions in x.c */
extern void xsetsel(char *str);
extern void xclipcopy(void);
extern void clippaste(const Arg *);

/* Globals - exported via vimnav.h */
VimNav vimnav = { .mode = VIMNAV_INACTIVE };
static int vimnav_paste_strip_newlines = 0;

/* Forward declarations */
static int vimnav_find_prompt_end(int screen_y);
static int vimnav_find_prompt_start_y(void);
static int vimnav_is_prompt_space(int y);
static void vimnav_update_selection(void);
static void vimnav_notify_zsh_visual_end(void);
static void vimnav_sync_to_zsh_cursor(void);

/* Notify zsh that visual mode selection has ended (send Escape) */
static void
vimnav_notify_zsh_visual_end(void)
{
	if (vimnav.zsh_visual) {
		/* Send Escape to zsh to exit visual mode */
		ttywrite("\033", 1, 1);
		vimnav.zsh_visual = 0;
	}
}

/* Sync st cursor to zsh cursor position (call when returning to prompt space) */
static void
vimnav_sync_to_zsh_cursor(void)
{
	if (vimnav_is_prompt_space(vimnav.y)) {
		int prompt_end = vimnav_find_prompt_end(vimnav.y);
		vimnav.x = prompt_end + vimnav.zsh_cursor;
		vimnav.savedx = vimnav.x;
		vimnav.last_shell_x = vimnav.x;
	}
}

/* Snap back to prompt line (scroll down if needed, update cursor position) */
static void
vimnav_snap_to_prompt(void)
{
	if (term.scr > 0) {
		kscrolldown(&(Arg){ .i = term.scr });
	}
	vimnav.y = term.c.y;
	vimnav_sync_to_zsh_cursor();
}

/* zsh cursor/visual sync functions */
void
vimnav_set_zsh_cursor(int pos)
{
	int prompt_end;

	vimnav.zsh_cursor = pos;

	/* If in nav mode on prompt line, update our cursor to match zsh */
	if (vimnav.mode != VIMNAV_INACTIVE && term.scr == 0 && vimnav.y == term.c.y) {
		prompt_end = vimnav_find_prompt_end(vimnav.y);
		vimnav.x = prompt_end + pos;
		vimnav.savedx = vimnav.x;
		vimnav.last_shell_x = vimnav.x;
		if (vimnav.mode == VIMNAV_VISUAL || vimnav.mode == VIMNAV_VISUAL_LINE) {
			vimnav_update_selection();
		}
		tfulldirt();
	}

	/* If zsh is in visual mode on prompt line, update st's selection rendering */
	if (vimnav.zsh_visual && vimnav.mode != VIMNAV_INACTIVE &&
	    term.scr == 0 && vimnav.y == term.c.y && vimnav.mode == VIMNAV_NORMAL) {
		prompt_end = vimnav_find_prompt_end(term.c.y);
		int anchor_x = prompt_end + vimnav.zsh_visual_anchor;
		int cursor_x = prompt_end + pos;
		if (vimnav.zsh_visual_line) {
			selstart(0, term.c.y, 0);
			sel.snap = SNAP_LINE;
			selextend(term.col - 1, term.c.y, SEL_REGULAR, 0);
		} else {
			selstart(anchor_x, term.c.y, 0);
			selextend(cursor_x, term.c.y, SEL_REGULAR, 0);
		}
		tfulldirt();
	}
}

void
vimnav_set_zsh_visual(int active, int anchor, int line_mode)
{
	int prompt_end;

	vimnav.zsh_visual = active;
	vimnav.zsh_visual_anchor = anchor;
	vimnav.zsh_visual_line = line_mode;

	if (active && vimnav.mode != VIMNAV_INACTIVE && term.scr == 0) {
		/* zsh entered visual mode on prompt line - st renders the selection */
		prompt_end = vimnav_find_prompt_end(term.c.y);
		int anchor_x = prompt_end + anchor;
		int cursor_x = prompt_end + vimnav.zsh_cursor;
		if (line_mode) {
			selstart(0, term.c.y, 0);
			sel.snap = SNAP_LINE;
			selextend(term.col - 1, term.c.y, SEL_REGULAR, 0);
		} else {
			selstart(anchor_x, term.c.y, 0);
			selextend(cursor_x, term.c.y, SEL_REGULAR, 0);
		}
		tfulldirt();
	} else if (!active && vimnav.mode != VIMNAV_INACTIVE) {
		/* zsh exited visual mode - clear selection */
		if (vimnav.mode == VIMNAV_VISUAL || vimnav.mode == VIMNAV_VISUAL_LINE) {
			vimnav.mode = VIMNAV_NORMAL;
		}
		selclear();
		tfulldirt();
	}
}

int
tisvimnav(void)
{
	return vimnav.mode != VIMNAV_INACTIVE;
}

static int
vimnav_screen_y(void)
{
	return vimnav.y;
}

static void
vimnav_update_selection(void)
{
	int screen_y = vimnav_screen_y();
	int anchor_screen_y = vimnav.anchor_abs_y + term.scr;

	if (vimnav.mode == VIMNAV_VISUAL) {
		selstart(vimnav.anchor_x, anchor_screen_y, 0);
		selextend(vimnav.x, screen_y, SEL_REGULAR, 0);
	} else if (vimnav.mode == VIMNAV_VISUAL_LINE) {
		selstart(0, anchor_screen_y, 0);
		sel.snap = SNAP_LINE;
		selextend(term.col - 1, screen_y, SEL_REGULAR, 0);
	}
	tfulldirt();
}

static void
vimnav_move_left(void)
{
	if (vimnav.x > 0) {
		vimnav.x--;
		vimnav.savedx = vimnav.x;
	}
	vimnav_update_selection();
}

static void
vimnav_move_right(void)
{
	int linelen = tlinelen(vimnav_screen_y());
	if (vimnav.x < linelen - 1 && vimnav.x < term.col - 1) {
		vimnav.x++;
		vimnav.savedx = vimnav.x;
	}
	vimnav_update_selection();
}

/* Check if there's any content in history above current scroll position.
 * This looks ahead to see if scrolling up would eventually show content,
 * even if the immediate next lines are empty. */
static int
vimnav_has_history_content(int scroll_offset)
{
	int i, idx;
	/* Check up to 10 lines ahead in history for any content */
	for (i = 0; i < 10 && scroll_offset + i < HISTSIZE; i++) {
		/* Calculate the history index for this scroll position.
		 * Must match TLINE macro: hist[(y + histi - scr + HISTSIZE + 1) % HISTSIZE]
		 * For y=0 (top line) at scroll_offset, looking i lines further back: */
		idx = (term.histi - scroll_offset - i + HISTSIZE + 1) % HISTSIZE;
		if (term.hist[idx]) {
			/* Check if this history line has content */
			int j;
			for (j = term.col - 1; j >= 0; j--) {
				if (term.hist[idx][j].u != ' ' && term.hist[idx][j].u != 0) {
					return 1;  /* Found content */
				}
			}
		}
	}
	return 0;  /* No content found in lookahead */
}

/* Scroll helper that respects vim nav boundaries and moves cursor */
static void
vimnav_scroll_up(int n)
{
	int old_scr = term.scr;
	int scrolled, remaining;
	int i;
	int linelen;

	/* Scroll up incrementally, checking for content in history */
	for (i = 0; i < n && term.scr < HISTSIZE - 1; i++) {
		term.scr++;
		/* Stop if there's no content anywhere in the history we'd scroll into */
		if (!vimnav_has_history_content(term.scr)) {
			term.scr--;
			break;
		}
	}
	scrolled = term.scr - old_scr;
	if (scrolled > 0) {
		tfulldirt();
	}

	/* Move cursor up by the amount we couldn't scroll */
	remaining = n - scrolled;
	if (remaining > 0 && vimnav.y > 0) {
		vimnav.y -= remaining;
		if (vimnav.y < 0)
			vimnav.y = 0;
	}

	/* Clamp x to line length */
	linelen = tlinelen(vimnav.y);
	vimnav.x = MIN(vimnav.savedx, linelen > 0 ? linelen - 1 : 0);

	/* Update selection if in visual mode */
	vimnav_update_selection();
}

static void
vimnav_scroll_down(int n)
{
	int scrolled;
	int max_scroll;
	int requested = n;  /* Save original request before capping */
	int linelen;

	/* Don't scroll past where the prompt currently is */
	max_scroll = term.scr;  /* Can scroll down at most term.scr lines */
	if (n > max_scroll)
		n = max_scroll;

	scrolled = n;
	if (scrolled > 0) {
		kscrolldown(&(Arg){ .i = scrolled });
	}

	/* Move cursor down by the amount we couldn't scroll */
	int remaining = requested - scrolled;
	if (remaining > 0) {
		/* Can move cursor down to prompt position (term.scr + term.c.y) */
		int max_valid_y = term.scr + term.c.y;
		if (vimnav.y < max_valid_y) {
			vimnav.y += remaining;
			if (vimnav.y > max_valid_y)
				vimnav.y = max_valid_y;
		}
	}

	/* Clamp cursor to prompt if we ended up past it */
	int max_valid_y = term.scr + term.c.y;
	if (vimnav.y > max_valid_y) {
		vimnav.y = max_valid_y;
		if (term.scr == 0) {
			vimnav.x = term.c.x;
			vimnav.last_shell_x = term.c.x;
		}
	}

	/* Clamp x to line length */
	linelen = tlinelen(vimnav.y);
	vimnav.x = MIN(vimnav.savedx, linelen > 0 ? linelen - 1 : 0);

	/* Update selection if in visual mode */
	vimnav_update_selection();
}

static void
vimnav_move_up(void)
{
	int linelen;
	int old_scr = term.scr;
	int was_in_prompt_space = vimnav_is_prompt_space(vimnav.y);

	if (vimnav.y > 0) {
		/* Move cursor up within visible area */
		vimnav.y--;
	} else {
		/* At top of screen, try to scroll up */
		term.scr++;
		if (term.scr > HISTSIZE - 1 || !vimnav_has_history_content(term.scr)) {
			/* Can't scroll or no content in history, revert */
			term.scr = old_scr;
		} else {
			tfulldirt();
		}
		/* Cursor stays at row 0 */
	}

	/* Handoff: if we left prompt space with zsh in visual mode, inherit selection */
	if (was_in_prompt_space && !vimnav_is_prompt_space(vimnav.y) &&
	    vimnav.zsh_visual && vimnav.mode == VIMNAV_NORMAL) {
		int prompt_end = vimnav_find_prompt_end(term.c.y);
		vimnav.anchor_x = prompt_end + vimnav.zsh_visual_anchor;
		vimnav.anchor_abs_y = term.c.y - term.scr;  /* Anchor stays on prompt line */
		if (vimnav.zsh_visual_line) {
			vimnav.mode = VIMNAV_VISUAL_LINE;
		} else {
			vimnav.mode = VIMNAV_VISUAL;
		}
	}

	linelen = tlinelen(vimnav.y);
	vimnav.x = MIN(vimnav.savedx, linelen > 0 ? linelen - 1 : 0);
	vimnav_update_selection();
}

static void
vimnav_move_down(void)
{
	int linelen;
	int was_in_prompt_space = vimnav_is_prompt_space(vimnav.y);
	/* The prompt is at screen row (term.scr + term.c.y) in scrolled view.
	 * Valid content is rows [0, term.scr + term.c.y]. Everything below is dead space. */
	int max_valid_y = term.scr + term.c.y;

	if (vimnav.y >= max_valid_y) {
		/* At or past prompt position, only allow scrolling down if scrolled */
		if (term.scr > 0) {
			kscrolldown(&(Arg){ .i = 1 });
			/* Recalculate after scroll and clamp if needed */
			max_valid_y = term.scr + term.c.y;
			if (vimnav.y > max_valid_y) {
				vimnav.y = max_valid_y;
				if (term.scr == 0) {
					vimnav.x = term.c.x;
					vimnav.last_shell_x = term.c.x;
				}
			}
		}
		/* If term.scr == 0, can't move down at all */
	} else if (vimnav.y >= term.row - 1) {
		/* At bottom of screen but not at prompt yet, scroll down */
		kscrolldown(&(Arg){ .i = 1 });
	} else {
		/* Can move down freely within valid range */
		vimnav.y++;
	}

	/* If entering prompt space from history in normal mode, sync to zsh cursor.
	 * Don't sync during visual mode - wait until selection is done. */
	if (!was_in_prompt_space && vimnav_is_prompt_space(vimnav.y) &&
	    vimnav.mode == VIMNAV_NORMAL) {
		int prompt_end = vimnav_find_prompt_end(vimnav.y);
		vimnav.x = prompt_end + vimnav.zsh_cursor;
		vimnav.savedx = vimnav.x;
		vimnav.last_shell_x = vimnav.x;
	} else {
		linelen = tlinelen(vimnav.y);
		vimnav.x = MIN(vimnav.savedx, linelen > 0 ? linelen - 1 : 0);
	}
	vimnav_update_selection();
}

static void
vimnav_move_bol(void)
{
	vimnav.x = 0;
	vimnav.savedx = 0;
	vimnav_update_selection();
}

static void
vimnav_move_eol(void)
{
	int linelen = tlinelen(vimnav_screen_y());
	vimnav.x = linelen > 0 ? linelen - 1 : 0;
	vimnav.savedx = vimnav.x;
	vimnav_update_selection();
}

static void
vimnav_move_word_forward(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	int linelen = tlinelen(screen_y);
	Glyph *gp;

	if (linelen == 0)
		return;

	/* Skip current word (non-delimiters) */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (ISDELIM(gp->u))
			break;
		x++;
	}

	/* Skip delimiters */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (!ISDELIM(gp->u))
			break;
		x++;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

static void
vimnav_move_word_backward(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	Glyph *gp;

	if (x == 0)
		return;

	x--;

	/* Skip delimiters */
	while (x > 0) {
		gp = &TLINE(screen_y)[x];
		if (!ISDELIM(gp->u))
			break;
		x--;
	}

	/* Skip to start of word */
	while (x > 0) {
		gp = &TLINE(screen_y)[x - 1];
		if (ISDELIM(gp->u))
			break;
		x--;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

static void
vimnav_move_word_end(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	int linelen = tlinelen(screen_y);
	Glyph *gp;

	if (linelen == 0 || x >= linelen - 1)
		return;

	/* Move forward at least one character */
	x++;

	/* Skip delimiters */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (!ISDELIM(gp->u))
			break;
		x++;
	}

	/* Move to end of word */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x + 1];
		if (ISDELIM(gp->u))
			break;
		x++;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

static void
vimnav_move_WORD_forward(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	int linelen = tlinelen(screen_y);
	Glyph *gp;

	if (linelen == 0)
		return;

	/* Skip current WORD (non-whitespace) */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (iswspace(gp->u))
			break;
		x++;
	}

	/* Skip whitespace */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (!iswspace(gp->u))
			break;
		x++;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

static void
vimnav_move_WORD_backward(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	Glyph *gp;

	if (x == 0)
		return;

	x--;

	/* Skip whitespace */
	while (x > 0) {
		gp = &TLINE(screen_y)[x];
		if (!iswspace(gp->u))
			break;
		x--;
	}

	/* Skip to start of WORD */
	while (x > 0) {
		gp = &TLINE(screen_y)[x - 1];
		if (iswspace(gp->u))
			break;
		x--;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

static void
vimnav_move_WORD_end(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	int linelen = tlinelen(screen_y);
	Glyph *gp;

	if (linelen == 0 || x >= linelen - 1)
		return;

	/* Move forward at least one character */
	x++;

	/* Skip whitespace */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (!iswspace(gp->u))
			break;
		x++;
	}

	/* Move to end of WORD */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x + 1];
		if (iswspace(gp->u))
			break;
		x++;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

/* Text object selection helpers */

/* Find word boundaries around cursor position (inner = exclude delimiters) */
static int
vimnav_find_word_bounds(int x, int y, int inner, int *start_x, int *end_x)
{
	Line line = TLINE(y);
	int linelen = tlinelen(y);
	int sx, ex;
	Glyph *gp;

	if (linelen == 0 || x >= linelen)
		return 0;

	gp = &line[x];

	/* If on a delimiter, select the delimiter run (or whitespace for 'around') */
	if (ISDELIM(gp->u)) {
		sx = x;
		ex = x;
		/* Expand left */
		while (sx > 0 && ISDELIM(line[sx - 1].u))
			sx--;
		/* Expand right */
		while (ex < linelen - 1 && ISDELIM(line[ex + 1].u))
			ex++;
		*start_x = sx;
		*end_x = ex;
		return 1;
	}

	/* Find word start */
	sx = x;
	while (sx > 0 && !ISDELIM(line[sx - 1].u))
		sx--;

	/* Find word end */
	ex = x;
	while (ex < linelen - 1 && !ISDELIM(line[ex + 1].u))
		ex++;

	if (!inner) {
		/* 'around' - include trailing whitespace, or leading if at end */
		if (ex < linelen - 1 && ISDELIM(line[ex + 1].u)) {
			while (ex < linelen - 1 && ISDELIM(line[ex + 1].u))
				ex++;
		} else if (sx > 0 && ISDELIM(line[sx - 1].u)) {
			while (sx > 0 && ISDELIM(line[sx - 1].u))
				sx--;
		}
	}

	*start_x = sx;
	*end_x = ex;
	return 1;
}

/* Find WORD boundaries (whitespace-delimited) */
static int
vimnav_find_WORD_bounds(int x, int y, int inner, int *start_x, int *end_x)
{
	Line line = TLINE(y);
	int linelen = tlinelen(y);
	int sx, ex;
	Glyph *gp;

	if (linelen == 0 || x >= linelen)
		return 0;

	gp = &line[x];

	/* If on whitespace, select whitespace run */
	if (iswspace(gp->u)) {
		sx = x;
		ex = x;
		while (sx > 0 && iswspace(line[sx - 1].u))
			sx--;
		while (ex < linelen - 1 && iswspace(line[ex + 1].u))
			ex++;
		*start_x = sx;
		*end_x = ex;
		return 1;
	}

	/* Find WORD start */
	sx = x;
	while (sx > 0 && !iswspace(line[sx - 1].u))
		sx--;

	/* Find WORD end */
	ex = x;
	while (ex < linelen - 1 && !iswspace(line[ex + 1].u))
		ex++;

	if (!inner) {
		/* 'around' - include trailing whitespace, or leading if at end */
		if (ex < linelen - 1 && iswspace(line[ex + 1].u)) {
			while (ex < linelen - 1 && iswspace(line[ex + 1].u))
				ex++;
		} else if (sx > 0 && iswspace(line[sx - 1].u)) {
			while (sx > 0 && iswspace(line[sx - 1].u))
				sx--;
		}
	}

	*start_x = sx;
	*end_x = ex;
	return 1;
}

/* Find matching pair boundaries (quotes, brackets, etc.)
 * If cursor is not inside an enclosing pair, search to the right for one. */
static int
vimnav_find_pair_bounds(int x, int y, Rune open, Rune close, int inner, int *start_x, int *end_x)
{
	Line line = TLINE(y);
	int linelen = tlinelen(y);
	int sx = -1, ex = -1;
	int depth = 0;
	int i;

	if (linelen == 0)
		return 0;

	/* For quotes (open == close), logic is different from brackets */
	if (open == close) {
		/* First, try to find if cursor is inside a quote pair.
		 * Count quotes to the left of cursor to determine if we're inside. */
		int quotes_left = 0;
		for (i = 0; i < x; i++) {
			if (line[i].u == open)
				quotes_left++;
		}

		/* If odd number of quotes to the left, we're inside a quoted region */
		if (quotes_left % 2 == 1) {
			/* Find opening quote (last quote before cursor) */
			for (i = x - 1; i >= 0; i--) {
				if (line[i].u == open) {
					sx = i;
					break;
				}
			}
			/* Find closing quote (first quote at or after cursor) */
			for (i = x; i < linelen; i++) {
				if (line[i].u == close) {
					ex = i;
					break;
				}
			}
			if (sx >= 0 && ex >= 0)
				goto found;
		}

		/* If cursor is on a quote, check if it starts a pair */
		if (line[x].u == open) {
			sx = x;
			for (i = x + 1; i < linelen; i++) {
				if (line[i].u == close) {
					ex = i;
					goto found;
				}
			}
		}

		/* Not inside quotes - search right for a quote pair */
		for (i = x + 1; i < linelen; i++) {
			if (line[i].u == open) {
				sx = i;
				/* Find closing quote */
				for (int j = i + 1; j < linelen; j++) {
					if (line[j].u == close) {
						ex = j;
						goto found;
					}
				}
				/* Opening quote found but no closing - continue searching */
				sx = -1;
			}
		}
		return 0;  /* No valid quote pair found */
	} else {
		/* For brackets: handle nesting */
		/* First, try to find opening bracket to the left (cursor inside pair) */
		depth = 0;
		for (i = x; i >= 0; i--) {
			if (line[i].u == close)
				depth++;
			else if (line[i].u == open) {
				if (depth == 0) {
					sx = i;
					break;
				}
				depth--;
			}
		}

		if (sx >= 0) {
			/* Found opening bracket to the left, find matching closer */
			depth = 0;
			for (i = sx; i < linelen; i++) {
				if (line[i].u == open)
					depth++;
				else if (line[i].u == close) {
					depth--;
					if (depth == 0) {
						ex = i;
						goto found;
					}
				}
			}
			/* Opening found but no valid closing - fall through to search right */
		}

		/* Not inside a pair - search right for an opening bracket */
		for (i = x + 1; i < linelen; i++) {
			if (line[i].u == open) {
				sx = i;
				/* Find matching closing bracket */
				depth = 1;
				for (int j = i + 1; j < linelen; j++) {
					if (line[j].u == open)
						depth++;
					else if (line[j].u == close) {
						depth--;
						if (depth == 0) {
							ex = j;
							goto found;
						}
					}
				}
				/* Opening found but no closing - continue searching */
				sx = -1;
			}
		}
		return 0;  /* No valid pair found */
	}

found:
	if (inner) {
		/* Exclude the delimiters */
		sx++;
		ex--;
		if (sx > ex)
			return 0;  /* Empty inside */
	}

	*start_x = sx;
	*end_x = ex;
	return 1;
}

/* Select text object and enter visual mode */
static void
vimnav_select_textobj(int start_x, int end_x)
{
	int screen_y = vimnav_screen_y();

	/* Enter visual mode with selection */
	vimnav.mode = VIMNAV_VISUAL;
	vimnav.anchor_x = start_x;
	vimnav.anchor_abs_y = screen_y - term.scr;
	vimnav.x = end_x;
	vimnav.savedx = end_x;

	selstart(start_x, screen_y, 0);
	selextend(end_x, screen_y, SEL_REGULAR, 0);
	tfulldirt();
}

/* Handle text object key (w, W, ", (, ), [, ], {, }) after i/a prefix */
static int
vimnav_handle_textobj(ulong ksym, int inner)
{
	int start_x, end_x;
	int y = vimnav_screen_y();
	int found = 0;

	switch (ksym) {
	case 'w':
		found = vimnav_find_word_bounds(vimnav.x, y, inner, &start_x, &end_x);
		break;
	case 'W':
		found = vimnav_find_WORD_bounds(vimnav.x, y, inner, &start_x, &end_x);
		break;
	case '"':
		found = vimnav_find_pair_bounds(vimnav.x, y, '"', '"', inner, &start_x, &end_x);
		break;
	case '\'':
		found = vimnav_find_pair_bounds(vimnav.x, y, '\'', '\'', inner, &start_x, &end_x);
		break;
	case '`':
		found = vimnav_find_pair_bounds(vimnav.x, y, '`', '`', inner, &start_x, &end_x);
		break;
	case '(':
	case ')':
	case 'b':
		found = vimnav_find_pair_bounds(vimnav.x, y, '(', ')', inner, &start_x, &end_x);
		break;
	case '[':
	case ']':
		found = vimnav_find_pair_bounds(vimnav.x, y, '[', ']', inner, &start_x, &end_x);
		break;
	case '{':
	case '}':
	case 'B':
		found = vimnav_find_pair_bounds(vimnav.x, y, '{', '}', inner, &start_x, &end_x);
		break;
	case '<':
	case '>':
		found = vimnav_find_pair_bounds(vimnav.x, y, '<', '>', inner, &start_x, &end_x);
		break;
	default:
		return 0;  /* Unknown text object */
	}

	if (found) {
		vimnav_select_textobj(start_x, end_x);
		return 1;
	}

	return 0;
}

static void
vimnav_move_top(void)
{
	int linelen;

	/* Scroll to top of history, stopping at blank lines */
	vimnav_scroll_up(HISTSIZE);
	vimnav.y = 0;

	linelen = tlinelen(vimnav.y);
	vimnav.x = MIN(vimnav.savedx, linelen > 0 ? linelen - 1 : 0);
	if (vimnav.x < 0)
		vimnav.x = 0;
	vimnav_update_selection();
}

static void
vimnav_move_bottom(void)
{
	int linelen;

	/* Scroll back to show current prompt */
	if (term.scr > 0) {
		kscrolldown(&(Arg){ .i = term.scr });
	}
	vimnav.y = term.c.y;  /* Use current shell cursor position */

	linelen = tlinelen(vimnav.y);
	vimnav.x = MIN(vimnav.savedx, linelen > 0 ? linelen - 1 : 0);
	if (vimnav.x < 0)
		vimnav.x = 0;
	vimnav_update_selection();
}

static void
vimnav_toggle_visual_char(void)
{
	if (vimnav.mode == VIMNAV_VISUAL) {
		vimnav.mode = VIMNAV_NORMAL;
		vimnav_notify_zsh_visual_end();
		selclear();
		vimnav_sync_to_zsh_cursor();
	} else {
		vimnav.mode = VIMNAV_VISUAL;
		vimnav.anchor_x = vimnav.x;
		vimnav.anchor_abs_y = vimnav_screen_y() - term.scr;
		selstart(vimnav.x, vimnav_screen_y(), 0);
	}
	tfulldirt();
}

static void
vimnav_toggle_visual_line(void)
{
	int screen_y = vimnav_screen_y();

	if (vimnav.mode == VIMNAV_VISUAL_LINE) {
		vimnav.mode = VIMNAV_NORMAL;
		vimnav_notify_zsh_visual_end();
		selclear();
		vimnav_sync_to_zsh_cursor();
	} else {
		vimnav.mode = VIMNAV_VISUAL_LINE;
		vimnav.anchor_abs_y = screen_y - term.scr;
		selstart(0, screen_y, 0);
		sel.snap = SNAP_LINE;
		selextend(term.col - 1, screen_y, SEL_REGULAR, 0);
	}
	tfulldirt();
}

int
tisvimnav_paste(void)
{
	return vimnav_paste_strip_newlines;
}

void
vimnav_paste_done(void)
{
	vimnav_paste_strip_newlines = 0;
}

static void
vimnav_yank_selection(void)
{
	char *text = getsel();
	if (text) {
		xsetsel(text);
		xclipcopy();
	}
}

static int
vimnav_find_prompt_end(int screen_y)
{
	/* Find the end of the prompt on this line by looking for common prompt
	 * delimiters: '% ', '$ ', '> ', '# ' (with trailing space).
	 * Returns the x position after the delimiter, or 0 if not found. */
	Line line = TLINE(screen_y);
	int linelen = tlinelen(screen_y);
	int last_delim = -1;

	for (int i = 0; i < linelen && i + 1 < term.col; i++) {
		Rune c = line[i].u;
		Rune next = line[i + 1].u;
		if ((c == '%' || c == '$' || c == '>' || c == '#') && next == ' ') {
			last_delim = i + 2;  /* Position after "% " */
		}
	}

	return last_delim > 0 ? last_delim : 0;
}

static int
vimnav_has_main_prompt(int screen_y)
{
	/* Check if this line has a main prompt (not a continuation prompt).
	 * Main prompts typically have '% ' or '$ ' or '# ' at the start or after path.
	 * Continuation prompts are just '> ' at the start. */
	Line line = TLINE(screen_y);
	int linelen = tlinelen(screen_y);

	for (int i = 0; i < linelen && i + 1 < term.col; i++) {
		Rune c = line[i].u;
		Rune next = line[i + 1].u;
		/* Main prompt delimiters (not >) */
		if ((c == '%' || c == '$' || c == '#') && next == ' ') {
			return 1;
		}
	}
	return 0;
}

static int
vimnav_find_prompt_start_y(void)
{
	/* Find the Y position where the prompt starts by searching upward
	 * from term.c.y for a line with a main prompt delimiter.
	 * For multi-line commands, this finds the first line of the command. */
	int y;

	for (y = term.c.y; y >= 0; y--) {
		if (vimnav_has_main_prompt(y)) {
			return y;
		}
	}
	/* If no prompt found, assume current line */
	return term.c.y;
}

static int
vimnav_is_prompt_space(int y)
{
	/* Check if y is within the prompt space (multi-line command region).
	 * Returns 1 if y is between prompt_start and term.c.y (inclusive). */
	int prompt_start;

	if (term.scr != 0) {
		return 0;  /* Not at bottom of scrollback */
	}

	prompt_start = vimnav_find_prompt_start_y();
	return (y >= prompt_start && y <= term.c.y);
}

static void
vimnav_yank_line(void)
{
	int screen_y = vimnav_screen_y();

	/* On prompt line, only yank the command input (after prompt) */
	if (term.scr == 0 && vimnav.y == term.c.y) {
		int start_x = vimnav_find_prompt_end(screen_y);
		int linelen = tlinelen(screen_y);

		/* If nothing after prompt, nothing to yank */
		if (start_x >= linelen) {
			return;
		}

		/* Character-level selection from prompt end to line end */
		selstart(start_x, screen_y, 0);
		sel.mode = SEL_READY;  /* Required for selextend to work with done=1 */
		selextend(linelen - 1, screen_y, SEL_REGULAR, 1);
	} else {
		/* For non-prompt lines, use SNAP_LINE to select whole line */
		selstart(0, screen_y, 0);
		sel.snap = SNAP_LINE;
		sel.mode = SEL_READY;  /* Required for selextend to work with done=1 */
		selextend(term.col - 1, screen_y, SEL_REGULAR, 1);
	}

	char *text = getsel();
	if (text) {
		xsetsel(text);
		xclipcopy();
	}

	selclear();
	tfulldirt();
}

void
vimnav_enter(void)
{
	int prompt_end;

	if (vimnav.mode != VIMNAV_INACTIVE || IS_SET(MODE_ALTSCREEN))
		return;

	/* If scrolled away from prompt, scroll back first for clean state */
	if (term.scr > 0) {
		kscrolldown(&(Arg){ .i = term.scr });
	}

	vimnav.y = term.c.y;  /* Screen row */
	vimnav.prompt_y = term.c.y;  /* Can't go below this row when scr == 0 */
	vimnav.scr_at_entry = term.scr;  /* Track scroll position at entry (should be 0) */
	vimnav.pending_textobj = 0;  /* Clear any pending text object state */

	/* Use zsh-reported cursor position for x coordinate */
	prompt_end = vimnav_find_prompt_end(vimnav.y);
	vimnav.x = prompt_end + vimnav.zsh_cursor;
	vimnav.savedx = vimnav.x;
	vimnav.ox = vimnav.x;
	vimnav.oy = vimnav.y;
	vimnav.last_shell_x = vimnav.x;

	/* Check if zsh was in visual mode - if so, inherit the selection */
	if (vimnav.zsh_visual) {
		if (vimnav.zsh_visual_line) {
			vimnav.mode = VIMNAV_VISUAL_LINE;
		} else {
			vimnav.mode = VIMNAV_VISUAL;
		}
		/* Set anchor to zsh's visual anchor position */
		vimnav.anchor_x = prompt_end + vimnav.zsh_visual_anchor;
		vimnav.anchor_abs_y = vimnav.y - term.scr;  /* Anchor is on prompt line */
		vimnav_update_selection();
	} else {
		vimnav.mode = VIMNAV_NORMAL;
		selclear();
	}

	tfulldirt();
}

void
vimnav_exit(void)
{
	if (vimnav.mode == VIMNAV_INACTIVE)
		return;

	vimnav.mode = VIMNAV_INACTIVE;
	vimnav.pending_textobj = 0;
	selclear();
	tfulldirt();
}

int
vimnav_handle_key(ulong ksym, uint state)
{
	int handled = 1;

	/* Ignore modifier-only key presses (Shift, Ctrl, Alt, Super) */
	if (ksym >= 0xffe1 && ksym <= 0xffee)  /* XK_Shift_L to XK_Hyper_R */
		return 1;

	/* Handle pending text object (after 'i' or 'a' was pressed) */
	if (vimnav.pending_textobj) {
		int inner = (vimnav.pending_textobj == 'i');
		vimnav.pending_textobj = 0;  /* Clear pending state */
		if (vimnav_handle_textobj(ksym, inner))
			return 1;
		/* If text object not found, fall through to normal handling */
	}

	/* Handle Ctrl+scroll commands */
	if (state & ControlMask) {
		switch (ksym) {
		case 'e':
			vimnav_scroll_up(1);
			return 1;
		case 'y':
			vimnav_scroll_down(1);
			return 1;
		case 'u':
			vimnav_scroll_up(term.row / 2);
			return 1;
		case 'd':
			vimnav_scroll_down(term.row / 2);
			return 1;
		case 'b':
			vimnav_scroll_up(term.row);
			return 1;
		case 'f':
			vimnav_scroll_down(term.row);
			return 1;
		default:
			return 0;  /* Unknown Ctrl key, exit vim mode */
		}
	}

	/* Only handle unmodified keys or Shift */
	if (state & ~ShiftMask)
		return 0;

	switch (ksym) {
	/* Movement keys */
	case 'h':
		/* In prompt space: always pass to zsh (zsh has cursor authority) */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;  /* Pass through to zsh */
		}
		vimnav_move_left();
		break;
	case 'j':
		vimnav_move_down();
		break;
	case 'k':
		vimnav_move_up();
		break;
	case 'l':
		/* In prompt space: always pass to zsh (zsh has cursor authority) */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;  /* Pass through to zsh */
		}
		vimnav_move_right();
		break;
	case '0':
		/* In prompt space: always pass to zsh (zsh has cursor authority) */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;  /* Pass through to zsh */
		}
		vimnav_move_bol();
		break;
	case '$':
	case 'w':
	case 'b':
	case 'e':
	case 'W':
	case 'B':
	case 'E':
		/* In prompt space: always pass to zsh (zsh has cursor authority) */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;  /* Pass through to zsh */
		}
		if (ksym == '$')
			vimnav_move_eol();
		else if (ksym == 'w')
			vimnav_move_word_forward();
		else if (ksym == 'b')
			vimnav_move_word_backward();
		else if (ksym == 'e')
			vimnav_move_word_end();
		else if (ksym == 'W')
			vimnav_move_WORD_forward();
		else if (ksym == 'B')
			vimnav_move_WORD_backward();
		else if (ksym == 'E')
			vimnav_move_WORD_end();
		break;
	case 'g':
		vimnav_move_top();
		break;
	case 'G':
		vimnav_move_bottom();
		break;

	/* Visual mode */
	case 'v':
		/* In prompt space: pass to zsh (zsh handles char selection) */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;  /* Pass through to zsh */
		}
		vimnav_toggle_visual_char();
		break;
	case 'V':
		/* Line visual always handled by st (enters nav mode with line selection) */
		vimnav_toggle_visual_line();
		break;

	/* Text object prefix: 'i' for inner, 'a' for around (only in visual mode) */
	case 'i':
	case 'a':
		/* In visual mode: start text object sequence */
		if (vimnav.mode == VIMNAV_VISUAL || vimnav.mode == VIMNAV_VISUAL_LINE) {
			vimnav.pending_textobj = ksym;
			break;
		}
		/* In normal mode: snap to prompt and pass to zsh (insert/append) */
		vimnav_snap_to_prompt();
		return 0;

	/* Editing operations - snap to prompt and pass to zsh */
	case 'x':  /* delete char */
	case 'X':  /* delete char before */
	case 'd':  /* delete with motion */
	case 'D':  /* delete to end of line */
	case 'c':  /* change with motion */
	case 'C':  /* change to end of line */
	case 's':  /* substitute char */
	case 'S':  /* substitute line */
	case 'r':  /* replace char */
	case 'R':  /* replace mode */
	case 'A':  /* append at end of line */
	case 'I':  /* insert at beginning of line */
	case 'o':  /* open line below */
	case 'O':  /* open line above */
	case 'u':  /* undo */
	case '.':  /* repeat last command */
	case '~':  /* toggle case */
		vimnav_snap_to_prompt();
		return 0;  /* Pass through to zsh */

	/* Yank */
	case 'y':
		if (vimnav.mode == VIMNAV_VISUAL || vimnav.mode == VIMNAV_VISUAL_LINE) {
			/* In visual mode, yank selection */
			vimnav_yank_selection();
			vimnav.mode = VIMNAV_NORMAL;
			vimnav_notify_zsh_visual_end();  /* Tell zsh to exit visual mode */
			selclear();
			vimnav_sync_to_zsh_cursor();  /* Sync cursor if in prompt space */
			tfulldirt();
		} else if (vimnav.zsh_visual) {
			/* zsh is in visual mode on prompt line - yank and clear */
			vimnav_yank_selection();
			vimnav_notify_zsh_visual_end();  /* Tell zsh to exit visual mode */
			selclear();
			tfulldirt();
		} else {
			/* No selection: yank line (or just command on prompt line) */
			vimnav_yank_line();
		}
		break;

	/* Paste */
	case 'p':
		vimnav_snap_to_prompt();
		vimnav_paste_strip_newlines = 1;
		clippaste(NULL);
		break;

	/* Escape: clear visual selection or stay in normal mode */
	case 0xff1b: /* XK_Escape */
		if (vimnav.mode == VIMNAV_VISUAL || vimnav.mode == VIMNAV_VISUAL_LINE) {
			vimnav.mode = VIMNAV_NORMAL;
			vimnav_notify_zsh_visual_end();  /* Tell zsh to exit visual mode */
			selclear();
			vimnav_sync_to_zsh_cursor();  /* Sync cursor if in prompt space */
			tfulldirt();
		} else if (vimnav.zsh_visual) {
			/* zsh is in visual mode on prompt line - clear st's rendering and notify zsh */
			vimnav_notify_zsh_visual_end();  /* Sends Escape to zsh and clears flag */
			selclear();
			tfulldirt();
		}
		break;

	default:
		handled = 0;
		break;
	}

	return handled;
}
