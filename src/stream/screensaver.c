#include "screensaver.h"

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Subscribe via luna-send-pub (the app's established Luna mechanism); when the
 * TV announces an imminent screensaver ("Active"), NACK it with the request's
 * timestamp so the screen stays on while the stream is showing. */

static pthread_t g_thread;
static volatile bool g_run;
static FILE *g_sub;

static void *run(void *arg)
{
    (void)arg;
    g_sub = popen(
        "luna-send-pub -i "
        "luna://com.webos.service.tvpower/power/registerScreenSaverRequest "
        "'{\"subscribe\":true,\"clientName\":\"com.local.ctmbridge\"}' 2>/dev/null",
        "r");
    if (!g_sub) return NULL;
    char line[1024];
    while (g_run && fgets(line, sizeof(line), g_sub)) {
        if (!strstr(line, "Active")) continue;
        const char *ts = strstr(line, "\"timestamp\":");
        char tsbuf[64] = "0";
        if (ts) {
            ts += 12;
            while (*ts == ' ' || *ts == '"') ts++;
            size_t n = 0;
            while (n < sizeof(tsbuf) - 1 && (ts[n] == '-' || (ts[n] >= '0' && ts[n] <= '9')))
                { tsbuf[n] = ts[n]; n++; }
            tsbuf[n] = '\0';
        }
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "luna-send-pub -n 1 "
                 "luna://com.webos.service.tvpower/power/responseScreenSaverRequest "
                 "'{\"clientName\":\"com.local.ctmbridge\",\"ack\":false,\"timestamp\":\"%s\"}' "
                 ">/dev/null 2>&1",
                 tsbuf);
        system(cmd);
    }
    return NULL;
}

void screensaver_block_start(void)
{
    if (g_run) return;
    g_run = true;
    if (pthread_create(&g_thread, NULL, run, NULL) != 0) g_run = false;
}

void screensaver_block_stop(void)
{
    if (!g_run) return;
    g_run = false;
    if (g_sub) pclose(g_sub); /* closes the pipe; fgets unblocks */
    g_sub = NULL;
    pthread_join(g_thread, NULL);
}
