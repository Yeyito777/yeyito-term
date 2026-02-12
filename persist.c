/* See LICENSE for license details. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "st.h"
#include "persist.h"

/* Access to st.c internals (duplicated, suckless pattern) */
#define HISTSIZE      (1 << 15)
#define IS_SET(flag)  ((term.mode & (flag)) != 0)

enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_INSERT      = 1 << 1,
	MODE_ALTSCREEN   = 1 << 2,
};

typedef struct {
	Glyph attr;
	int x;
	int y;
	char state;
} TCursor;

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
	int histn;
} Term;

extern Term term;

/* Declared in win.h, implemented in x.c */
extern void xsetdwmsaveargv(const char *);

#define PERSIST_MAGIC   "STHIST"
#define PERSIST_VERSION 1

typedef struct {
	char magic[6];
	uint16_t version;
	uint16_t col;
	uint16_t row;
	uint16_t histi;
	uint16_t histn;
	uint16_t pad;
} PersistHeader;

static char persistdir[PATH_MAX];
static char persist_cwd_buf[PATH_MAX];
static int initialized;

static void
mkdirp(const char *path)
{
	char tmp[PATH_MAX];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0700);
			*p = '/';
		}
	}
	mkdir(tmp, 0700);
}

static void
rmdir_recursive(const char *path)
{
	DIR *d;
	struct dirent *ent;
	struct stat st;
	char filepath[PATH_MAX];

	d = opendir(path);
	if (!d)
		return;
	while ((ent = readdir(d))) {
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;
		snprintf(filepath, sizeof(filepath), "%s/%s",
				path, ent->d_name);
		if (stat(filepath, &st) == 0 && S_ISDIR(st.st_mode))
			rmdir_recursive(filepath);
		else
			unlink(filepath);
	}
	closedir(d);
	rmdir(path);
}

static void
persist_save_scrollback(void)
{
	char path[PATH_MAX];
	FILE *f;
	PersistHeader hdr;
	int i, oldest;
	Line *screen;

	snprintf(path, sizeof(path), "%s/scrollback-history.save", persistdir);
	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "[persist] save scrollback: %s: %s\n",
				path, strerror(errno));
		return;
	}

	memcpy(hdr.magic, PERSIST_MAGIC, 6);
	hdr.version = PERSIST_VERSION;
	hdr.col = term.col;
	hdr.row = term.row;
	hdr.histi = term.histi;
	hdr.histn = term.histn;
	hdr.pad = 0;
	fwrite(&hdr, sizeof(hdr), 1, f);

	/* Write history lines oldest to newest.
	 * histi points to the most recently written line.
	 * oldest = (histi - histn + 1 + HISTSIZE) % HISTSIZE */
	if (term.histn > 0) {
		oldest = (term.histi - term.histn + 1 + HISTSIZE) % HISTSIZE;
		for (i = 0; i < term.histn; i++) {
			int idx = (oldest + i) % HISTSIZE;
			fwrite(term.hist[idx], sizeof(Glyph),
					term.col, f);
		}
	}

	/* Write screen lines (main screen, not alt) */
	screen = IS_SET(MODE_ALTSCREEN) ? term.alt : term.line;
	for (i = 0; i < term.row; i++)
		fwrite(screen[i], sizeof(Glyph), term.col, f);

	fclose(f);
}

static void
persist_save_generic(void)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "%s/generic-data.save", persistdir);
	f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "[persist] save generic: %s: %s\n",
				path, strerror(errno));
		return;
	}
	if (persist_cwd_buf[0])
		fprintf(f, "cwd=%s\n", persist_cwd_buf);
	fprintf(f, "cursor_y=%d\n", term.c.y);
	fclose(f);
}

static void
persist_sweep_orphans(const char *stdir)
{
	DIR *d;
	struct dirent *ent;
	char path[PATH_MAX];
	pid_t pid;

	d = opendir(stdir);
	if (!d)
		return;
	while ((ent = readdir(d))) {
		if (strncmp(ent->d_name, "st-", 3) != 0)
			continue;
		pid = atoi(ent->d_name + 3);
		if (pid <= 0)
			continue;
		/* kill(pid, 0) checks if process exists */
		if (kill(pid, 0) == -1 && errno == ESRCH) {
			snprintf(path, sizeof(path), "%s/%s", stdir, ent->d_name);
			rmdir_recursive(path);
		}
	}
	closedir(d);
}

void
persist_init(pid_t pid)
{
	const char *home;
	char stdir[PATH_MAX];
	char logpath[PATH_MAX];
	int logfd;

	home = getenv("HOME");
	if (!home)
		home = "/tmp";

	snprintf(stdir, sizeof(stdir), "%s/.runtime/st", home);
	persist_sweep_orphans(stdir);

	snprintf(persistdir, sizeof(persistdir),
			"%s/st-%d", stdir, (int)pid);
	mkdirp(persistdir);

	/* Redirect stderr to log.log */
	snprintf(logpath, sizeof(logpath), "%s/log.log", persistdir);
	logfd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (logfd >= 0) {
		dup2(logfd, STDERR_FILENO);
		close(logfd);
	}

	initialized = 1;
	fprintf(stderr, "[persist] initialized: %s\n", persistdir);
}

void
persist_register(void)
{
	char argv[PATH_MAX + 32];

	if (!initialized)
		return;

	snprintf(argv, sizeof(argv), "st --from-save %s", persistdir);
	xsetdwmsaveargv(argv);
	fprintf(stderr, "[persist] registered: %s\n", argv);
}

void
persist_save(void)
{
	if (!initialized)
		return;
	persist_save_scrollback();
	persist_save_generic();
}

void
persist_restore(const char *dir, int *out_col, int *out_row)
{
	char path[PATH_MAX];
	FILE *f;
	char line[PATH_MAX + 16];
	PersistHeader hdr;
	int i, histn, rows;
	int cursor_y = -1;

	/* Read generic data */
	snprintf(path, sizeof(path), "%s/generic-data.save", dir);
	f = fopen(path, "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = '\0';
			if (strncmp(line, "cwd=", 4) == 0)
				persist_set_cwd(line + 4);
			else if (strncmp(line, "cursor_y=", 9) == 0)
				cursor_y = atoi(line + 9);
		}
		fclose(f);
	}
	/* Read scrollback history */
	snprintf(path, sizeof(path), "%s/scrollback-history.save", dir);
	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "[persist] restore: no scrollback file: %s\n",
				path);
		goto cleanup;
	}

	if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
		fprintf(stderr, "[persist] restore: short header read\n");
		fclose(f);
		goto cleanup;
	}
	if (memcmp(hdr.magic, PERSIST_MAGIC, 6) != 0) {
		fprintf(stderr, "[persist] restore: bad magic\n");
		fclose(f);
		goto cleanup;
	}
	if (hdr.version != PERSIST_VERSION) {
		fprintf(stderr, "[persist] restore: version %d != %d\n",
				hdr.version, PERSIST_VERSION);
		fclose(f);
		goto cleanup;
	}

	/* Resize terminal to saved dimensions */
	if (hdr.col != term.col || hdr.row != term.row)
		tresize(hdr.col, hdr.row);

	/* Report restored dimensions so xinit can size the window correctly */
	if (out_col)
		*out_col = hdr.col;
	if (out_row)
		*out_row = hdr.row;

	/* Read history lines into term.hist[0..histn-1] */
	histn = hdr.histn;
	if (histn > HISTSIZE)
		histn = HISTSIZE;
	for (i = 0; i < histn; i++) {
		if (fread(term.hist[i], sizeof(Glyph), hdr.col, f)
				!= (size_t)hdr.col) {
			fprintf(stderr, "[persist] restore: short history "
					"read at line %d\n", i);
			histn = i;
			break;
		}
	}
	term.histi = histn > 0 ? histn - 1 : 0;
	term.histn = histn;
	term.scr = 0;

	/* Read screen lines */
	rows = hdr.row;
	if (rows > term.row)
		rows = term.row;
	for (i = 0; i < rows; i++) {
		if (fread(term.line[i], sizeof(Glyph), hdr.col, f)
				!= (size_t)hdr.col) {
			fprintf(stderr, "[persist] restore: short screen "
					"read at line %d\n", i);
			break;
		}
	}

	fclose(f);

	/* Restore cursor row so the new shell prompt overwrites the old one */
	if (cursor_y >= 0 && cursor_y < term.row) {
		term.c.y = cursor_y;
		term.c.x = 0;
	}

	tfulldirt();

cleanup:
	/* Delete consumed directory */
	rmdir_recursive(dir);
}

void
persist_cleanup(void)
{
	if (!initialized)
		return;
	rmdir_recursive(persistdir);
	initialized = 0;
}

int
persist_active(void)
{
	return initialized;
}

void
persist_set_cwd(const char *cwd)
{
	if (cwd)
		snprintf(persist_cwd_buf, sizeof(persist_cwd_buf), "%s", cwd);
	else
		persist_cwd_buf[0] = '\0';
}

const char *
persist_get_cwd(void)
{
	return persist_cwd_buf;
}

const char *
persist_get_dir(void)
{
	return persistdir;
}
