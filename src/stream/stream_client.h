/* CTMS receiver: TCP client thread that connects to the Windows host, parses
 * the CTMS stream, feeds video ES to the NDL player and tracks the remote
 * cursor for the UI overlay. */
#ifndef CTM_STREAM_CLIENT_H
#define CTM_STREAM_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#define STREAM_CURSOR_MAX_DIM 256

typedef struct stream_cursor_state {
    int x, y; /* top-left, capture-space */
    bool visible;
    int width, height;
    /* RGBA8, width*height*4 bytes; valid when width > 0 */
    uint8_t rgba[STREAM_CURSOR_MAX_DIM * STREAM_CURSOR_MAX_DIM * 4];
    uint32_t shape_gen; /* bumped when the shape changes */
} stream_cursor_state;

typedef struct stream_stats {
    bool connected;
    bool video_loaded;
    int codec; /* 1 hevc, 2 av1 */
    int width, height;
    bool isHDR;
    uint64_t frames, bytes;
    /* lag of frame arrival vs the stream's own clock (pts), ms: how much
     * later than "live" we are relative to the first received frame.
     * Includes network + sender queueing; rises => falling behind. */
    double lagMs;
    /* timeline -- each term avoids cross-clock subtraction so none can go
     * negative. enc is host-clock only; net is half the round trip. */
    bool synced;      /* have a round-trip sample yet */
    double rttMs;     /* host<->TV network round trip (PING/PONG, min) -- control-path floor */
    double netMs;     /* real one-way frame transport = (arrival + offset) - tSend; <0 if not measured */
    double encMs;     /* enc = t1 - tEnc (real AMF encode; scales with res) */
    double hostMs;    /* present -> encoded = t1 - t0 (full host pipeline) */
    double submitMs;  /* t3 - t2 (arrival -> NDL submit; tiny, NDL is async) */
    double decMs;     /* decode/display buffering: renderBuf x measured frame interval (Moonlight/ss4s NDL) */
    double g2gMs;     /* present->display, t0-anchored: (arrival+offset - t0) + dec; <=0 until offset known */
} stream_stats;

bool stream_client_start(const char *host, int port, const char *app_id);
void stream_client_stop(void);
void stream_client_get_stats(stream_stats *out);
/* Like get_stats, but enc/host/net are averaged over all frames since the last
 * flush (then reset). The live overlay calls this once per second. */
void stream_client_flush_stats(stream_stats *out);
/* Copies the cursor state; returns true if the shape changed since *last_gen
 * (and updates *last_gen). Position/visibility are always current. */
bool stream_client_get_cursor(stream_cursor_state *out, uint32_t *last_gen);

#endif
