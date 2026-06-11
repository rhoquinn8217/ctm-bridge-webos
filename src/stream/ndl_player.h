/* Direct NDL video player: dlopen libNDL_directmedia at runtime (the lib only
 * exists on the TV), feed Annex-B ES straight to the hardware decoder. */
#ifndef CTM_NDL_PLAYER_H
#define CTM_NDL_PLAYER_H

#include <stdbool.h>
#include "ctm_stream_protocol.h"

bool ndl_player_init(const char *app_id); /* dlopen + NDL_DirectMediaInit */
bool ndl_player_load(const CtmsStreamInfo *info);
bool ndl_player_loaded(void);
void ndl_player_feed(const void *es, unsigned size, long long pts_us);
void ndl_player_unload(void);
void ndl_player_quit(void);
const char *ndl_player_error(void); /* last error string, "" if none */

#endif
