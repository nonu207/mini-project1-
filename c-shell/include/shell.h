#ifndef SHELL_H
#define SHELL_H

/* ── Project headers ──────────────────────────────────────────────── */
#include "change_dir.h"
#include "lexer.h"
#include "exec.h"
#include "seq.h"
#include "bg.h"
#include "prompt.h"
#include "reveal.h"
#include "peek.h"
#include "locate.h"
#include "activities.h"
#include "resume.h"
#include "ping.h"
#include "term.h"

/* ── Standard library headers ─────────────────────────────────────── */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#endif /* SHELL_H */
