/* Tests for what setting the listener's address does to what is known about it.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). A stream to a different PC read
 * ONLINE before anything had asked that PC, because "probed" was never cleared.
 * These pin the rule in src/shared/agent_endpoint.inl: a new address is unknown
 * until it answers, the same one keeps its last reading, and no address knows
 * nothing.
 *
 * ➡️ Build and run:  cc tests/test_agent_endpoint.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>

#include "../src/shared/agent_endpoint.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

int main(void)
{
    bool online, probed, changed;

    printf("\nthe first address: assumed online, not yet probed\n");
    online = false; probed = false;
    changed = agent_endpoint_set("", 48054, "192.168.1.10", 48054, &online, &probed);
    ok(changed, "changed");
    ok(online && !probed, "online assumed, probed cleared");

    printf("\na different PC after a probed one: unknown again\n");
    online = true; probed = true;
    changed = agent_endpoint_set("192.168.1.10", 48054, "192.168.1.20", 48054, &online, &probed);
    ok(changed, "changed");
    ok(!probed, "probed cleared, so the status reads unknown, not ONLINE");

    printf("\nthe same PC on another port is another listener\n");
    online = true; probed = true;
    changed = agent_endpoint_set("192.168.1.10", 48054, "192.168.1.10", 48154, &online, &probed);
    ok(changed && !probed, "changed, and probed cleared");

    printf("\nthe same PC again keeps its last reading\n");
    online = false; probed = true;
    changed = agent_endpoint_set("192.168.1.10", 48054, "192.168.1.10", 48054, &online, &probed);
    ok(!changed, "not changed");
    ok(!online && probed, "still OFFLINE, as last read (it was reset to ONLINE)");
    online = true; probed = true;
    agent_endpoint_set("192.168.1.10", 48054, "192.168.1.10", 48054, &online, &probed);
    ok(online && probed, "and still ONLINE when that was the reading");

    printf("\nno address knows nothing\n");
    online = true; probed = true;
    changed = agent_endpoint_set("192.168.1.10", 48054, "", 48054, &online, &probed);
    ok(changed && !online && !probed, "cleared: changed, offline, not probed");
    online = true; probed = true;
    changed = agent_endpoint_set("", 48054, NULL, 48054, &online, &probed);
    ok(!changed && !online && !probed, "none before and none now: not changed, nothing known");

    printf("\n%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
