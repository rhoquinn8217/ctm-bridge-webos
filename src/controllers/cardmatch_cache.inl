/* --- which card belongs to which controller, remembered ---------------------
 *
 * ⭐ SHARED BY BOTH PATHS, which is the whole point of it living here.
 *
 * Two places ask this question: a bridge, and a signal played with no session
 * behind it. They used to answer it separately, so the three seconds was paid
 * TWICE for the same controller -- refuse a plug, pay; start the listener and
 * bridge, pay again. With three controllers that is five probes where it
 * should be three.
 *
 * A card does not change while its controller stays plugged in, so whoever
 * answers first writes it down and everyone else reads it.
 *
 * ⛔ KEYED BY DEVICE NODE, AND NODES ARE REUSED. Unplug a controller, plug in
 * another, and it may land on the same path with a different card. Seen on
 * 2026-09-01: a controller released and bridged again heard its tone on the
 * OTHER controller and never showed its mute light -- the note was trusted,
 * so the probe never ran, and the remembered card had moved.
 *
 * ⭐ SO EVERY NOTE CARRIES THE CARD'S OWN IDENTITY, and is checked against it
 * before it is believed. /proc/asound/cardN/usbbus names the USB bus and
 * device number behind a card, and the device number changes every time a
 * device is enumerated -- unplug and replug, and it is a different number.
 * A note is therefore good exactly as long as the controller it was written
 * for stays plugged in, which is the one condition under which a card cannot
 * change. Readable without root: measured on C1 2026-08-08 from the app's own
 * shell, 002/014 and 002/012 for two controllers.
 *
 * ⓘ Why not forget on session teardown instead: a release does not
 * re-enumerate anything, so the card is still right when the same controller
 * bridges again -- and that is the case the cache exists for. Forgetting
 * there would charge the three seconds on every re-bridge for nothing.
 *
 * ⚠️ A card whose usbbus cannot be read is NOT remembered. On such a
 * television the cache costs a probe per bridge and can never be wrong, and
 * the log says so once per answer.
 *
 * ⓘ Not covered, and accepted: a hidraw node freed and reused WITHOUT the
 * controller re-enumerating. That needs a driver unbind and has not been seen
 * on a television.
 *
 * ⚠️ Every function here wants g_cardmatch_lock held, as before.
 *
 * Its own file so tests/test_cardmatch_cache.c can drive it against a scratch
 * directory: the path below is a macro for exactly that reason. */

#ifndef CARDMATCH_USBBUS_PATH
#define CARDMATCH_USBBUS_PATH "/proc/asound/card%d/usbbus"
#endif

#define CARDMATCH_CACHE_MAX   4
#define CARDMATCH_USBBUS_LEN  16

static struct {
    char node[64];
    int  card;
    char usbbus[CARDMATCH_USBBUS_LEN];   /* "bus/dev", e.g. "002/014" */
} g_cardmatch_cache[CARDMATCH_CACHE_MAX];
static int g_cardmatch_cached;

/* The card's USB bus and device number, as "bus/dev". Returns 1 and fills
 * `out`, or 0 with `out` empty when the file cannot be read or says nothing. */
static int cardmatch_read_usbbus(int card, char *out, size_t out_len)
{
    out[0] = '\0';
    char path[128];
    snprintf(path, sizeof(path), CARDMATCH_USBBUS_PATH, card);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[CARDMATCH_USBBUS_LEN];
    int got = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!got) return 0;
    line[strcspn(line, "\r\n")] = '\0';
    if (!line[0]) return 0;
    snprintf(out, out_len, "%s", line);
    return 1;
}

/* Drop note i. Order carries no meaning, so the last note moves into the gap. */
static void cardmatch_cache_drop(int i)
{
    if (i < 0 || i >= g_cardmatch_cached) return;
    --g_cardmatch_cached;
    if (i != g_cardmatch_cached) {
        g_cardmatch_cache[i] = g_cardmatch_cache[g_cardmatch_cached];
    }
    memset(&g_cardmatch_cache[g_cardmatch_cached], 0,
           sizeof(g_cardmatch_cache[0]));
}

/* Forget every note whose card is no longer the device it was written for.
 * Returns how many were dropped. Each one is logged, because a drop here is
 * the moment the 2026-09-01 fault would otherwise have happened. */
static int cardmatch_cache_prune(void)
{
    int dropped = 0;
    for (int i = 0; i < g_cardmatch_cached; ) {
        char now[CARDMATCH_USBBUS_LEN];
        cardmatch_read_usbbus(g_cardmatch_cache[i].card, now, sizeof(now));
        if (strcmp(now, g_cardmatch_cache[i].usbbus) == 0) { ++i; continue; }
        alsa_log("[cardmatch]", "forgot %s: card=%d was usb %s, now %s",
                 g_cardmatch_cache[i].node, g_cardmatch_cache[i].card,
                 g_cardmatch_cache[i].usbbus, now[0] ? now : "gone");
        cardmatch_cache_drop(i);
        ++dropped;
    }
    return dropped;
}

/* The remembered card for a node, or -1. Stale notes are forgotten on the
 * way, so an elimination that runs after this sees only notes that still
 * hold. */
static int cardmatch_cache_get(const char *node)
{
    if (!node || !node[0]) return -1;
    cardmatch_cache_prune();
    for (int i = 0; i < g_cardmatch_cached; ++i) {
        if (strcmp(g_cardmatch_cache[i].node, node) == 0) {
            return g_cardmatch_cache[i].card;
        }
    }
    return -1;
}

/* The identity a node's note was written with, for the log. "" if none. */
static const char *cardmatch_cache_usbbus(const char *node)
{
    if (!node || !node[0]) return "";
    for (int i = 0; i < g_cardmatch_cached; ++i) {
        if (strcmp(g_cardmatch_cache[i].node, node) == 0) {
            return g_cardmatch_cache[i].usbbus;
        }
    }
    return "";
}

/* Remember that `node` answered as `card`, together with what that card is
 * right now. A card belongs to one controller, so any other node's note
 * naming the same card goes -- the probe just proved it wrong. */
static void cardmatch_cache_put(const char *node, int card)
{
    if (!node || !node[0] || card < 0) return;

    char usbbus[CARDMATCH_USBBUS_LEN];
    if (!cardmatch_read_usbbus(card, usbbus, sizeof(usbbus))) {
        alsa_log("[cardmatch]", "card=%d for %s not remembered: usbbus unreadable (errno=%d)",
                 card, node, errno);
        return;
    }

    cardmatch_cache_prune();
    for (int i = 0; i < g_cardmatch_cached; ) {
        if (g_cardmatch_cache[i].card == card &&
            strcmp(g_cardmatch_cache[i].node, node) != 0) {
            alsa_log("[cardmatch]", "forgot %s: card=%d now answers for %s",
                     g_cardmatch_cache[i].node, card, node);
            cardmatch_cache_drop(i);
            continue;
        }
        ++i;
    }
    for (int i = 0; i < g_cardmatch_cached; ++i) {
        if (strcmp(g_cardmatch_cache[i].node, node) == 0) {
            g_cardmatch_cache[i].card = card;
            snprintf(g_cardmatch_cache[i].usbbus,
                     sizeof(g_cardmatch_cache[i].usbbus), "%s", usbbus);
            return;
        }
    }
    if (g_cardmatch_cached >= CARDMATCH_CACHE_MAX) {
        alsa_log("[cardmatch]", "card=%d for %s not remembered: table full",
                 card, node);
        return;
    }
    snprintf(g_cardmatch_cache[g_cardmatch_cached].node,
             sizeof(g_cardmatch_cache[0].node), "%s", node);
    g_cardmatch_cache[g_cardmatch_cached].card = card;
    snprintf(g_cardmatch_cache[g_cardmatch_cached].usbbus,
             sizeof(g_cardmatch_cache[0].usbbus), "%s", usbbus);
    ++g_cardmatch_cached;
}
