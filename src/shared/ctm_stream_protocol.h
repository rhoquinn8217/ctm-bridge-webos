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
    CTMS_VIDEO_FRAME = 2,  /* Annex-B ES payload; hdr.pts = frame pts, flags bit0 = IDR */
    CTMS_CURSOR_POS = 3,   /* CtmsCursorPos (sent on change) */
    CTMS_CURSOR_SHAPE = 4, /* CtmsCursorShape + RGBA8 pixels (when shape changes) */
};

#define CTMS_FLAG_IDR 0x0001

typedef struct CtmsHdr {
    uint32_t magic; /* CTMS_MAGIC */
    uint16_t type;
    uint16_t flags;
    uint64_t pts;        /* video: frame pts; cursor: sender ms timestamp */
    uint32_t payloadLen; /* bytes following this header */
} CtmsHdr;

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
