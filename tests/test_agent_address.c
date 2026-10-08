/* Tests for the listener's address: a dotted IPv4 address, or a name looked up.
 *
 * ⭐ WHY THIS EXISTS. A PC added in the app by name streamed and could never
 * bridge, because the commands to the listener took only a dotted address
 * (code review, 2026-10-05). These pin the four answers: an address is kept,
 * a name is looked up, and a name with no IPv4 address, or an IPv6 address,
 * is kept as given.
 *
 * ⓘ "localhost" is looked up through the hosts file, so it needs no network.
 * The unknown name asks the resolver and comes back an error with a network
 * or without one; only how long that takes differs.
 *
 * ➡️ Build and run:  cc tests/test_agent_address.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

#include "../src/shared/agent_address.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

int main(void)
{
    char out[64];
    const char *why = NULL;

    printf("\nan address is kept as it is\n");
    ok(agent_address_ipv4("192.168.1.10", out, sizeof(out), &why) == 0, "an IPv4 address: nothing looked up");
    ok(strcmp(out, "192.168.1.10") == 0, "and kept");

    printf("\na name is looked up\n");
    ok(agent_address_ipv4("localhost", out, sizeof(out), &why) == 1, "localhost: looked up");
    ok(strcmp(out, "127.0.0.1") == 0, "and is 127.0.0.1");

    printf("\na name with no IPv4 address is kept, and says why\n");
    ok(agent_address_ipv4("no-such-host.invalid", out, sizeof(out), &why) == -1, "an unknown name: not found");
    ok(strcmp(out, "no-such-host.invalid") == 0, "kept as given");
    ok(why != NULL && why[0] != '\0', "with a reason");
    ok(agent_address_ipv4("::1", out, sizeof(out), &why) == -1, "an IPv6 address: no IPv4 for it");
    ok(strcmp(out, "::1") == 0, "kept as given");

    printf("\n%d check(s), %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
