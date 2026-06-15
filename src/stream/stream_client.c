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
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static pthread_t g_thread;
static pthread_t g_ping_thread;
static volatile bool g_run;
static int g_sock = -1;
static char g_host[64];
static int g_port;
static char g_app_id[64];

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static stream_cursor_state g_cursor;
static stream_stats g_stats;
static double g_t0;        /* local ms at first video frame */
static int64_t g_pts0 = -1; /* its pts (us) */

/* round-trip estimate (min sample); rtt/2 is the control-path transport floor */
static double g_best_rtt_us = 1e18;
static bool g_synced;
/* host_clock - TV_clock, taken NTP-style on the min-RTT PONG -> lets us turn the
 * host send-departure stamp (tSend) into real one-way frame transport. */
static int64_t g_offset_us;
static bool g_have_offset;
/* per-frame accumulators for the overlay window (averaged + reset on flush) */
static double g_acc_enc, g_acc_host, g_acc_net, g_acc_dec, g_acc_g2g;
static int g_acc_n, g_acc_net_n, g_acc_dec_n, g_acc_g2g_n;
static uint64_t g_last_frame_us;   /* previous frame's feed-done time, for the inter-frame interval */

static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static uint64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

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
        case CTMS_VIDEO_FRAME: {
            /* pts (t0) = present time, h.t1 = encode-done, both host-clock us.
             * t2 = arrival (now), t3 = handed to the display. dec = t3 - t2 is
             * TV-clock only, so it can never go negative.
             * Diagnostic: /tmp/ctm-nofeed skips the NDL feed (delivery-rate only). */
            const uint64_t t2 = mono_us();                       /* arrival */
            if (access("/tmp/ctm-nofeed", F_OK) != 0)
                ndl_player_feed(payload, h.payloadLen, (long long)h.pts);
            const uint64_t t3 = mono_us();                       /* handed to NDL (shown) */
            const double now = t2 / 1000.0;
            if (g_pts0 < 0) { g_pts0 = (int64_t)h.pts; g_t0 = now; }
            const double lag = (now - g_t0) - (double)((int64_t)h.pts - g_pts0) / 1000.0;
            const double encMs = (double)((int64_t)h.t1 - (int64_t)h.tEnc) / 1000.0;  /* real encode */
            const double hostMs = (double)((int64_t)h.t1 - (int64_t)h.pts) / 1000.0;  /* present->encoded */
            /* decode/display buffering, Moonlight/ss4s NDL method (NDL has no per-
             * frame display event -- only state callbacks): render-queue depth x the
             * MEASURED inter-frame interval. The 0.5 floor mirrors ss4s. */
            const int rbuf = ndl_player_render_buffer();
            double decMs = -1.0;
            if (g_last_frame_us > 0 && rbuf >= 0) {
                const double bufLen = rbuf > 0 ? (double)rbuf : 0.5;
                decMs = bufLen * (double)(t3 - g_last_frame_us) / 1000.0;
            }
            g_last_frame_us = t3;
            pthread_mutex_lock(&g_mtx);
            g_stats.frames++;
            g_stats.bytes += h.payloadLen;
            g_stats.lagMs = lag;
            g_stats.encMs = encMs;
            g_stats.hostMs = hostMs;
            g_stats.submitMs = (double)(t3 - t2) / 1000.0;                      /* NDL submit (async) */
            if (g_synced) { g_stats.synced = true; g_stats.rttMs = g_best_rtt_us / 1000.0; }
            /* real one-way transport: convert arrival (TV clock) into host clock via
             * the offset, then subtract the host send-departure stamp. */
            double netMs = -1.0;
            if (g_have_offset && h.tSend) {
                const int64_t trans_us = (int64_t)t2 + g_offset_us - (int64_t)h.tSend;
                netMs = (trans_us >= 0 && trans_us < 1000000) ? (double)trans_us / 1000.0 : -1.0;
            }
            g_stats.netMs = netMs;
            if (decMs >= 0.0) g_stats.decMs = decMs;
            /* g2g (present->display), t0-anchored like the host's d2-t0: arrival
             * converted to host clock minus present, plus the TV display buffering.
             * Telescopes through capture+encode+send-queue+transport+buffering -- no
             * gap (unlike host+net+dec, which drops the send-queue leg t1->tSend). */
            double g2gMs = -1.0;
            if (g_have_offset && decMs >= 0.0) {
                const int64_t arr_us = (int64_t)t2 + g_offset_us - (int64_t)h.pts;  /* present->arrival */
                if (arr_us >= 0 && arr_us < 2000000) g2gMs = (double)arr_us / 1000.0 + decMs;
            }
            if (g2gMs >= 0.0) g_stats.g2gMs = g2gMs;
            /* accumulate over the overlay window (flushed/averaged once per second) */
            g_acc_enc += encMs; g_acc_host += hostMs; g_acc_n++;
            if (decMs >= 0.0) { g_acc_dec += decMs; g_acc_dec_n++; }
            if (g2gMs >= 0.0) { g_acc_g2g += g2gMs; g_acc_g2g_n++; }
            if (netMs >= 0.0) { g_acc_net += netMs; g_acc_net_n++; }
            pthread_mutex_unlock(&g_mtx);
            break;
        }
        case CTMS_AUDIO_FRAME:
            ndl_player_feed_audio(payload, h.payloadLen, (long long)h.pts);
            break;
        case CTMS_PONG:
            if (h.payloadLen >= sizeof(CtmsPong)) {
                CtmsPong pong;
                memcpy(&pong, payload, sizeof(pong));
                const uint64_t tr = mono_us();                       /* PONG arrival (TV clock) */
                const double rtt = (double)tr - (double)pong.clientUs;
                if (rtt > 0 && rtt < g_best_rtt_us) {
                    /* cleanest round trip -> also take the clock offset here, NTP-style:
                     * offset = hostUs - (clientUs + tr)/2 = host_clock - TV_clock. The
                     * min-RTT sample minimises the midpoint error that made it go negative. */
                    const uint64_t mid = (pong.clientUs + tr) / 2;   /* TV-clock midpoint */
                    const int64_t off = (int64_t)pong.hostUs - (int64_t)mid;
                    pthread_mutex_lock(&g_mtx);
                    g_best_rtt_us = rtt;
                    g_offset_us = off;
                    g_have_offset = true;
                    g_synced = true;
                    pthread_mutex_unlock(&g_mtx);
                }
            }
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

/* Clock-sync pinger: ~4 Hz PING with the client clock; the host echoes it with
 * its clock in PONG. Runs on its own thread (the read loop is blocked on recv).
 * Writes concurrently with the reader -- fine for TCP. */
static void *ping_run(void *arg)
{
    (void)arg;
    while (g_run) {
        int s = g_sock;
        if (s >= 0) {
            CtmsHdr h;
            memset(&h, 0, sizeof(h));
            h.magic = CTMS_MAGIC;
            h.type = CTMS_PING;
            h.payloadLen = sizeof(CtmsPing);
            CtmsPing ping;
            ping.clientUs = mono_us();
            uint8_t buf[sizeof(CtmsHdr) + sizeof(CtmsPing)];
            memcpy(buf, &h, sizeof(h));
            memcpy(buf + sizeof(h), &ping, sizeof(ping));
            send(s, buf, sizeof(buf), MSG_NOSIGNAL); /* errors handled by reader */
        }
        for (int i = 0; i < 25 && g_run; i++) usleep(10 * 1000); /* ~250 ms */
    }
    return NULL;
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
        g_synced = false;            /* re-sync each connection (host epoch may differ) */
        g_best_rtt_us = 1e18;
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
    g_pts0 = -1;
    g_run = true;
    if (pthread_create(&g_thread, NULL, run, NULL) != 0) {
        g_run = false;
        return false;
    }
    pthread_create(&g_ping_thread, NULL, ping_run, NULL);
    return true;
}

void stream_client_stop(void)
{
    if (!g_run) return;
    g_run = false;
    if (g_sock >= 0) shutdown(g_sock, SHUT_RDWR); /* unblock recv */
    pthread_join(g_thread, NULL);
    pthread_join(g_ping_thread, NULL);
    ndl_player_unload();
}

void stream_client_get_stats(stream_stats *out)
{
    pthread_mutex_lock(&g_mtx);
    *out = g_stats;
    pthread_mutex_unlock(&g_mtx);
}

/* Snapshot + window-average the per-frame legs (enc/host/net) over all frames
 * since the last flush, then reset the accumulators. The overlay calls this once
 * per second so the shown numbers are averages, not the last frame's value. */
void stream_client_flush_stats(stream_stats *out)
{
    pthread_mutex_lock(&g_mtx);
    *out = g_stats;
    if (g_acc_n > 0) {
        out->encMs = g_acc_enc / g_acc_n;
        out->hostMs = g_acc_host / g_acc_n;
    }
    if (g_acc_dec_n > 0) out->decMs = g_acc_dec / g_acc_dec_n;
    if (g_acc_g2g_n > 0) out->g2gMs = g_acc_g2g / g_acc_g2g_n;
    out->netMs = (g_acc_net_n > 0) ? (g_acc_net / g_acc_net_n) : -1.0;
    g_acc_enc = g_acc_host = g_acc_net = g_acc_dec = g_acc_g2g = 0.0;
    g_acc_n = g_acc_net_n = g_acc_dec_n = g_acc_g2g_n = 0;
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
