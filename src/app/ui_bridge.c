/* Agent control client (discovery + commands), bridge sessions, process
 * spawning, the per-device settings store, and plug-in/out orchestration.
 * Moved verbatim out of lvgl_ui.c; de-static'd and prototyped in ui_common.h. */

#define _GNU_SOURCE

#include "ctm_state.h"
#include "ctm_bridge_protocol.h"
#include "ctm_hostmouse.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
tv_bridge_worker_settings_t default_settings_for_item(const logical_device_t *item)
{
    tv_bridge_worker_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    settings.kind = TV_BRIDGE_KIND_HID;
    settings.audio_mode = TV_BRIDGE_AUDIO_AUTO;
    settings.latency_ms = 60;
    settings.haptics_gain_centi = 100;
    settings.headset_volume_percent = 100;
    settings.speaker_volume_percent = 100;
    settings.ds5_patch_high_nibble = 0xf;
    settings.ds5_patch_low_nibble = 0xd;
    settings.ds5_patch2_high_nibble = 0xf;
    settings.ds5_patch2_low_nibble = 0x7;

    const char *kind = bridge_kind_for_item(item);
    if (strcmp(kind, "ds5") == 0) {
        settings.kind = TV_BRIDGE_KIND_DS5;
        settings.headset_volume_percent = 0x4d;
        /* 0x64, matching the wired path, rather than the 0x41 this was.
         *
         * The hardware range is 0x00-0x64 and the controller is quiet at the
         * bottom of it: measured on this fleet, anything under roughly 0x3c
         * is inaudible across a room. 0x41 sat barely above that. The wired
         * path settled on 0x64 after measuring 94 dB against 80 dB at 0x64
         * with echo cancel off, and games send 0x64 themselves.
         *
         * Nothing on the host side ever corrects a low value: the listener
         * only learns a speaker volume when Windows sends a USB Audio Class
         * volume message, which does not happen unless someone moves that
         * device's slider. So whatever is set here is what the user hears. */
        settings.speaker_volume_percent = 0x64;
    } else if (strcmp(kind, "ds5_usb") == 0) {
        /* Wired: the host owns the output reports, so the audio fields are
         * never patched here. Volume defaults match the BT arm so behaviour
         * is unchanged for a listener that does not distinguish the two. */
        settings.kind = TV_BRIDGE_KIND_DS5;
        settings.headset_volume_percent = 0x4d;
        settings.speaker_volume_percent = 0x64;   /* see the ds5 arm above */
    } else if (strcmp(kind, "ds5e") == 0) {
        /* Edge over BT: a DualSense as far as reports go, so the same values. */
        settings.kind = TV_BRIDGE_KIND_DS5;
        settings.headset_volume_percent = 0x4d;
        settings.speaker_volume_percent = 0x64;   /* see the ds5 arm above */
    } else if (strcmp(kind, "ds5e_usb") == 0) {
        /* Edge wired: as ds5_usb -- the host owns the output reports. */
        settings.kind = TV_BRIDGE_KIND_DS5;
        settings.headset_volume_percent = 0x4d;
        settings.speaker_volume_percent = 0x64;   /* see the ds5 arm above */
    } else if (strncmp(kind, "ds4", 3) == 0) {   /* ds4 and ds4_usb alike */
        settings.kind = TV_BRIDGE_KIND_DS4;
        settings.haptics_gain_centi = 0;
        /* ~75% of the 0x4F raw ceiling — the pad persists whatever volume was
         * last written, so a sane audible default beats inheriting stale 0. */
        settings.headset_volume_percent = 0x3b;
        settings.speaker_volume_percent = 0x3b;
    }
    return settings;
}

ui_device_settings_t *ui_record_for_item(const logical_device_t *item)
{
    if (!item) return NULL;
    for (int i = 0; i < g_settings_count; ++i) {
        if (strcmp(g_settings[i].key, item->key) == 0) {
            return &g_settings[i];
        }
    }
    if (g_settings_count >= MAX_DEVICES) {
        return NULL;
    }
    snprintf(g_settings[g_settings_count].key, sizeof(g_settings[0].key), "%s", item->key);
    g_settings[g_settings_count].settings = default_settings_for_item(item);
    g_settings[g_settings_count].headset_volume_percent =
        g_settings[g_settings_count].settings.headset_volume_percent;
    g_settings[g_settings_count].speaker_volume_percent =
        g_settings[g_settings_count].settings.speaker_volume_percent;
    return &g_settings[g_settings_count++];
}

tv_bridge_worker_settings_t *settings_for_item(const logical_device_t *item)
{
    ui_device_settings_t *record = ui_record_for_item(item);
    return record ? &record->settings : NULL;
}

static int session_index_locked(const char *key);

void apply_settings_to_session(const logical_device_t *item)
{
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!item || !settings) return;
    /* ⓘ Under the table's lock, and never on a stopping entry: its controller
     * may be freed by the path tearing it down. */
    pthread_mutex_lock(&g_sessions_mutex);
    int session = session_index_locked(item->key);
    if (session >= 0 && !g_sessions[session].stopping && g_sessions[session].controller) {
        ctm_controller_set_settings(g_sessions[session].controller, settings);
    }
    pthread_mutex_unlock(&g_sessions_mutex);
}

int run_child_wait(char *const argv[])
{
    /* posix_spawn (clone/vfork semantics) instead of fork()+exec. fork() copies
     * the parent's page tables — cheap in the small standalone app, but expensive
     * and a recurring stall inside the large moonlight process (video buffers).
     * The stopSniff worker calls this every 500 ms, so the fork stall was starving
     * the bridge -> BT sniff mode (~115 Hz) + added latency. posix_spawn does not
     * copy the address space, so its cost is independent of process size. */
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_RDWR, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_RDWR, 0);

    extern char **environ;
    pid_t pid = -1;
    int spawn_rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0) {
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void stop_sniff_once(const char *mac)
{
    if (!valid_bt_address(mac)) {
        return;
    }
    char payload[96];
    snprintf(payload, sizeof(payload), "{\"address\":\"%s\"}", mac);
    char *const argv[] = {
        "luna-send-pub",
        "-n",
        "1",
        "-f",
        "luna://com.webos.service.bluetooth2/device/internal/stopSniff",
        payload,
        NULL
    };
    (void)run_child_wait(argv);
}

void *stop_sniff_worker(void *arg)
{
    (void)arg;
    while (g_running) {
        char macs[MAX_DEVICES][64];
        int count = 0;
        pthread_mutex_lock(&g_bt_mac_mutex);
        count = g_bt_mac_count;
        if (count > MAX_DEVICES) count = MAX_DEVICES;
        for (int i = 0; i < count; ++i) {
            snprintf(macs[i], sizeof(macs[i]), "%s", g_bt_macs[i]);
        }
        pthread_mutex_unlock(&g_bt_mac_mutex);

        for (int i = 0; i < count; ++i) {
            stop_sniff_once(macs[i]);
        }
        usleep(500000);
    }
    return NULL;
}

void publish_bt_macs(void)
{
    char macs[MAX_DEVICES][64];
    int count = 0;
    for (int i = 0; i < g_devices.count && count < MAX_DEVICES; ++i) {
        const logical_device_t *item = &g_devices.items[i];
        if (strcmp(bus_label(item->bus), "BT") != 0 || !valid_bt_address(item->mac)) {
            continue;
        }
        bool duplicate = false;
        for (int j = 0; j < count; ++j) {
            if (strcmp(macs[j], item->mac) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            snprintf(macs[count++], sizeof(macs[0]), "%s", item->mac);
        }
    }

    pthread_mutex_lock(&g_bt_mac_mutex);
    g_bt_mac_count = count;
    for (int i = 0; i < count; ++i) {
        snprintf(g_bt_macs[i], sizeof(g_bt_macs[0]), "%s", macs[i]);
    }
    pthread_mutex_unlock(&g_bt_mac_mutex);
}

/* Set the agent endpoint directly, skipping discovery. When: a host app that
 * already knows where the agent is (e.g. moonlight, which is streaming from
 * that same machine) can say so instead of relying on a broadcast probe, which
 * cannot leave the local network. Passing NULL or "" clears it and restores
 * discovery. */
void ctm_bridge_set_agent_host(const char *host, int port)
{
    if (host && host[0]) {
        snprintf(g_agent_host, sizeof(g_agent_host), "%s", host);
    } else {
        g_agent_host[0] = '\0';
    }
    g_agent_port = port > 0 ? port : CTM_AGENT_PORT;
    g_agent_online = g_agent_host[0] != '\0';
}

/* Do we know where the agent is, and was it answering when the worker last
 * asked?
 *
 * ⛔ THE BROADCAST IS GONE (2026-09-15). This used to probe the local network
 * for an agent whenever no address was set -- and in the app it could never
 * run: a stream sets the address as it starts, and the function returned
 * before opening the socket whenever an address was known. So the probe read
 * as live code for weeks while being unreachable, which is the whole reason
 * this clean-up exists. The headless ui_app is told its address now, so
 * nothing anywhere is left to discover.
 *
 * ⓘ Knowing the address is NOT evidence that anything is listening there. The
 * worker asks every few seconds on its own thread and leaves the answer here,
 * so this returns at once. ⛔ Never ask inline: this is called from the
 * interface -- on stream start, on opening the overlay, on every header
 * refresh -- and a network call here stalls it for as long as the host takes
 * to not answer.
 *
 * ⓘ The address is deliberately kept when the agent goes quiet: it may simply
 * be restarting, and the next probe should try the same place again. */
bool agent_is_known(void)
{
    return g_agent_host[0] ? g_agent_online : false;
}

int send_agent_command(const char *command, char *response, size_t response_len)
{
    /* ⛔⛔ THE ADDRESS, NOT THE ONLINE FLAG. This is what the probe uses to ask
     * whether the agent is there, so requiring `online` here latches the answer:
     * the listener goes away, the flag goes false, and every command refuses --
     * including the probe that would have noticed it came back. The TV then
     * says "listener offline" until a stream start re-sets the address.
     *
     * ⓘ Introduced and found the same evening, 2026-09-15, while tidying the
     * broadcast discovery away. agent_is_known() is right for the UI and the
     * plug paths, which want "known AND answering"; this one wants "do we know
     * where to ask". */
    if (!g_agent_host[0]) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_agent_port);
    if (inet_aton(g_agent_host, &addr.sin_addr) == 0 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        g_agent_online = false;
        g_agent_probed = true;
        return -1;
    }

    char line[512];
    snprintf(line, sizeof(line), "%s\n", command);
    if (send(fd, line, strlen(line), 0) < 0) {
        close(fd);
        return -1;
    }
    ssize_t n = recv(fd, response, response_len > 0 ? response_len - 1 : 0, 0);
    close(fd);
    if (response_len > 0) {
        response[n > 0 ? n : 0] = '\0';
    }
    /* ⭐⭐ A COMMAND THAT GOT THROUGH IS PROOF THE AGENT IS THERE.
     *
     * ⛔ The failure path above already sets g_agent_online false; nothing set
     * it TRUE except the probe, so a successful bridge left the panel still
     * claiming the server was down until the next probe came round.
     *
     * ⭐ It also means a bridge attempted while the state is UNKNOWN resolves
     * it either way, which is what makes pressing a row a reasonable thing to
     * do before anything has been probed. */
    if (n > 0) {
        g_agent_online = true;
        g_agent_probed = true;
    }
    return n > 0 && response && starts_with(response, "OK") ? 0 : -1;
}

static int session_index_locked(const char *key)
{
    for (int i = 0; i < g_session_count; ++i) {
        if (strcmp(g_sessions[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

/* ⓘ The index is only good while nothing else edits the table, so callers use
 * it as "is this bridged" -- a stopping entry counts, since its teardown has not
 * finished. Anything that reads the entry itself takes g_sessions_mutex and uses
 * session_index_locked(). */
int session_index_for_key(const char *key)
{
    pthread_mutex_lock(&g_sessions_mutex);
    const int index = session_index_locked(key);
    pthread_mutex_unlock(&g_sessions_mutex);
    return index;
}

/* TV-pointer session state lives OUTSIDE g_sessions (it is not a controller),
 * so the port allocator must be told about its port explicitly — otherwise the
 * first generic-HID plug reuses the pointer's port and its TCP session lands
 * on the pointer's listener (agent bridge waits forever, device never
 * enumerates on the host). */
static bool g_tv_pointer_active;
static char g_tv_pointer_busid[32];
static int g_tv_pointer_port;

int next_bridge_port(void)
{
    int port = CTM_BRIDGE_BASE_PORT;
    pthread_mutex_lock(&g_sessions_mutex);
    for (;;) {
        bool used = false;
        if (g_tv_pointer_active && port == g_tv_pointer_port) {
            used = true;
        }
        for (int i = 0; !used && i < g_session_count; ++i) {
            if (g_sessions[i].port == port) {
                used = true;
                break;
            }
        }
        if (!used) {
            pthread_mutex_unlock(&g_sessions_mutex);
            return port;
        }
        ++port;
    }
}

int first_scan_index_for_item(const logical_device_t *item)
{
    if (!item || item->device_count <= 0) return -1;
    int index = item->device_indices[0];
    return index >= 0 && index < g_scan.count ? index : -1;
}

void make_bridge_busid(const logical_device_t *item, char *out, size_t out_len)
{
    static unsigned seq;
    const char *kind = bridge_kind_for_item(item);
    snprintf(out, out_len, "ctm-%s-%u", kind, ++seq);
}

/* ⭐⭐ A NEW SESSION, OR A NEW CONTROLLER FOR ONE ALREADY IN THE TABLE.
 *
 * ⛔ Refused while the key's previous session is still stopping: taking over
 * its entry would hand the path tearing it down a controller it never claimed.
 * The caller tears the new controller down again. ⓘ A replaced controller is
 * torn down here, after its entry already holds the new one, so no other path
 * can reach it. */
bool add_session(const char *key, const char *busid, ctm_controller_t *controller, int port)
{
    pthread_mutex_lock(&g_sessions_mutex);
    int index = session_index_locked(key);
    if (index >= 0) {
        if (g_sessions[index].stopping) {
            pthread_mutex_unlock(&g_sessions_mutex);
            return false;
        }
        ctm_controller_t *replaced = g_sessions[index].controller != controller
                                     ? g_sessions[index].controller : NULL;
        snprintf(g_sessions[index].busid, sizeof(g_sessions[index].busid), "%s", busid ? busid : "");
        g_sessions[index].port = port;
        g_sessions[index].controller = controller;
        pthread_mutex_unlock(&g_sessions_mutex);
        if (replaced) {
            ctm_controller_plug_out_reason(replaced, CTM_UNPLUG_REPLACED);
            ctm_controller_destroy(replaced);
        }
        return true;
    }
    if (g_session_count >= MAX_SESSIONS) {
        pthread_mutex_unlock(&g_sessions_mutex);
        return false;
    }
    snprintf(g_sessions[g_session_count].key, sizeof(g_sessions[0].key), "%s", key);
    snprintf(g_sessions[g_session_count].busid, sizeof(g_sessions[0].busid), "%s", busid ? busid : "");
    g_sessions[g_session_count].port = port;
    g_sessions[g_session_count].controller = controller;
    g_sessions[g_session_count].stopping = false;
    g_session_count++;
    pthread_mutex_unlock(&g_sessions_mutex);
    return true;
}

/* Take a stopping entry out of the table and wake anyone waiting for it.
 * When: holding g_sessions_mutex, after its teardown has finished. */
static void session_remove_stopped_locked(const char *key)
{
    const int index = session_index_locked(key);
    if (index >= 0 && g_sessions[index].stopping) {
        memmove(&g_sessions[index], &g_sessions[index + 1],
                (size_t)(g_session_count - index - 1) * sizeof(g_sessions[0]));
        g_session_count--;
    }
    pthread_cond_broadcast(&g_sessions_cond);
}

/* ⭐⭐ ONE PATH TEARS A CONTROLLER DOWN, WHICHEVER ASKS FIRST.
 *
 * ⛔ THE FAULT: the chord's release worker called this with no lock, the panel's
 * Release under the app's device lock, and the end of the stream went through
 * release_local_sessions_on_exit() with no lock at all. The entry stayed in the
 * table until the plug-out finished -- up to 2.4 s with a release signal -- so a
 * second path in that window found the same controller, plugged it out again
 * and destroyed it a second time.
 * ➡️ Now the entry is CLAIMED under g_sessions_mutex before anything slow
 * happens. A path that finds it already claimed returns: the claimant finishes
 * the job, and removes the entry when it has. */
void stop_session(const char *key)
{
    pthread_mutex_lock(&g_sessions_mutex);
    const int index = session_index_locked(key);
    if (index < 0 || g_sessions[index].stopping) {
        pthread_mutex_unlock(&g_sessions_mutex);
        return;
    }
    g_sessions[index].stopping = true;
    ctm_controller_t *controller = g_sessions[index].controller;
    char busid[sizeof(g_sessions[0].busid)];
    snprintf(busid, sizeof(busid), "%s", g_sessions[index].busid);
    const int port = g_sessions[index].port;
    pthread_mutex_unlock(&g_sessions_mutex);

    if (controller) {
        ctm_controller_plug_out(controller);
        ctm_controller_destroy(controller);
    }
    if (port > 0) {
        char cmd[160];
        char response[256];
        snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
        (void)send_agent_command(cmd, response, sizeof(response));
    }

    pthread_mutex_lock(&g_sessions_mutex);
    session_remove_stopped_locked(key);
    pthread_mutex_unlock(&g_sessions_mutex);
}

/* --- TV pointer -> host mouse (synthetic device; no hidraw) ---------------
 * Tracked separately from g_sessions (which is controller-typed): our own
 * BRIDGE_START/STOP around the ctm_hostmouse synthesizer. Kind MUST be "hid":
 * the agent whitelists BRIDGE_START kinds and only "hid" reaches the "auto"
 * dynamic-profile path that builds the device from the descriptor the
 * synthesizer sends in HELLO (unknown kinds are rejected, not auto-routed).
 * State (g_tv_pointer_active/busid/port) is declared above next_bridge_port,
 * which must skip the pointer's port. */

bool ctm_tv_pointer_plug(void)
{
    if (g_tv_pointer_active) return true;
    if (!agent_is_known()) {
        log_append("TV pointer: Windows agent not found");
        return false;
    }
    static unsigned seq;
    int port = next_bridge_port();
    snprintf(g_tv_pointer_busid, sizeof(g_tv_pointer_busid), "ctm-mouse-%u", ++seq);
    char cmd[160], response[256];
    snprintf(cmd, sizeof(cmd), "BRIDGE_START hid %d %s", port, g_tv_pointer_busid);
    if (send_agent_command(cmd, response, sizeof(response)) != 0) {
        log_append("TV pointer: agent bridge start failed: %s", response);
        return false;
    }
    if (ctm_hostmouse_plug(g_agent_host, port) != 0) {
        log_append("TV pointer: synthesizer start failed");
        snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", g_tv_pointer_busid);
        (void)send_agent_command(cmd, response, sizeof(response));
        return false;
    }
    g_tv_pointer_port = port;
    g_tv_pointer_active = true;
    log_append("TV pointer bridged to host (busid=%s port=%d)", g_tv_pointer_busid, port);
    return true;
}

void ctm_tv_pointer_unplug(void)
{
    if (!g_tv_pointer_active) return;
    ctm_hostmouse_unplug();
    char cmd[160], response[256];
    snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", g_tv_pointer_busid);
    (void)send_agent_command(cmd, response, sizeof(response));
    g_tv_pointer_active = false;
    log_append("TV pointer released; control returned to TV");
}

bool ctm_tv_pointer_active(void)
{
    return g_tv_pointer_active;
}

/* ⭐ EVERY SESSION, AT THE END OF A STREAM OR OF THE APP.
 *
 * ⛔ It walked the table with no lock and plugged out whatever it found, so a
 * controller the chord's release worker or the panel was already tearing down
 * was plugged out and destroyed twice (see stop_session). ➡️ It claims every
 * entry nobody else has, tears those down, and then WAITS for the ones another
 * path is still finishing, so the stream's end still returns with every release
 * played. ⓘ No BRIDGE_STOP, as before: the listener sees the connections close.
 * ⚠️ The wait gives up after 5 s rather than hanging an exit on a teardown that
 * never finishes. */
void release_local_sessions_on_exit(void)
{
    ctm_tv_pointer_unplug();

    char keys[MAX_SESSIONS][sizeof(g_sessions[0].key)];
    ctm_controller_t *controllers[MAX_SESSIONS];
    int n = 0;
    pthread_mutex_lock(&g_sessions_mutex);
    for (int i = 0; i < g_session_count && n < MAX_SESSIONS; ++i) {
        if (g_sessions[i].stopping) {
            continue;
        }
        g_sessions[i].stopping = true;
        snprintf(keys[n], sizeof(keys[n]), "%s", g_sessions[i].key);
        controllers[n] = g_sessions[i].controller;
        ++n;
    }
    pthread_mutex_unlock(&g_sessions_mutex);

    for (int i = 0; i < n; ++i) {
        if (controllers[i]) {
            ctm_controller_plug_out_reason(controllers[i], CTM_UNPLUG_SHUTDOWN);
            ctm_controller_destroy(controllers[i]);
        }
    }

    pthread_mutex_lock(&g_sessions_mutex);
    for (int i = 0; i < n; ++i) {
        session_remove_stopped_locked(keys[i]);
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    for (;;) {
        int still = 0;
        for (int i = 0; i < g_session_count; ++i) {
            if (g_sessions[i].stopping) ++still;
        }
        if (still == 0) {
            break;
        }
        if (pthread_cond_timedwait(&g_sessions_cond, &g_sessions_mutex, &deadline) == ETIMEDOUT) {
            log_append("release on exit: %d session(s) still being torn down after 5 s", still);
            break;
        }
    }
    pthread_mutex_unlock(&g_sessions_mutex);
}

const char *bridge_kind_for_item(const logical_device_t *item)
{
    if (!item) return "hid";
    if (strcmp(item->vid, "054c") == 0 && strcmp(item->pid, "0ce6") == 0)
        return strcmp(bus_label(item->bus), "USB") == 0 ? "ds5_usb" : "ds5";
    if (strcmp(item->vid, "054c") == 0 && strcmp(item->pid, "0df2") == 0)
        return strcmp(bus_label(item->bus), "USB") == 0 ? "ds5e_usb" : "ds5e";
    /* ⭐⭐ THE BUS DECIDES, as it does for the DualSense arms above.
     *
     * ⛔ THE FAULT THIS FIXES: this arm answered "ds4" whatever the bus, so a
     * CABLED DS4 was handed to the host as a Bluetooth one. The host then loads
     * the map that reads Bluetooth report 0x11 while the pad sends 0x01, so the
     * pad bridged, showed PLUGGED, and did nothing in the game.
     *
     * ⓘ A wired DS4 needs no report rewriting: its own 0x01 report is already
     * exactly what the virtual wired DS4 emits, so the host side is a
     * pass-through map, the way the wired DualSense's is. */
    if (strcmp(item->vid, "054c") == 0 &&
        (strcmp(item->pid, "09cc") == 0 || strcmp(item->pid, "05c4") == 0))
        return strcmp(bus_label(item->bus), "USB") == 0 ? "ds4_usb" : "ds4";
    /* ⭐ Anything the Xbox driver runs reaches the host as an Xbox pad, whoever
     * made it -- the TV reads them all the same way (controller_xpad.c). */
    if (strcmp(item->driver, "xpad") == 0) return "xbox";
    if (strcmp(item->vid, "045e") == 0 &&
        (is_xbox_pid(item->pid) || contains_ci(item->name, "xbox"))) return "xbox";
    if (strcmp(item->vid, "28de") == 0 && strcmp(item->pid, "1304") == 0) return "puck";
    return "hid";
}

/* Session key for ONE specific hidraw node of a logical device, so each
 * interface can be plugged/unplugged independently. When: per-node plug + its
 * button state. */
void node_session_key(const logical_device_t *item, int scan_index, char *out, size_t out_len)
{
    const char *tag = "node";
    if (scan_index >= 0 && scan_index < g_scan.count && g_scan.devices[scan_index].hidraw[0]) {
        tag = g_scan.devices[scan_index].hidraw;
    }
    snprintf(out, out_len, "%s#%s", item ? item->key : "", tag);
}

/* Core plug: start the host bridge, build a controller from one chosen scan
 * node, plug it in, and register the session under session_key. Shared by the
 * whole-device plug (plug_in_item) and the per-interface plug (plug_in_node).
 * When: any Plug button. */
/* Serialize the cached puck enumeration (g_puck_enum) into a CTMB_MSG_ENUM
 * payload: [ctmb_enum_info_t][descriptors blob][ per iface: ctmb_enum_iface_t +
 * report_desc ]. Caller frees. NULL if no valid enumeration. */
static uint8_t *build_puck_enum_payload(int *out_len)
{
    if (!g_puck_enum.valid) return NULL;
    int size = (int)sizeof(ctmb_enum_info_t) + g_puck_enum.descriptors_len;
    for (int i = 0; i < g_puck_enum.if_count; ++i) {
        size += (int)sizeof(ctmb_enum_iface_t) + g_puck_enum.ifs[i].rdesc_len;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) return NULL;
    int off = 0;
    ctmb_enum_info_t info;
    memset(&info, 0, sizeof(info));
    info.descriptors_len = (uint16_t)g_puck_enum.descriptors_len;
    info.iface_count = (uint8_t)g_puck_enum.if_count;
    info.full_speed = 1;   /* the puck is full-speed (speed=12) */
    memcpy(buf + off, &info, sizeof(info)); off += (int)sizeof(info);
    memcpy(buf + off, g_puck_enum.descriptors, (size_t)g_puck_enum.descriptors_len);
    off += g_puck_enum.descriptors_len;
    for (int i = 0; i < g_puck_enum.if_count; ++i) {
        ctmb_enum_iface_t ie;
        memset(&ie, 0, sizeof(ie));
        ie.interface_number = (uint8_t)g_puck_enum.ifs[i].num;
        ie.iface_class = (uint8_t)strtol(g_puck_enum.ifs[i].cls, NULL, 16);
        ie.report_desc_len = (uint16_t)g_puck_enum.ifs[i].rdesc_len;
        memcpy(buf + off, &ie, sizeof(ie)); off += (int)sizeof(ie);
        memcpy(buf + off, g_puck_enum.ifs[i].rdesc, (size_t)g_puck_enum.ifs[i].rdesc_len);
        off += g_puck_enum.ifs[i].rdesc_len;
    }
    *out_len = off;
    return buf;
}

static bool plug_in_scan_index(logical_device_t *item, int scan_index, const char *session_key)
{
    if (!item || scan_index < 0 || scan_index >= g_scan.count) {
        return false;
    }
    if (!agent_is_known()) {
        log_append("Windows agent not found");
        return false;
    }

    const device_info_t *dev = &g_scan.devices[scan_index];
    const char *kind = bridge_kind_for_item(item);

    /* Build the neutral descriptor and hand the device to a controller — one
     * mechanism for DS5/DS4/xbox/puck/generic (factory picks the ops). */
    ctm_controller_dev_t cdev;
    memset(&cdev, 0, sizeof(cdev));
    snprintf(cdev.vid, sizeof(cdev.vid), "%s", item->vid);
    snprintf(cdev.pid, sizeof(cdev.pid), "%s", item->pid);
    snprintf(cdev.bus, sizeof(cdev.bus), "%s", bus_label(item->bus));
    snprintf(cdev.name, sizeof(cdev.name), "%s", item->name);
    snprintf(cdev.path, sizeof(cdev.path), "%s", dev->node);
    snprintf(cdev.mac, sizeof(cdev.mac), "%s", item->mac);
    snprintf(cdev.serial, sizeof(cdev.serial), "%s", item->serial);
    snprintf(cdev.driver, sizeof(cdev.driver), "%s", item->driver);

    /* ⭐⭐ REFUSE A DEVICE THAT CANNOT BE READ, BEFORE THE HOST HEARS OF IT.
     *
     * ⛔ THE FAULT, measured on the U5s 2026-09-13 with both Xbox pads: their
     * node is /dev/input/jsN, which cannot be read as HID. The session found
     * that out on its own thread, after BRIDGE_START and after the row read
     * bridged, and stopped without a word -- the row stayed bridged and the
     * host kept a session that timed out 30 s later.
     *
     * ➡️ Asked here instead, so nothing is started that cannot run. */
    const int refused = controller_preflight(&cdev);
    if (refused != 0) {
        log_append("refused %s: %s cannot be opened for bridging (%s)",
                   item->name, dev->node, strerror(refused));
        ctm_gesture_log(NULL, "bridge refused: %s (kind %s) at %s cannot be opened for "
                        "bridging, errno=%d -- nothing was sent to the host",
                        item->name, kind, dev->node, refused);
        return false;
    }

    char response[512];
    int port = next_bridge_port();
    char cmd[256];
    char busid[32];
    make_bridge_busid(item, busid, sizeof(busid));
    snprintf(cmd, sizeof(cmd), "BRIDGE_START %s %d %s", kind, port, busid);
    if (send_agent_command(cmd, response, sizeof(response)) != 0) {
        log_append("agent bridge start failed: %s", response);
        return false;
    }

    /* ⭐⭐ THE DS4's HANDOVER TONE, PLAYED BEFORE THE SESSION EXISTS.
     *
     * ⛔ It cannot be played after. Measured across builds 412-417 on
     * 2026-09-22: a write to a BRIDGED DS4 blocks about five seconds every
     * time, so the frames arrive far too late and the decoder plays a click.
     * Moving the tone to the session thread fixed the pacing and cost five
     * seconds of input lag instead; halving the write rate changed nothing.
     * ✅ The link is free right here, which is why the REFUSAL tone -- the one
     * signal that has never had a session -- has been clean since its first
     * attempt.
     *
     * ⓘ It costs about a second before the pad bridges, and it reads the
     * right way round: the tone says "handing over", and then it does.
     * ⚠️ Synchronous on purpose. On a thread it would overlap the session
     * starting, which is the very thing that breaks it. */
    if (strcmp(kind, "ds4") == 0) {
        const int trc = ds4_signal_tone_node(cdev.path, 0 /* BTSIG_HANDING_OVER */);
        log_append("ds4 handover tone before the session: rc=%d", trc);
    }

    ctm_controller_t *controller = ctm_controller_create(&cdev);
    if (!controller) {
        log_append("controller create failed");
        snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
        (void)send_agent_command(cmd, response, sizeof(response));
        return false;
    }
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (settings) {
        ctm_controller_set_settings(controller, settings);
    }
    /* Composite (puck): forward the cached USB enumeration so the host builds the
     * full composite device from the puck's own descriptors (Stage 2). */
    if (strcmp(kind, "puck") == 0 && g_puck_enum.valid) {
        int elen = 0;
        uint8_t *epl = build_puck_enum_payload(&elen);
        if (epl) {
            ctm_controller_set_enum_payload(controller, epl, elen);
            free(epl);
            log_append("forwarding puck enumeration to host (%d bytes)", elen);
        }
    }
    if (ctm_controller_plug_in(controller, g_agent_host, port) != 0) {
        log_append("controller plug-in failed");
        ctm_controller_destroy(controller);
        snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
        (void)send_agent_command(cmd, response, sizeof(response));
        return false;
    }
    /* ⛔ Its answer was ignored, which left a controller that could not be
     * recorded running with nothing able to release it. Now it is torn down
     * again: the table is full, or the key's previous session is still
     * stopping (see add_session). */
    if (!add_session(session_key, busid, controller, port)) {
        log_append("controller for %s not recorded (table full, or its last session is "
                   "still stopping); undoing the plug", session_key);
        ctm_controller_plug_out(controller);
        ctm_controller_destroy(controller);
        snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
        (void)send_agent_command(cmd, response, sizeof(response));
        return false;
    }
    log_append("controller started kind=%s node=%s busid=%s host=%s port=%d",
               kind, dev->node, busid, g_agent_host, port);
    return true;
}

/* The TV's own Magic Remote (e.g. "LGE MR24"): its row IS the TV-pointer
 * bridge. The raw hidraw relay would forward the LG-vendor descriptor that
 * Windows rejects (code 10), so plug/unplug of this row must drive the
 * ctm_hostmouse synthesizer instead — see plug_button_cb/auto_plug_devices. */
bool item_is_tv_remote(const logical_device_t *item)
{
    if (!item) return false;
    return strncmp(item->name, "LGE ", 4) == 0 || strstr(item->name, "MR2") != NULL;
}

/* Plug the whole device using its first hidraw node (the default). When: the
 * logical row's Plug button. */
bool plug_in_item(logical_device_t *item)
{
    if (!item) {
        return false;
    }
    ctm_bridge_gesture_init();
    int scan_index = first_scan_index_for_item(item);
    if (scan_index < 0) {
        log_append("no hidraw node for %s", item->name);
        ctm_gesture_log(NULL, "plug failed: no hidraw node for %s", item->name);
        return false;
    }
    return plug_in_scan_index(item, scan_index, item->key);
}

/* Plug ONE chosen hidraw interface (keyed per node, independent of the whole-
 * device plug). When: a sub-row Plug button — pick exactly which interface of a
 * composite device to bridge. */
bool plug_in_node(logical_device_t *item, int scan_index)
{
    if (!item || scan_index < 0 || scan_index >= g_scan.count) {
        return false;
    }
    char key[96];
    node_session_key(item, scan_index, key, sizeof(key));
    return plug_in_scan_index(item, scan_index, key);
}

/* The unplug gesture's worker and the plug-by-node entry point live in their
 * own file. They are features of ours rather than gaps in the bridge, so
 * keeping them out of here means an upstream change cannot collide with them,
 * and removing them is deleting one line.
 *
 * Included rather than compiled separately because they use the session table
 * and the logical device list, both private to this file.
 *
 * INCLUDED AT THE END, deliberately: the moved code calls plug_in_item(),
 * send_agent_command() and stop_session(), all defined above. Its own two
 * entry points are declared in ctm_state.h, so anything earlier in this file
 * can still call them.
 */
#include "ctm_gesture_worker.inl"
