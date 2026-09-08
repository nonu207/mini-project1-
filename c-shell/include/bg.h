#ifndef BG_H
#define BG_H

#include <sys/types.h>

#include "lexer.h"
#include "change_dir.h"

typedef struct {
    pid_t pid;
    int   job_number;
    char  command_name[256];
} BgJob;

void init_bg(void);
void register_bg_job(pid_t pid, const char *name);
void check_bg_jobs(void);
int  run_bg_group(Token *start, HopEntry *db, int *db_size);

#endif /* BG_H */
