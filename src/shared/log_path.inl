/* Where the core's log files go, and the path buffer each thread gets.
 *
 * ⭐ $HOME/logs, created if missing, inside the app's own install directory
 * (ctm_state.h says why); /tmp when HOME is unset (desktop builds, the tests).
 *
 * ⛔⛔ ONE BUFFER PER THREAD (code review, 2026-10-05). It was one buffer for the
 * whole process, so two threads opening different logs at once could each read
 * the other's path, and a line landed in the wrong file. And the folder was
 * marked found BEFORE its name was written, so a second thread arriving in that
 * moment read an empty name. pthread_once settles the folder once, whoever
 * asks first, and nobody reads it before it is written.
 *
 * ⓘ Its own file so that tests/test_log_path.c can include it as it is.
 */

#ifndef LOG_PATH_INL
#define LOG_PATH_INL

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

static char s_log_dir[PATH_MAX];
static pthread_once_t s_log_dir_once = PTHREAD_ONCE_INIT;

static void log_dir_resolve(void)
{
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        snprintf(s_log_dir, sizeof(s_log_dir), "%s/logs", home);
        /* EEXIST is success for our purposes; anything else falls back. */
        if (mkdir(s_log_dir, 0755) == 0 || errno == EEXIST) {
            return;
        }
    }
    snprintf(s_log_dir, sizeof(s_log_dir), "/tmp");
}

static const char *ctm_log_dir(void)
{
    pthread_once(&s_log_dir_once, log_dir_resolve);
    return s_log_dir;
}

/* ⚠️ The answer lives in this thread's buffer until this thread asks again. */
const char *ctm_log_path(const char *name)
{
    static __thread char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", ctm_log_dir(), name ? name : "ctm.log");
    return path;
}

#endif /* LOG_PATH_INL */
