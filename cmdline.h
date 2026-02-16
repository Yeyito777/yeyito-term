/* See LICENSE for license details. */
/* Command-line mode for st terminal (vim-like : command) */

#ifndef CMDLINE_H
#define CMDLINE_H

/*
 * Command-line window configuration.
 * Shows at the bottom of the terminal when ':' is pressed in nav mode.
 * Simulates vim's : command-line interface.
 */
static const char *cmdline_bg_color = "#00050f";     /* terminal background */
static const char *cmdline_fg_color = "#f1faee";     /* terminal foreground */
static const char *cmdline_err_color = "#febfb8";    /* error text color */
static const char *cmdline_cursor_color = "#48cae4"; /* cursor color */
static const char *cmdline_border_color = "#1d3557"; /* top border */
static const char *cmdline_sel_color = "#4f5258";    /* visual selection */
static const int cmdline_border_top = 1;             /* top border thickness */

#define CMDLINE_MAX_INPUT 256
#define CMDLINE_HIST_MAX  64

/* Public functions */
void cmdline_init(void);
void cmdline_open(void);
void cmdline_close(void);
int cmdline_handle_key(unsigned long ksym, unsigned int state,
                       const char *buf, int len);
void cmdline_draw(void);
void cmdline_resize(void);
int cmdline_active(void);

#endif /* CMDLINE_H */
