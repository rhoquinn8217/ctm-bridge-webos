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
} stream_stats;

bool stream_client_start(const char *host, int port, const char *app_id);
void stream_client_stop(void);
void stream_client_get_stats(stream_stats *out);
/* Copies the cursor state; returns true if the shape changed since *last_gen
 * (and updates *last_gen). Position/visibility are always current. */
bool stream_client_get_cursor(stream_cursor_state *out, uint32_t *last_gen);

#endif
