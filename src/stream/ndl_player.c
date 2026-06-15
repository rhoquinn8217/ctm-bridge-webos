#define NDL_DIRECTMEDIA_API_VERSION 2

#include "ndl_player.h"
#include "dbglog.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Real webOS NDL DirectMedia (webOS 5+): linked via the SDK's NDL_directmedia
 * pkg-config, same as ss4s's webos5 backend. The lib (libNDL_directmedia.so.1)
 * is present on the TV. */
#include <NDL_directmedia_v2.h>

static bool g_inited;
static bool g_loaded;
static char g_err[256];

const char *ndl_player_error(void) { return g_err; }
bool ndl_player_loaded(void) { return g_loaded; }

/* Probe (option c): NDL hands us decode/display events through this callback.
 * We don't yet know which event ids carry a display timestamp, so log every
 * one to /tmp/ctmbridge-live.log and inspect. If any event fires per displayed
 * frame with a usable clock, that becomes the real t3 (shown) we can't get
 * otherwise -- NDL exposes no display-time query. Throttled so a per-frame
 * event can't flood the log. */
static void load_cb(int type, long long numValue, const char *strValue)
{
    static long long n;
    static int last_type = -1;
    /* always log a type the first time we see it, then 1-in-120 after */
    if (type != last_type || (n % 120) == 0) {
        ctm_dbg("ndl_cb: type=%d num=%lld str=%s (#%lld)",
                type, numValue, strValue ? strValue : "(null)", n);
        last_type = type;
    }
    n++;
}

bool ndl_player_init(const char *app_id)
{
    if (g_inited) return true;
    g_err[0] = '\0';
    /* Prefer OUR app id: under SAM getenv("APPID") is com.local.ctmbridge, but
     * under a dev SSH session it is com.palm.devmode.openssh, which NDL rejects. */
    const char *id = (app_id && *app_id) ? app_id : getenv("APPID");
    ctm_dbg("ndl_init: APPID=%s id=%s -> NDL_DirectMediaInit", getenv("APPID"), id ? id : "(null)");
    int rc = NDL_DirectMediaInit(id, NULL);
    ctm_dbg("ndl_init: NDL_DirectMediaInit rc=%d", rc);
    if (rc != 0) {
        snprintf(g_err, sizeof(g_err), "NDL_DirectMediaInit(%s) = %d: %s",
                 id ? id : "(null)", rc, NDL_DirectMediaGetError());
        return false;
    }
    g_inited = true;
    return true;
}

bool ndl_player_load(const CtmsStreamInfo *info)
{
    if (!g_inited) { snprintf(g_err, sizeof(g_err), "not inited"); return false; }
    if (g_loaded) ndl_player_unload();

    NDL_DIRECTMEDIA_DATA_INFO_T di;
    memset(&di, 0, sizeof(di));
    di.video.width = info->width;
    di.video.height = info->height;
    di.video.type = (info->codec == 2) ? NDL_VIDEO_TYPE_AV1 : NDL_VIDEO_TYPE_H265;
    di.video.unknown1 = info->fps; /* framerate hint (unnamed in the header) */
    if (info->hasAudio) {
        di.audio.opus.type = NDL_AUDIO_TYPE_OPUS;
        di.audio.opus.channels = info->audioChannels ? info->audioChannels : 2;
        di.audio.opus.sampleRate = (info->audioRate ? info->audioRate : 48000) / 1000.0; /* kHz */
        di.audio.opus.streamHeader = NULL; /* NDL infers from packets (like ss4s) */
    } else {
        di.audio.type = 0;         /* video only */
    }
    ctm_dbg("ndl_load: codec=%u %dx%d hdr=%d -> NDL_DirectMediaLoad", di.video.type, di.video.width, di.video.height, info->isHDR);
    int rc = NDL_DirectMediaLoad(&di, load_cb);
    ctm_dbg("ndl_load: NDL_DirectMediaLoad rc=%d", rc);
    if (rc != 0) {
        snprintf(g_err, sizeof(g_err), "NDL_DirectMediaLoad = %d: %s", rc, NDL_DirectMediaGetError());
        return false;
    }
    g_loaded = true;

    /* SetHDRInfo is gated OFF by default: the SDK header declares a 12-int
     * signature but the TV's lib may want the newer struct form (ABI skew ->
     * SIGBUS). HDR mastering data is also carried in the bitstream SEI, so the
     * decoder can pick it up without this call. Set CTM_NDL_SETHDR=1 to test. */
    if (info->isHDR && getenv("CTM_NDL_SETHDR")) {
        const float *p = info->primaries;
        ctm_dbg("ndl_load: calling NDL_DirectVideoSetHDRInfo");
        rc = NDL_DirectVideoSetHDRInfo(
            (int)(p[2] * 50000.0f), (int)(p[3] * 50000.0f), /* G */
            (int)(p[4] * 50000.0f), (int)(p[5] * 50000.0f), /* B */
            (int)(p[0] * 50000.0f), (int)(p[1] * 50000.0f), /* R */
            (int)(p[6] * 50000.0f), (int)(p[7] * 50000.0f), /* white */
            (int)(info->maxLum * 10000.0f), (int)(info->minLum * 10000.0f),
            (int)info->maxCLL, (int)info->maxFALL);
        ctm_dbg("ndl_load: NDL_DirectVideoSetHDRInfo rc=%d", rc);
    } else if (info->isHDR) {
        ctm_dbg("ndl_load: SetHDRInfo skipped (HDR via bitstream SEI)");
    }
    return true;
}

void ndl_player_feed(const void *es, unsigned size, long long pts_us)
{
    if (!g_loaded) return;
    static int first = 1;
    if (first) { ctm_dbg("ndl_feed: first frame %u bytes pts=%lld", size, pts_us); first = 0; }
    int rc = NDL_DirectVideoPlay((void *)es, size, pts_us);
    if (rc != 0)
        snprintf(g_err, sizeof(g_err), "NDL_DirectVideoPlay = %d: %s", rc, NDL_DirectMediaGetError());
}

int ndl_player_render_buffer(void)
{
    if (!g_loaded) return -1;
    int len = -1;
    if (NDL_DirectVideoGetRenderBufferLength(&len) != 0) return -1;
    return len;
}

void ndl_player_feed_audio(const void *opus, unsigned size, long long pts_us)
{
    if (!g_loaded) return;
    int rc = NDL_DirectAudioPlay((void *)opus, size, pts_us);
    if (rc != 0)
        snprintf(g_err, sizeof(g_err), "NDL_DirectAudioPlay = %d: %s", rc, NDL_DirectMediaGetError());
}

void ndl_player_unload(void)
{
    if (g_loaded) { NDL_DirectMediaUnload(); g_loaded = false; }
}

void ndl_player_quit(void)
{
    ndl_player_unload();
    if (g_inited) { NDL_DirectMediaQuit(); g_inited = false; }
}
