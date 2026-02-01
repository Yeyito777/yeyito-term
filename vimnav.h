/* See LICENSE for license details. */
/* Vim navigation mode for st terminal */

#ifndef VIMNAV_H
#define VIMNAV_H

#include "st.h"  /* for uint, ulong types */

/* Vim navigation state structure */
typedef struct {
	int mode;           /* vimnav_mode state */
	int x, y;           /* vim cursor position (screen row) */
	int ox, oy;         /* old position for cursor redraw */
	int savedx;         /* saved x column for vertical movement */
	int prompt_y;       /* y position of shell prompt (can't go below) */
	int scr_at_entry;   /* scroll position when entering vim mode */
	int anchor_x;       /* visual mode anchor x (screen column) */
	int anchor_abs_y;   /* visual mode anchor y (absolute: screen_y - term.scr) */
	int last_shell_x;   /* last known shell cursor x for sync detection */
	int pending_y;      /* waiting for second y in yy sequence */
	/* zsh-reported state (for cursor sync) */
	int zsh_cursor;     /* cursor position reported by zsh */
	int zsh_visual;     /* 1 if zsh is in visual mode */
	int zsh_visual_anchor; /* anchor position for zsh visual mode */
	int zsh_visual_line;   /* 1 if zsh visual mode is line-wise (V) */
	/* Text object pending state */
	int pending_textobj; /* 'i' for inner, 'a' for around, 0 for none */
	/* f/F find character state */
	int pending_find;    /* 'f' or 'F' when waiting for char, 0 otherwise */
	Rune last_find_char; /* character from last f/F search */
	int last_find_forward; /* 1 if last search was f (forward), 0 if F (backward) */
	/* g command state (for gg) */
	int pending_g;       /* 1 when waiting for second g, 0 otherwise */
} VimNav;

/* Global vim navigation state (defined in vimnav.c) */
extern VimNav vimnav;

/* Public functions */
void vimnav_enter(void);
void vimnav_exit(void);
int tisvimnav(void);
int tisvimnav_paste(void);
void vimnav_paste_done(void);
int vimnav_handle_key(ulong ksym, uint state);

/* zsh cursor/visual sync functions (called from st.c OSC handler) */
void vimnav_set_zsh_cursor(int pos);
void vimnav_set_zsh_visual(int active, int anchor, int line_mode);

#endif /* VIMNAV_H */
