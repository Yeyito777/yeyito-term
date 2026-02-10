/* See LICENSE for license details. */
/* Notification overlay for st terminal */

#ifndef NOTIF_H
#define NOTIF_H

#include <time.h>

#ifndef TIMEDIFF
#define TIMEDIFF(t1, t2)	((t1.tv_sec-t2.tv_sec)*1000 + \
				(t1.tv_nsec-t2.tv_nsec)/1E6)
#endif

/*
 * Notification overlay configuration.
 * Shows a popup in the top-right corner when _ST_NOTIFY property is set.
 */
static const char *notif_border_color = "#5fafd7";   /* steel blue */
static const char *notif_bg_color = "#001520";        /* very dark blue */
static const char *notif_fg_color = "#ffffff";        /* white text */
static const int notif_border_width = 2;              /* border thickness */
static const int notif_margin = 10;                   /* margin from parent window edge */
static const int notif_padding = 8;                   /* internal padding */
static const float notif_font_scale = 1.5;            /* font size multiplier */
static const int notif_display_ms = 5000;             /* auto-dismiss after 5 seconds */

/* Public functions */
void notif_show(const char *msg);
void notif_hide(void);
void notif_draw(void);
void notif_resize(void);
int notif_active(void);
int notif_check_timeout(struct timespec *now);

#endif /* NOTIF_H */
