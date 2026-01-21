/* See LICENSE for license details. */
/* SSH indicator overlay for st terminal */

#ifndef SSHIND_H
#define SSHIND_H

/*
 * SSH indicator overlay configuration.
 * Shows a small window in the top-right corner when SSH'd into a remote host.
 */
static const char *sshind_border_color = "#b18dff";  /* light purple */
static const char *sshind_bg_color = "#0d0015";      /* very dark purple */
static const char *sshind_fg_color = "#ffffff";      /* white text */
static const int sshind_border_width = 2;            /* border thickness */
static const int sshind_margin = 10;                 /* margin from parent window edge */
static const int sshind_padding = 8;                 /* internal padding */
static const float sshind_font_scale = 1.5;          /* font size multiplier */

/* Public functions */
void sshind_show(const char *host);
void sshind_hide(void);
void sshind_draw(void);
void sshind_resize(void);
int sshind_active(void);

#endif /* SSHIND_H */
