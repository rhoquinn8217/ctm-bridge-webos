#include "stream_client.h"
#include "ctm_stream_protocol.h"
#include "ndl_player.h"
#include "dbglog.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static pthread_t g_thread;
static volatile bool g_run;
static int g_sock = -1;
static char g_host[64];
static int g_port;
static char g_app_id[64];

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static stream_cursor_state g_cursor;
static stream_stats g_stats;

static bool recv_all(int s, void *p, size_t len)
{
    char *b = (char *)p;
    while (len > 0) {
        ssize_t n = recv(s, b, len, 0);
        if (n <= 0) return false;
        b += n;
        len -= (size_t)n;
    }
    return true;
}

static void handle_info(const CtmsStreamInfo *si)
{
    ctm_dbg("rx: STREAM_INFO codec=%u %ux%u hdr=%u", si->codec, si->width, si->height, si->isHDR);
    bool ok = ndl_player_load(si);
    pthread_mutex_lock(&g_mtx);
    g_stats.video_loaded = ok;
    g_stats.codec = si->codec;
    g_stats.width = si->width;
    g_stats.height = si->height;
    g_stats.isHDR = si->isHDR != 0;
    pthread_mutex_unlock(&g_mtx);
    if (!ok) fprintf(stderr, "[stream] NDL load failed: %s\n", ndl_player_error());
}

static void read_loop(int s)
{
    uint8_t *payload = NULL;
    size_t cap = 0;
    while (g_run) {
        CtmsHdr h;
        if (!recv_all(s, &h, sizeof(h)) || h.magic != CTMS_MAGIC) break;
        if (h.payloadLen > 32u * 1024u * 1024u) break; /* sanity */
        if (h.payloadLen > cap) {
            cap = h.payloadLen;
            payload = (uint8_t *)realloc(payload, cap);
            if (!payload) break;
        }
        if (h.payloadLen && !recv_all(s, payload, h.payloadLen)) break;

        switch (h.type) {
        case CTMS_STREAM_INFO:
            if (h.payloadLen >= sizeof(CtmsStreamInfo))
                handle_info((const CtmsStreamInfo *)payload);
            break;
        case CTMS_VIDEO_FRAME:
            /* pts: encoder frame index -> microseconds at 60 fps */
            ndl_player_feed(payload, h.payloadLen, (long long)h.pts * 1000000ll / 60ll);
            pthread_mutex_lock(&g_mtx);
            g_stats.frames++;
            g_stats.bytes += h.payloadLen;
            pthread_mutex_unlock(&g_mtx);
            break;
        case CTMS_CURSOR_POS:
            if (h.payloadLen >= sizeof(CtmsCursorPos)) {
                const CtmsCursorPos *p = (const CtmsCursorPos *)payload;
                pthread_mutex_lock(&g_mtx);
                g_cursor.x = p->x;
                g_cursor.y = p->y;
                g_cursor.visible = p->visible != 0;
                pthread_mutex_unlock(&g_mtx);
            }
            break;
        case CTMS_CURSOR_SHAPE:
            if (h.payloadLen >= sizeof(CtmsCursorShape)) {
                CtmsCursorShape sh;
                memcpy(&sh, payload, sizeof(sh));
                const size_t need = (size_t)sh.width * sh.height * 4;
                if (sh.width <= STREAM_CURSOR_MAX_DIM && sh.height <= STREAM_CURSOR_MAX_DIM &&
                    h.payloadLen >= sizeof(sh) + need) {
                    pthread_mutex_lock(&g_mtx);
                    g_cursor.width = sh.width;
                    g_cursor.height = sh.height;
                    memcpy(g_cursor.rgba, payload + sizeof(sh), need);
                    g_cursor.shape_gen++;
                    pthread_mutex_unlock(&g_mtx);
                }
            }
            break;
        default:
            break;
        }
    }
    free(payload);
}

static void *run(void *arg)
{
    (void)arg;
    while (g_run) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)g_port);
        if (inet_pton(AF_INET, g_host, &a.sin_addr) != 1) {
            fprintf(stderr, "[stream] bad host %s\n", g_host);
            close(s);
            break;
        }
        if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
            close(s);
            for (int i = 0; i < 10 && g_run; i++) usleep(100 * 1000);
            continue;
        }
        int nd = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
        g_sock = s;
        ctm_dbg("rx: connected to %s:%d", g_host, g_port);
        pthread_mutex_lock(&g_mtx);
        g_stats.connected = true;
        pthread_mutex_unlock(&g_mtx);

        read_loop(s);

        g_sock = -1;
        close(s);
        ndl_player_unload();
        pthread_mutex_lock(&g_mtx);
        g_stats.connected = false;
        g_stats.video_loaded = false;
        pthread_mutex_unlock(&g_mtx);
    }
    return NULL;
}

bool stream_client_start(const char *host, int port, const char *app_id)
{
    if (g_run) return true;
    snprintf(g_host, sizeof(g_host), "%s", host);
    snprintf(g_app_id, sizeof(g_app_id), "%s", app_id);
    g_port = port;
    ctm_dbg("=== ui_live start: host=%s port=%d app=%s ===", g_host, g_port, g_app_id);
    if (!ndl_player_init(g_app_id)) {
        fprintf(stderr, "[stream] NDL init failed: %s\n", ndl_player_error());
        return false;
    }
    memset(&g_stats, 0, sizeof(g_stats));
    g_run = true;
    if (pthread_create(&g_thread, NULL, run, NULL) != 0) {
        g_run = false;
        return false;
    }
    return true;
}

void stream_client_stop(void)
{
    if (!g_run) return;
    g_run = false;
    if (g_sock >= 0) shutdown(g_sock, SHUT_RDWR); /* unblock recv */
    pthread_join(g_thread, NULL);
    ndl_player_unload();
}

void stream_client_get_stats(stream_stats *out)
{
    pthread_mutex_lock(&g_mtx);
    *out = g_stats;
    pthread_mutex_unlock(&g_mtx);
}

bool stream_client_get_cursor(stream_cursor_state *out, uint32_t *last_gen)
{
    pthread_mutex_lock(&g_mtx);
    *out = g_cursor;
    pthread_mutex_unlock(&g_mtx);
    const bool changed = out->shape_gen != *last_gen;
    *last_gen = out->shape_gen;
    return changed;
}
