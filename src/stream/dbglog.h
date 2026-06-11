/* Crash-trail logger: appends to a file the app jail can write (the in-app UI
 * log is lost when the process dies, and webOS native stdout is detached under
 * SAM). fopen/fclose per line so the last line survives a hard crash. */
#ifndef CTM_DBGLOG_H
#define CTM_DBGLOG_H

#include <stdarg.h>
#include <stdio.h>

static inline void ctm_dbg(const char *fmt, ...)
{
    FILE *f = fopen("/tmp/ctmbridge-live.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

#endif
