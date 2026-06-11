/* Live view: fullscreen-transparent LVGL screen so the NDL video plane shows
 * through, with the remote cursor + a small status line drawn on top. */
#ifndef CTM_UI_LIVE_H
#define CTM_UI_LIVE_H

#include <stdbool.h>

void ui_live_open(void);  /* start stream client + screensaver veto, show live screen */
void ui_live_close(void); /* stop everything, return to the controller screen */
bool ui_live_active(void);

#endif
