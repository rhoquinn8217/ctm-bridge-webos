/* DualSense (DS5) detail panel. Moved verbatim out of lvgl_ui.c; de-static'd,
 * prototyped in ui_common.h. */

#define _GNU_SOURCE

#include "ui_common.h"

#include <stdio.h>
void build_ds5_detail(lv_obj_t *parent, const logical_device_t *item,
                             tv_bridge_worker_settings_t *settings, int width)
{
    ui_device_settings_t *record = ui_record_for_item(item);
    int y = 0;
    detail_label(parent, "Sony DS5 Controller", 0, y, width, &lv_font_montserrat_20, lv_color_hex(0xf5f8fa));
    y += 38;
    y = detail_add_identity(parent, item, y, width);
    y = detail_add_audio_modes(parent, settings, y, width);
    y = detail_add_slider(parent, "Headset volume", record ? (int)record->headset_volume_percent : 0x4d,
                          0, 100, y, width, 3);
    y = detail_add_slider(parent, "Speaker volume", record ? (int)record->speaker_volume_percent : 0x41,
                          0, 100, y, width, 4);
    /* ⭐ FLOOR OPENED TO 0 (was 20), to find out what 0 means.
     *
     * ⚠️ NOBODY KNOWS WHAT THIS VALUE DOES. It is written into bytes 3..7 of
     * block 0x91 of the 398-byte 0x36 audio output report -- five identical
     * bytes, which is not the shape of a single field. The 20 was a floor
     * inherited with the slider, never explained, and the default has drifted
     * 96 -> 48 -> 60 with no reasoning recorded at any step.
     *
     * ⛔ No public reverse-engineering documents this block. The community has
     * mapped the flat 78-byte 0x31 report thoroughly and stops there.
     *
     * ⚠️ WATCH FOR: 0x91 also carries the AUDIO SEQUENCE, and the sequence
     * incrementing is what makes the speaker play at all. A 0 here may silence
     * speaker audio rather than reduce its latency. */
    y = detail_add_slider(parent, "Latency", (int)settings->latency_ms, 0, 255, y, width, 1);
    y = detail_add_slider(parent, "Haptics gain", (int)settings->haptics_gain_centi, 0, 500, y, width, 2);
}

