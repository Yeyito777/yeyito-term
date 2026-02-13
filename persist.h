/* See LICENSE for license details. */

#ifndef PERSIST_H
#define PERSIST_H

#include <sys/types.h>

void persist_init(pid_t pid);
void persist_register(void);
void persist_save(void);
void persist_restore(const char *dir, int *out_col, int *out_row);
void persist_cleanup(void);
int persist_active(void);
void persist_set_cwd(const char *cwd);
const char *persist_get_cwd(void);
void persist_set_altcmd(const char *cmd);
const char *persist_get_altcmd(void);
const char *persist_get_dir(void);
const char *persist_find_orphan(void);

#endif /* PERSIST_H */
