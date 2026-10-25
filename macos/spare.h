/* See LICENSE for license details. */
/*
 * Warm spare: an st that has finished its expensive startup (AppKit, Metal,
 * fonts, window) but has neither shown its window nor started a shell.
 * st-aerospace-launch claims it from the single slot <runtime>/spare (or a
 * private slot, --spare-slot, when it boots one for itself) and sends a
 * "spawn" request; the spare then starts the shell in the requested directory
 * and shows itself, instead of a cold launch costing ~0.5 s.
 */

#ifndef ST_MACOS_SPARE_H
#define ST_MACOS_SPARE_H

#include <stddef.h>
#include <sys/types.h>

#define SPARE_MAX_ENV  32
#define SPARE_MAX_ARGS 128

typedef struct {
	char *cwd;                        /* NULL: unchanged */
	char *title;                      /* NULL: default title */
	char *env[SPARE_MAX_ENV];         /* "NAME=VALUE" */
	int nenv;
	char *argv[SPARE_MAX_ARGS + 1];   /* NULL-terminated; 0 args: the shell */
	int argc;
	char *storage;
} SpareRequest;

/* The "spawn" payload is a sequence of NUL-terminated fields: cwd=DIR,
 * title=TITLE, env=NAME=VALUE (repeatable) and arg=WORD (repeatable, in
 * order; like st -e). Unknown keys are ignored so launchers can grow.
 * Returns 0 on success and -1 for malformed or oversized requests. */
int spare_parse_request(const char *payload, size_t length,
    SpareRequest *request);
void spare_free_request(SpareRequest *request);

/* "<inode>:<mtime>" of path. A launcher only claims a spare whose stamp
 * matches the installed binary, so a reinstall never serves stale code. */
int spare_exe_stamp(const char *path, char *buffer, size_t size);

/* $HOME/.runtime/st, created if missing. */
int spare_runtime_dir(char *buffer, size_t size);

/* Publish "<pid> <window-id> <stamp>" as the slot file. Returns 1 when
 * published, 0 when another live spare already holds the slot (the caller
 * is redundant) and -1 on error. */
int spare_publish(const char *slot, pid_t pid, unsigned long windowid,
    const char *stamp);

/* Whether the slot currently names pid. */
int spare_holds_slot(const char *slot, pid_t pid);

/* Remove the slot if it still names pid. */
void spare_withdraw(const char *slot, pid_t pid);

#endif /* ST_MACOS_SPARE_H */
