/* CTM stream protocol ("CTMS"): video ES + cursor metadata over one TCP
 * stream. The Windows host listens; this app connects. Keep byte-identical
 * with CTM-USBIP/src/capture/ctm_stream_protocol.h. Little-endian, packed. */
#ifndef CTM_STREAM_PROTOCOL_H
#define CTM_STREAM_PROTOCOL_H

#include <stdint.h>

#pragma pack(push, 1)

#define CTMS_MAGIC 0x534D5443u /* 'CTMS' */
#define CTMS_PORT 48056

enum {
    CTMS_STREAM_INFO = 1,  /* CtmsStreamInfo (on connect + every encoder reconfigure) */
    CTMS_VIDEO_FRAME = 2,  /* Annex-B ES; hdr.pts=t0 present, hdr.t1=encoded; flags bit0=IDR */
    CTMS_CURSOR_POS = 3,   /* CtmsCursorPos (sent on change) */
    CTMS_CURSOR_SHAPE = 4, /* CtmsCursorShape + RGBA8 pixels (when shape changes) */
    CTMS_PING = 5,         /* client->host: CtmsPing (clock sync) */
    CTMS_PONG = 6,         /* host->client: CtmsPong (echo + host clock) */
    CTMS_AUDIO_FRAME = 7,  /* interleaved S16LE PCM; hdr.pts = capture time (us) */
};

#define CTMS_FLAG_IDR 0x0001

/* All host timestamps are microseconds since the host stream epoch. */
typedef struct CtmsHdr {
    uint32_t magic; /* CTMS_MAGIC */
    uint16_t type;
    uint16_t flags;
    uint64_t pts;        /* video: t0 = Windows present time (us); cursor: send time */
    uint64_t tEnc;       /* video: encode-START time (us); enc = t1 - tEnc */
    uint64_t t1;         /* video: encode-done time (us); else 0 */
    uint64_t tSend;      /* video: send-departure time (us): first byte to socket */
    uint32_t payloadLen; /* bytes following this header */
} CtmsHdr;

typedef struct CtmsPing {
    uint64_t clientUs;
} CtmsPing;
typedef struct CtmsPong {
    uint64_t clientUs;
    uint64_t hostUs;
} CtmsPong;

typedef struct CtmsStreamInfo {
    uint16_t codec; /* 1 = HEVC, 2 = AV1 */
    uint16_t width, height;
    uint16_t fps;
    uint8_t isHDR; /* 1 = PQ BT.2020 10-bit content */
    uint8_t _pad;
    /* HDR10 statics (valid when isHDR) */
    float primaries[8]; /* rx,ry,gx,gy,bx,by,wx,wy */
    float maxLum, minLum;
    float maxCLL, maxFALL;
    uint8_t hasAudio;      /* 1 = an audio stream follows (S16LE PCM) */
    uint8_t audioChannels; /* e.g. 2 */
    uint16_t _apad;
    uint32_t audioRate;    /* PCM sample rate, Hz (e.g. 48000) */
} CtmsStreamInfo;

typedef struct CtmsCursorPos {
    int32_t x, y; /* top-left of shape, capture-space pixels */
    uint8_t visible;
    uint8_t _pad[3];
} CtmsCursorPos;

typedef struct CtmsCursorShape {
    uint16_t width, height;
    int16_t hotX, hotY;
    /* followed by width*height*4 bytes RGBA8 */
} CtmsCursorShape;

#pragma pack(pop)

#endif
