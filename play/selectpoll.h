/* poll() on top of select(): macOS poll() doesn't support character
 * devices, so it can't wait on a tty or a pty. */
#ifndef PLAY_SELECTPOLL_H
#define PLAY_SELECTPOLL_H

#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>

static int
select_poll(struct pollfd *fds, int n, int timeout_ms)
{
	fd_set r, w;
	struct timeval tv, *tvp = NULL;
	int i, max = -1, ret;

	FD_ZERO(&r);
	FD_ZERO(&w);
	for (i = 0; i < n; i++) {
		fds[i].revents = 0;
		if (fds[i].fd < 0)
			continue;
		if (fds[i].events & POLLIN)
			FD_SET(fds[i].fd, &r);
		if (fds[i].events & POLLOUT)
			FD_SET(fds[i].fd, &w);
		if (fds[i].fd > max)
			max = fds[i].fd;
	}
	if (timeout_ms >= 0) {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		tvp = &tv;
	}
	ret = select(max + 1, &r, &w, NULL, tvp);
	if (ret <= 0)
		return ret;
	for (i = 0; i < n; i++) {
		if (fds[i].fd < 0)
			continue;
		if (FD_ISSET(fds[i].fd, &r))
			fds[i].revents |= POLLIN;
		if (FD_ISSET(fds[i].fd, &w))
			fds[i].revents |= POLLOUT;
	}
	return ret;
}

#endif
