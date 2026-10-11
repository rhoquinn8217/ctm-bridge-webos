/* A guard on where the bridged-and-waiting light is wired, and where it must
 * never be.
 *
 * ⭐ WHY THIS EXISTS (rhoquinn8217, 2026-10-08). A bridged pad breathes green
 * until the PC takes its light, painted from the session loop and stopped by
 * the PC's first report that sets the light. ⛔ A Bluetooth DS4 must not have
 * it: the breath writes the pad twenty times a second, and a write to a
 * bridged Bluetooth DS4 can block for seconds while holding the kernel's one
 * lock for every hidraw node. These read the shipped source.
 *
 * ➡️ Build and run:  cc tests/test_ready_light_wiring.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

static char *load(const char *rel)
{
    const char *prefixes[] = { "", "../" };
    for (unsigned i = 0; i < 2; ++i) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s", prefixes[i], rel);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *s = malloc((size_t)n + 1);
        if (!s) { fclose(f); return NULL; }
        s[fread(s, 1, (size_t)n, f)] = '\0';
        fclose(f);
        return s;
    }
    return NULL;
}

/* From `head` to the first line that starts with `end`. NULL if not found. */
static char *between(const char *src, const char *head, const char *end)
{
    const char *a = src ? strstr(src, head) : NULL;
    if (!a) return NULL;
    const char *b = strstr(a, end);
    if (!b) return NULL;
    char *out = malloc((size_t)(b - a) + 1);
    if (!out) return NULL;
    memcpy(out, a, (size_t)(b - a));
    out[b - a] = '\0';
    return out;
}

int main(void)
{
    char *common = load("src/controllers/controller_common.c");
    char *ds4 = load("src/controllers/controller_ds4.c");
    char *ds5 = load("src/controllers/controller_ds5.c");
    char *xpad = load("src/controllers/controller_xpad.c");
    ok(common && ds4 && ds5 && xpad, "the guard can read the four files");
    if (!common || !ds4 || !ds5 || !xpad) return 1;

    printf("\nthe session\n");
    char *loop = between(common, "static void run_session(ctm_controller_t *c,", "\n}");
    char *msg = between(common, "static void handle_message(ctm_controller_t *c,", "\n}");
    char *tick = between(common, "static void ready_light_tick(ctm_controller_t *c)", "\n}");
    ok(loop && strstr(loop, "ready_light_tick(c);") != NULL, "the session loop breathes the light");
    ok(msg && strstr(msg, "ready_light_note_host(c, payload") != NULL,
       "every report from the PC is asked whether it sets the light");
    ok(tick && strstr(tick, "controller_signal_busy(c)") != NULL,
       "and the breath waits for any signal, and stops for a release");

    printf("\nwhich pads breathe\n");
    char *ds4_bt = between(ds4, "const ctm_controller_ops_t ctm_controller_ds4_ops = {", "\n};");
    char *ds4_usb = between(ds4, "const ctm_controller_ops_t controller_ds4_usb_ops = {", "\n};");
    char *ds5_ops = between(ds5, "const ctm_controller_ops_t ctm_controller_ds5_ops = {", "\n};");
    char *ds5e_ops = between(ds5, "const ctm_controller_ops_t ctm_controller_ds5e_ops = {", "\n};");
    ok(ds4_bt && strstr(ds4_bt, ".ready_light") == NULL,
       "never a Bluetooth DS4: a write to a bridged one can block for seconds");
    ok(ds4_usb && strstr(ds4_usb, ".ready_light = ds4_usb_ready_light") != NULL &&
       strstr(ds4_usb, ".host_sets_light = ds4_usb_host_sets_light") != NULL, "a cabled DS4 does");
    ok(ds5_ops && strstr(ds5_ops, ".ready_light = ds5_ready_light") != NULL &&
       strstr(ds5_ops, ".host_sets_light = ds5_report_sets_light") != NULL, "a DualSense does, on either bus");
    ok(ds5e_ops && strstr(ds5e_ops, ".ready_light = ds5_ready_light") != NULL &&
       strstr(ds5e_ops, ".host_sets_light = ds5_report_sets_light") != NULL, "and an Edge");

    printf("\nan Xbox pad's one buzz each way\n");
    char *xpad_ops = between(xpad, "const ctm_controller_ops_t controller_xpad_ops = {", "\n};");
    ok(xpad_ops && strstr(xpad_ops, ".signal_connected = xpad_signal_connected") != NULL &&
       strstr(xpad_ops, ".signal_unplugging = xpad_signal_unplugging") != NULL,
       "the core buzzes it on bridge and release, so the app stands aside for both");
    ok(xpad_ops && strstr(xpad_ops, ".on_plug_init") == NULL, "and not a second time as the session starts");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
