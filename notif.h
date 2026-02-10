/* See LICENSE for license details. */
/* Notification overlay for st terminal - stacked toast system */

#ifndef NOTIF_H
#define NOTIF_H

#include <time.h>

#ifndef TIMEDIFF
#define TIMEDIFF(t1, t2)	((t1.tv_sec-t2.tv_sec)*1000 + \
				(t1.tv_nsec-t2.tv_nsec)/1E6)
#endif

/*
 * Stacked toast notification configuration.
 * Shows popups in the top-right corner when _ST_NOTIFY property is set.
 * New notifications appear at the top, pushing old ones down.
 * Each toast auto-dismisses independently after notif_display_ms.
 */
static const char *notif_border_color = "#1d9bf0";    /* blue */
static const char *notif_bg_color = "#00050f";         /* terminal background */
static const char *notif_fg_color = "#ffffff";         /* white text */
static const int notif_border_width = 2;               /* border thickness */
static const int notif_margin = 10;                    /* margin from parent window edge */
static const int notif_padding = 8;                    /* internal padding */
static const float notif_font_scale = 1.5;             /* font size multiplier */
static const int notif_display_ms = 5000;              /* auto-dismiss after 5 seconds */
static const int notif_toast_gap = 6;                  /* vertical gap between stacked toasts */

#define NOTIF_MAX_TOASTS 8                             /* max simultaneous toasts */
#define NOTIF_MAX_LINES  16                            /* max lines per toast message */

/* Public functions */
void notif_show(const char *msg);
void notif_hide(void);
void notif_draw(void);
void notif_resize(void);
int notif_active(void);
int notif_check_timeout(struct timespec *now);

#endif /* NOTIF_H */
