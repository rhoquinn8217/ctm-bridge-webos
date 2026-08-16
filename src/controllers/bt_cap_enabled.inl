/* ⛔⛔ EXPERIMENTAL BRANCH ONLY -- mic-capture-experimental.
 *
 * ⚠️ IF YOU ARE READING THIS AS A MERGE CONFLICT, THE ANSWER IS: KEEP THIS FILE.
 * The stable branch has no arming code, so it has no use for a switch that
 * enables it. This file exists only here, and nothing on stable will ever
 * delete or edit it.
 *
 * ⭐ WHY IT IS ITS OWN FILE. ctm_mic_safety.inl is shared: everything in it is
 * a guard, and every guard exists on stable too. The switch is the opposite --
 * the one piece that only makes sense where arming exists. Keeping them apart
 * means a change to the guards on stable can never collide with the switch.
 */

#if MICSAFE_EXPERIMENTAL_ARMING
/* ⛔ THE RUNTIME SWITCH. Default OFF, and it stays off unless the TV app asks.
 *
 * ⭐ READ IN EXACTLY ONE PLACE -- when a controller is handed to the host. Every
 * other mention of capture in this project turns it OFF, and that asymmetry is
 * deliberate: a bug in the reading of this flag can fail to arm, which is
 * harmless, but can never fail to disarm.
 *
 * ⚠️ It is not read again mid-session. The setting is sampled as the bridge
 * starts, so a change applies to the next stream, not the current one. */
static bool g_bt_capture_enabled = false;

void ctm_bt_capture_set_enabled(bool on)
{
    g_bt_capture_enabled = on;
    micsafe_log("microphone capture %s", on ? "ENABLED by the app" : "disabled");
}

bool ctm_bt_capture_enabled(void)
{
    return g_bt_capture_enabled;
}
#else
/* ⓘ REACHED ONLY IF SOMEONE SETS THE DEFINE TO 0 ON THIS BRANCH, to compile the
 * arming out while keeping everything else. The TV app calls the setter without
 * knowing that, so stubs keep it linking -- and returning false keeps every
 * guard behaving exactly as stable's does.
 *
 * ⛔ NOT a stable-branch fallback: this file does not exist there at all. */
void ctm_bt_capture_set_enabled(bool on) { (void)on; }
bool ctm_bt_capture_enabled(void) { return false; }
#endif
