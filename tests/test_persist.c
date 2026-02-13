/* See LICENSE for license details. */
/* Unit tests for st persistence (save/restore) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>

#include "test.h"
#include "../st.h"
#include "../persist.h"

/* Duplicate Term definition from st.c (suckless pattern).
 * Must match the layout in st.c exactly. */
#define HISTSIZE (1 << 15)

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

Term term;

/* Stubs for st.h functions */
void die(const char *errstr, ...) { (void)errstr; }
void *xmalloc(size_t len) { return malloc(len); }
void *xrealloc(void *p, size_t len) { return realloc(p, len); }
char *xstrdup(const char *s) { return strdup(s); }
void tfulldirt(void) {}
void redraw(void) {}
void draw(void) {}
size_t utf8encode(Rune u, char *c) { (void)u; (void)c; return 0; }
void kscrolldown(const Arg *a) { (void)a; }
void kscrollup(const Arg *a) { (void)a; }
void printscreen(const Arg *a) { (void)a; }
void printsel(const Arg *a) { (void)a; }
void sendbreak(const Arg *a) { (void)a; }
void toggleprinter(const Arg *a) { (void)a; }
int tattrset(int a) { (void)a; return 0; }
int tisaltscreen(void) { return 0; }
int tlinelen(int y) { (void)y; return 0; }
void tnew(int c, int r) { (void)c; (void)r; }
void tsetdirtattr(int a) { (void)a; }
void ttyhangup(void) {}
int ttynew(const char *a, char *b, const char *c, char **d)
	{ (void)a; (void)b; (void)c; (void)d; return 0; }
size_t ttyread(void) { return 0; }
void ttyresize(int a, int b) { (void)a; (void)b; }
void ttywrite(const char *a, size_t b, int c) { (void)a; (void)b; (void)c; }
void resettitle(void) {}
void selclear(void) {}
void selinit(void) {}
void selstart(int a, int b, int c) { (void)a; (void)b; (void)c; }
void selextend(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
int selected(int a, int b) { (void)a; (void)b; return 0; }
char *getsel(void) { return NULL; }
void vimnav_enter(void) {}
void vimnav_exit(void) {}
int tisvimnav(void) { return 0; }
int tisvimnav_paste(void) { return 0; }
void vimnav_paste_done(void) {}
int vimnav_handle_key(unsigned long a, unsigned int b) { (void)a; (void)b; return 0; }
void sshind_show(const char *a) { (void)a; }
void sshind_hide(void) {}
void sshind_draw(void) {}
void sshind_resize(void) {}
int sshind_active(void) { return 0; }
int sshind_height(void) { return 0; }
void notif_show(const char *a) { (void)a; }
void notif_hide(void) {}
void notif_draw(void) {}
void notif_resize(void) {}
int notif_active(void) { return 0; }
int notif_check_timeout(struct timespec *a) { (void)a; return 0; }

/* Mock xsetdwmsaveargv */
static char mock_dwm_argv[PATH_MAX];
void xsetdwmsaveargv(const char *argv)
{
	snprintf(mock_dwm_argv, sizeof(mock_dwm_argv), "%s", argv);
}

/* Mock tresize */
void tresize(int col, int row)
{
	int i;

	for (i = 0; i < term.row; i++) {
		free(term.line[i]);
		free(term.alt[i]);
	}
	free(term.line);
	free(term.alt);
	free(term.dirty);
	free(term.tabs);

	term.col = col;
	term.row = row;
	term.line = calloc(row, sizeof(Line));
	term.alt = calloc(row, sizeof(Line));
	term.dirty = calloc(row, sizeof(int));
	term.tabs = calloc(col, sizeof(int));
	for (i = 0; i < row; i++) {
		term.line[i] = calloc(col, sizeof(Glyph));
		term.alt[i] = calloc(col, sizeof(Glyph));
	}
	for (i = 0; i < HISTSIZE; i++)
		term.hist[i] = realloc(term.hist[i], col * sizeof(Glyph));
}

/* Helpers */
static void
setup_term(int col, int row)
{
	int i;

	memset(&term, 0, sizeof(term));
	term.col = col;
	term.row = row;
	term.line = calloc(row, sizeof(Line));
	term.alt = calloc(row, sizeof(Line));
	term.dirty = calloc(row, sizeof(int));
	term.tabs = calloc(col, sizeof(int));
	for (i = 0; i < row; i++) {
		term.line[i] = calloc(col, sizeof(Glyph));
		term.alt[i] = calloc(col, sizeof(Glyph));
	}
	for (i = 0; i < HISTSIZE; i++)
		term.hist[i] = calloc(col, sizeof(Glyph));
}

static void
cleanup_term(void)
{
	int i;

	for (i = 0; i < term.row; i++) {
		free(term.line[i]);
		free(term.alt[i]);
	}
	free(term.line);
	free(term.alt);
	free(term.dirty);
	free(term.tabs);
	for (i = 0; i < HISTSIZE; i++)
		free(term.hist[i]);
	memset(&term, 0, sizeof(term));
}

static char testdir[PATH_MAX];

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
setup_testdir(void)
{
	snprintf(testdir, sizeof(testdir), "/tmp/st-test-%d", getpid());
	mkdir(testdir, 0700);
}

static void
cleanup_testdir(void)
{
	rmdir_recursive(testdir);
}

/* === CWD tracking tests === */

TEST(cwd_set_and_get)
{
	persist_set_cwd("/home/user/project");
	ASSERT_STR_EQ("/home/user/project", persist_get_cwd());
}

TEST(cwd_overwrite)
{
	persist_set_cwd("/first");
	persist_set_cwd("/second");
	ASSERT_STR_EQ("/second", persist_get_cwd());
}

TEST(cwd_null_clears)
{
	persist_set_cwd("/something");
	persist_set_cwd(NULL);
	ASSERT_STR_EQ("", persist_get_cwd());
}

/* === Scrollback save/restore roundtrip via persist_save/persist_restore === */

TEST(full_roundtrip)
{
	char savedir[PATH_MAX];
	char restoredir[PATH_MAX];

	setup_term(8, 3);

	/* Set up state */
	persist_set_cwd("/home/test/project");
	term.c.y = 2;
	term.c.x = 5;
	term.histn = 2;
	term.histi = 1;
	term.hist[0][0].u = 'X';
	term.hist[0][0].fg = 42;
	term.hist[1][0].u = 'Y';
	term.hist[1][0].fg = 84;
	term.line[0][0].u = '$';
	term.line[0][0].fg = 256;

	/* Save via persist module */
	setup_testdir();
	snprintf(savedir, sizeof(savedir), "%s/full", testdir);
	mkdir(savedir, 0700);
	/* Temporarily set persist internals — call init with fake pid */
	persist_init(99999);
	/* Override the dir to our testdir */
	{
		/* We can't set persistdir directly since it's static in persist.c.
		 * Instead, use persist_get_dir() for verification and accept
		 * that save goes to the init dir. Let's save from the init dir. */
	}
	persist_save();

	/* Verify files exist */
	{
		char path[PATH_MAX];
		struct stat st;
		snprintf(path, sizeof(path), "%s/scrollback-history.save",
				persist_get_dir());
		ASSERT(stat(path, &st) == 0);
		snprintf(path, sizeof(path), "%s/generic-data.save",
				persist_get_dir());
		ASSERT(stat(path, &st) == 0);
	}

	/* Copy dir for restore (restore deletes the dir) */
	snprintf(restoredir, sizeof(restoredir), "%s/restore", testdir);
	{
		char cmd[PATH_MAX * 2 + 16];
		snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'",
				persist_get_dir(), restoredir);
		system(cmd);
	}

	/* Reset */
	cleanup_term();
	setup_term(8, 3);
	persist_set_cwd(NULL);

	/* Restore */
	persist_restore(restoredir, NULL, NULL);

	/* Verify CWD */
	ASSERT_STR_EQ("/home/test/project", persist_get_cwd());

	/* Verify history */
	ASSERT_EQ(2, term.histn);
	ASSERT_EQ('X', (int)term.hist[0][0].u);
	ASSERT_EQ(42, (int)term.hist[0][0].fg);
	ASSERT_EQ('Y', (int)term.hist[1][0].u);
	ASSERT_EQ(84, (int)term.hist[1][0].fg);

	/* Verify screen */
	ASSERT_EQ('$', (int)term.line[0][0].u);
	ASSERT_EQ(256, (int)term.line[0][0].fg);

	/* Verify cursor position */
	ASSERT_EQ(2, term.c.y);
	ASSERT_EQ(0, term.c.x);

	persist_cleanup();
	cleanup_term();
	cleanup_testdir();
}

TEST(empty_history_roundtrip)
{
	char restoredir[PATH_MAX];

	setup_term(10, 3);

	/* Screen content only, no history */
	term.line[0][0].u = 'A';
	term.line[0][0].fg = 100;
	term.line[1][0].u = 'B';
	term.line[1][0].fg = 200;

	setup_testdir();
	persist_init(99998);
	persist_save();

	snprintf(restoredir, sizeof(restoredir), "%s/restore", testdir);
	{
		char cmd[PATH_MAX * 2 + 16];
		snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'",
				persist_get_dir(), restoredir);
		system(cmd);
	}

	cleanup_term();
	setup_term(10, 3);

	persist_restore(restoredir, NULL, NULL);

	ASSERT_EQ('A', (int)term.line[0][0].u);
	ASSERT_EQ(100, (int)term.line[0][0].fg);
	ASSERT_EQ('B', (int)term.line[1][0].u);
	ASSERT_EQ(200, (int)term.line[1][0].fg);
	ASSERT_EQ(0, term.histn);

	persist_cleanup();
	cleanup_term();
	cleanup_testdir();
}

TEST(cursor_y_restored)
{
	char restoredir[PATH_MAX];

	setup_term(10, 5);

	/* Cursor at row 3 (like an idle prompt mid-screen) */
	term.c.y = 3;
	term.c.x = 7;
	term.line[3][0].u = '$';

	setup_testdir();
	persist_init(99997);
	persist_save();

	snprintf(restoredir, sizeof(restoredir), "%s/restore", testdir);
	{
		char cmd[PATH_MAX * 2 + 16];
		snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'",
				persist_get_dir(), restoredir);
		system(cmd);
	}

	cleanup_term();
	setup_term(10, 5);
	term.c.y = 0;
	term.c.x = 0;

	persist_restore(restoredir, NULL, NULL);

	ASSERT_EQ(3, term.c.y);
	ASSERT_EQ(0, term.c.x);
	ASSERT_EQ('$', (int)term.line[3][0].u);

	persist_cleanup();
	cleanup_term();
	cleanup_testdir();
}

TEST(bad_magic_skipped)
{
	char dir[PATH_MAX];
	char path[PATH_MAX];
	FILE *f;

	setup_term(5, 2);
	setup_testdir();
	snprintf(dir, sizeof(dir), "%s/bad", testdir);
	mkdir(dir, 0700);

	snprintf(path, sizeof(path), "%s/scrollback-history.save", dir);
	f = fopen(path, "wb");
	ASSERT_NOT_NULL(f);
	fprintf(f, "BADMAGIC");
	fclose(f);

	persist_restore(dir, NULL, NULL);
	ASSERT_EQ(0, term.histn);

	cleanup_term();
	cleanup_testdir();
}

TEST(register_sets_dwm_argv)
{
	setup_testdir();
	persist_init(88888);

	mock_dwm_argv[0] = '\0';
	persist_register();

	ASSERT(strstr(mock_dwm_argv, "st --from-save") != NULL);
	ASSERT(strstr(mock_dwm_argv, "88888") != NULL);

	persist_cleanup();
	cleanup_testdir();
}

/* === Altcmd tracking tests === */

/* MODE_ALTSCREEN = 1 << 2 */
#define MODE_ALTSCREEN (1 << 2)

TEST(altcmd_set_and_get)
{
	persist_set_altcmd("htop");
	ASSERT_STR_EQ("htop", persist_get_altcmd());
	persist_set_altcmd(NULL);
}

TEST(altcmd_null_clears)
{
	persist_set_altcmd("nvim foo.txt");
	persist_set_altcmd(NULL);
	ASSERT_STR_EQ("", persist_get_altcmd());
}

TEST(altcmd_saved_when_altscreen)
{
	char restoredir[PATH_MAX];

	setup_term(8, 3);
	persist_set_altcmd("htop");
	term.mode |= MODE_ALTSCREEN;

	setup_testdir();
	persist_init(99990);
	persist_save();

	snprintf(restoredir, sizeof(restoredir), "%s/restore", testdir);
	{
		char cmd[PATH_MAX * 2 + 16];
		snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'",
				persist_get_dir(), restoredir);
		system(cmd);
	}

	/* Reset */
	cleanup_term();
	setup_term(8, 3);
	persist_set_altcmd(NULL);
	term.mode = 0;

	persist_restore(restoredir, NULL, NULL);
	ASSERT_STR_EQ("htop", persist_get_altcmd());

	persist_set_altcmd(NULL);
	persist_cleanup();
	cleanup_term();
	cleanup_testdir();
}

/* === Save command override tests === */

TEST(save_cmd_set_and_get)
{
	persist_set_save_cmd("claude --resume abc-123");
	ASSERT_STR_EQ("claude --resume abc-123", persist_get_save_cmd());
	persist_set_save_cmd(NULL);
}

TEST(save_cmd_null_clears)
{
	persist_set_save_cmd("something");
	persist_set_save_cmd(NULL);
	ASSERT_STR_EQ("", persist_get_save_cmd());
}

TEST(save_cmd_overrides_altcmd)
{
	char restoredir[PATH_MAX];

	setup_term(8, 3);
	persist_set_altcmd("claude");
	persist_set_save_cmd("claude --resume abc-123");
	term.mode |= MODE_ALTSCREEN;

	setup_testdir();
	persist_init(99980);
	persist_save();

	snprintf(restoredir, sizeof(restoredir), "%s/restore", testdir);
	{
		char cmd[PATH_MAX * 2 + 16];
		snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'",
				persist_get_dir(), restoredir);
		system(cmd);
	}

	/* Reset */
	cleanup_term();
	setup_term(8, 3);
	persist_set_altcmd(NULL);
	persist_set_save_cmd(NULL);
	term.mode = 0;

	persist_restore(restoredir, NULL, NULL);
	ASSERT_STR_EQ("claude --resume abc-123", persist_get_altcmd());

	persist_set_altcmd(NULL);
	persist_set_save_cmd(NULL);
	persist_cleanup();
	cleanup_term();
	cleanup_testdir();
}

TEST(save_cmd_saved_without_altscreen)
{
	char restoredir[PATH_MAX];

	setup_term(8, 3);
	persist_set_save_cmd("claude --resume abc-123");
	term.mode = 0; /* altscreen NOT set — save_cmd is explicit, always saved */

	setup_testdir();
	persist_init(99979);
	persist_save();

	snprintf(restoredir, sizeof(restoredir), "%s/restore", testdir);
	{
		char cmd[PATH_MAX * 2 + 16];
		snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'",
				persist_get_dir(), restoredir);
		system(cmd);
	}

	/* Reset */
	cleanup_term();
	setup_term(8, 3);
	persist_set_save_cmd(NULL);
	persist_set_altcmd(NULL);

	persist_restore(restoredir, NULL, NULL);
	ASSERT_STR_EQ("claude --resume abc-123", persist_get_altcmd());

	persist_set_altcmd(NULL);
	persist_cleanup();
	cleanup_term();
	cleanup_testdir();
}

TEST(altcmd_used_when_no_save_cmd)
{
	char restoredir[PATH_MAX];

	setup_term(8, 3);
	persist_set_altcmd("htop");
	persist_set_save_cmd(NULL); /* no save_cmd override */
	term.mode |= MODE_ALTSCREEN;

	setup_testdir();
	persist_init(99978);
	persist_save();

	snprintf(restoredir, sizeof(restoredir), "%s/restore", testdir);
	{
		char cmd[PATH_MAX * 2 + 16];
		snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'",
				persist_get_dir(), restoredir);
		system(cmd);
	}

	/* Reset */
	cleanup_term();
	setup_term(8, 3);
	persist_set_altcmd(NULL);

	persist_restore(restoredir, NULL, NULL);
	ASSERT_STR_EQ("htop", persist_get_altcmd());

	persist_set_altcmd(NULL);
	persist_cleanup();
	cleanup_term();
	cleanup_testdir();
}

/* === Ephemeral flag tests === */

TEST(ephemeral_set_and_get)
{
	persist_set_ephemeral(0);
	ASSERT_EQ(0, persist_is_ephemeral());
	persist_set_ephemeral(1);
	ASSERT_EQ(1, persist_is_ephemeral());
	persist_set_ephemeral(0);
}

TEST(ephemeral_saved_and_restored)
{
	char restoredir[PATH_MAX];

	setup_term(8, 3);
	persist_set_ephemeral(1);
	persist_set_save_cmd("agent --resume abc-123");

	setup_testdir();
	persist_init(99970);
	persist_save();

	snprintf(restoredir, sizeof(restoredir), "%s/restore", testdir);
	{
		char cmd[PATH_MAX * 2 + 16];
		snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'",
				persist_get_dir(), restoredir);
		system(cmd);
	}

	/* Reset */
	cleanup_term();
	setup_term(8, 3);
	persist_set_ephemeral(0);
	persist_set_save_cmd(NULL);
	persist_set_altcmd(NULL);

	persist_restore(restoredir, NULL, NULL);
	ASSERT_EQ(1, persist_is_ephemeral());
	ASSERT_STR_EQ("agent --resume abc-123", persist_get_altcmd());

	persist_set_ephemeral(0);
	persist_set_altcmd(NULL);
	persist_cleanup();
	cleanup_term();
	cleanup_testdir();
}

TEST(non_ephemeral_not_saved)
{
	char restoredir[PATH_MAX];

	setup_term(8, 3);
	persist_set_ephemeral(0);

	setup_testdir();
	persist_init(99969);
	persist_save();

	snprintf(restoredir, sizeof(restoredir), "%s/restore", testdir);
	{
		char cmd[PATH_MAX * 2 + 16];
		snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'",
				persist_get_dir(), restoredir);
		system(cmd);
	}

	/* Reset */
	cleanup_term();
	setup_term(8, 3);

	persist_restore(restoredir, NULL, NULL);
	ASSERT_EQ(0, persist_is_ephemeral());

	persist_cleanup();
	cleanup_term();
	cleanup_testdir();
}

TEST(altcmd_not_saved_without_altscreen)
{
	char restoredir[PATH_MAX];

	setup_term(8, 3);
	persist_set_altcmd("htop");
	term.mode = 0; /* altscreen NOT set */

	setup_testdir();
	persist_init(99989);
	persist_save();

	snprintf(restoredir, sizeof(restoredir), "%s/restore", testdir);
	{
		char cmd[PATH_MAX * 2 + 16];
		snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'",
				persist_get_dir(), restoredir);
		system(cmd);
	}

	/* Reset */
	cleanup_term();
	setup_term(8, 3);
	persist_set_altcmd(NULL);

	persist_restore(restoredir, NULL, NULL);
	ASSERT_STR_EQ("", persist_get_altcmd());

	persist_cleanup();
	cleanup_term();
	cleanup_testdir();
}

/* === Test suites === */

TEST_SUITE(cwd)
{
	RUN_TEST(cwd_set_and_get);
	RUN_TEST(cwd_overwrite);
	RUN_TEST(cwd_null_clears);
}

TEST_SUITE(save_restore)
{
	RUN_TEST(full_roundtrip);
	RUN_TEST(empty_history_roundtrip);
	RUN_TEST(cursor_y_restored);
	RUN_TEST(bad_magic_skipped);
}

TEST_SUITE(altcmd)
{
	RUN_TEST(altcmd_set_and_get);
	RUN_TEST(altcmd_null_clears);
	RUN_TEST(altcmd_saved_when_altscreen);
	RUN_TEST(altcmd_not_saved_without_altscreen);
}

TEST_SUITE(save_cmd)
{
	RUN_TEST(save_cmd_set_and_get);
	RUN_TEST(save_cmd_null_clears);
	RUN_TEST(save_cmd_overrides_altcmd);
	RUN_TEST(save_cmd_saved_without_altscreen);
	RUN_TEST(altcmd_used_when_no_save_cmd);
}

TEST_SUITE(ephemeral)
{
	RUN_TEST(ephemeral_set_and_get);
	RUN_TEST(ephemeral_saved_and_restored);
	RUN_TEST(non_ephemeral_not_saved);
}

TEST_SUITE(integration)
{
	RUN_TEST(register_sets_dwm_argv);
}

int
main(void)
{
	printf("st persist test suite\n");
	printf("========================================\n");

	RUN_SUITE(cwd);
	RUN_SUITE(save_restore);
	RUN_SUITE(altcmd);
	RUN_SUITE(save_cmd);
	RUN_SUITE(ephemeral);
	RUN_SUITE(integration);

	return test_summary();
}
