#include "ui_live.h"
#include "ui_common.h"

#include "stream_client.h"
#include "screensaver.h"
#include "ndl_player.h"
#include "ctm_stream_protocol.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *g_live_scr;
static lv_obj_t *g_prev_scr;
static lv_obj_t *g_live_status;
static lv_obj_t *g_cursor_img;
static lv_timer_t *g_live_timer;
static uint32_t g_shape_gen;

/* LVGL image backing store for the cursor sprite (BGRA for TRUE_COLOR_ALPHA
 * at 32-bit color depth). */
static uint8_t g_cursor_px[STREAM_CURSOR_MAX_DIM * STREAM_CURSOR_MAX_DIM * 4];
static lv_img_dsc_t g_cursor_dsc;

bool ui_live_active(void) { return g_live_scr != NULL; }

static uint64_t g_prev_frames, g_prev_bytes;
static uint32_t g_prev_ms;

static void live_tick(lv_timer_t *t)
{
    (void)t;
    if (!g_live_scr) return;

    stream_stats st;
    stream_client_get_stats(&st);

    /* refresh the text twice a second; rates from the deltas */
    const uint32_t now = lv_tick_get();
    if (now - g_prev_ms >= 500) {
        const double dt = (now - g_prev_ms) / 1000.0;
        const double fps = (double)(st.frames - g_prev_frames) / dt;
        const double mbps = (double)(st.bytes - g_prev_bytes) * 8.0 / 1e6 / dt;
        g_prev_frames = st.frames; g_prev_bytes = st.bytes; g_prev_ms = now;
        char buf[224];
        snprintf(buf, sizeof(buf),
                 "%s | %s %dx%d %s | %.1f fps  %.1f Mb/s  lag %+.0f ms  buf %d | f=%llu | long BACK: exit",
                 st.connected ? "connected" : "connecting...",
                 st.codec == 2 ? "AV1" : "HEVC", st.width, st.height,
                 st.isHDR ? "HDR" : "SDR",
                 fps, mbps, st.lagMs, ndl_player_render_buffer(),
                 (unsigned long long)st.frames);
        lv_label_set_text(g_live_status, buf);
    }

    stream_cursor_state cur;
    const bool shape_changed = stream_client_get_cursor(&cur, &g_shape_gen);
    if (shape_changed && cur.width > 0) {
        const int n = cur.width * cur.height;
        for (int i = 0; i < n; i++) {       /* RGBA -> BGRA */
            g_cursor_px[i * 4 + 0] = cur.rgba[i * 4 + 2];
            g_cursor_px[i * 4 + 1] = cur.rgba[i * 4 + 1];
            g_cursor_px[i * 4 + 2] = cur.rgba[i * 4 + 0];
            g_cursor_px[i * 4 + 3] = cur.rgba[i * 4 + 3];
        }
        memset(&g_cursor_dsc, 0, sizeof(g_cursor_dsc));
        g_cursor_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        g_cursor_dsc.header.w = cur.width;
        g_cursor_dsc.header.h = cur.height;
        g_cursor_dsc.data_size = (uint32_t)n * 4;
        g_cursor_dsc.data = g_cursor_px;
        lv_img_set_src(g_cursor_img, &g_cursor_dsc);
    }
    if (cur.visible && cur.width > 0 && st.width > 0) {
        /* video plane is fullscreen: map capture-space -> UI-space */
        const int ui_w = lv_obj_get_width(g_live_scr);
        const int ui_h = lv_obj_get_height(g_live_scr);
        const int x = cur.x * ui_w / st.width;
        const int y = cur.y * ui_h / st.height;
        lv_img_set_zoom(g_cursor_img, 256 * ui_w / st.width);
        lv_obj_set_pos(g_cursor_img, x, y);
        lv_obj_clear_flag(g_cursor_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_cursor_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_live_open(void)
{
    if (g_live_scr) return;
    if (!stream_client_start(g_agent_host, CTMS_PORT, "com.local.ctmbridge")) {
        log_append("live: start failed: %s", ndl_player_error());
        return;
    }
    screensaver_block_start();

    g_prev_scr = lv_scr_act();
    g_live_scr = lv_obj_create(NULL);
    /* fully transparent screen: the NDL video plane sits below the app surface */
    lv_obj_set_style_bg_opa(g_live_scr, LV_OPA_TRANSP, 0);

    g_live_status = lv_label_create(g_live_scr);
    lv_obj_set_style_text_color(g_live_status, lv_color_hex(0xd0d8de), 0);
    lv_obj_set_style_bg_color(g_live_status, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_live_status, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(g_live_status, 6, 0);
    lv_obj_set_pos(g_live_status, 12, 12);
    lv_label_set_text(g_live_status, "connecting...");

    g_cursor_img = lv_img_create(g_live_scr);
    lv_obj_add_flag(g_cursor_img, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_size_mode(g_cursor_img, LV_IMG_SIZE_MODE_REAL);

    g_shape_gen = 0;
    g_prev_frames = g_prev_bytes = 0;
    g_prev_ms = lv_tick_get();
    g_live_timer = lv_timer_create(live_tick, 16, NULL);
    lv_scr_load(g_live_scr);
    log_append("live: started");
}

void ui_live_close(void)
{
    if (!g_live_scr) return;
    stream_client_stop();
    screensaver_block_stop();
    lv_timer_del(g_live_timer);
    g_live_timer = NULL;
    lv_scr_load(g_prev_scr);
    lv_obj_del(g_live_scr);
    g_live_scr = NULL;
    g_live_status = NULL;
    g_cursor_img = NULL;
    log_append("live: stopped");
}
