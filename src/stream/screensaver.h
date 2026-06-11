/* Screensaver veto while streaming: subscribe to tvpower's
 * registerScreenSaverRequest and NACK each request. */
#ifndef CTM_SCREENSAVER_H
#define CTM_SCREENSAVER_H

void screensaver_block_start(void);
void screensaver_block_stop(void);

#endif
