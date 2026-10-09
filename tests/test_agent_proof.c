/* Tests for the proof a TV sends with a bridge command.
 *
 * ⭐ WHY THIS EXISTS. The listener lets a TV from the internet bridge only
 * with a proof it is paired with the PC (code review, 2026-10-05). The
 * signature is over text this side builds and the listener rebuilds, so one
 * byte different on either side shuts out a paired TV with nothing visibly
 * wrong. These pin src/shared/agent_proof.inl: which commands carry a proof,
 * reading the listener's challenge (and NOT reading an older listener's
 * refusal as one), the signed text byte for byte, and the line sent.
 *
 * ⓘ The signed text is the listener's paired_tv.inl signed_text(); its own
 * tests check openssl's signatures over exactly these bytes.
 *
 * ➡️ Build and run:  cc tests/test_agent_proof.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>

#include "../src/shared/agent_proof.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

int main(void)
{
    printf("\nwhich commands carry a proof\n");
    ok(agent_proof_wanted("BRIDGE_START ds5 48056 ctm-ds5-3"), "BRIDGE_START");
    ok(agent_proof_wanted("BRIDGE_STOP ctm-ds5-3"), "BRIDGE_STOP");
    ok(!agent_proof_wanted("STATUS"), "not the probe");
    ok(!agent_proof_wanted("PROOF_CHALLENGE"), "not the challenge itself");
    ok(!agent_proof_wanted("BRIDGE_STARTX"), "not a longer word");
    ok(!agent_proof_wanted(NULL), "not nothing");

    printf("\nthe listener's challenge\n");
    char nonce[AGENT_PROOF_NONCE_LEN + 1];
    ok(agent_proof_nonce("OK NONCE 00112233445566778899aabbccddeeff\n", nonce, sizeof(nonce)) &&
       strcmp(nonce, "00112233445566778899aabbccddeeff") == 0, "read, newline and all");
    ok(agent_proof_nonce("OK NONCE 00112233445566778899aabbccddeeff", nonce, sizeof(nonce)), "without the newline");
    ok(!agent_proof_nonce("ERR unknown command\n", nonce, sizeof(nonce)), "an older listener's refusal is no challenge");
    ok(!agent_proof_nonce("OK NONCE 0011", nonce, sizeof(nonce)), "too short");
    ok(!agent_proof_nonce("OK NONCE 00112233445566778899AABBCCDDEEFF\n", nonce, sizeof(nonce)), "upper case is not what the listener sends");
    ok(!agent_proof_nonce("OK NONCE 00112233445566778899aabbccddeeff00\n", nonce, sizeof(nonce)), "too long");
    ok(!agent_proof_nonce("OK NONCE 00112233445566778899aabbccddeefg\n", nonce, sizeof(nonce)), "not hex");
    ok(!agent_proof_nonce("OK NONCE 00112233445566778899aabbccddeeff\n", nonce, 8), "no room");
    ok(!agent_proof_nonce(NULL, nonce, sizeof(nonce)), "no reply");

    printf("\nthe signed text, byte for byte\n");
    char text[200];
    const size_t n = agent_proof_signed_text("00112233445566778899aabbccddeeff",
                                             "BRIDGE_START ds5 48056 ctm-ds5-3", text, sizeof(text));
    const char *want = "DS5-USBIP proof 1\n00112233445566778899aabbccddeeff\nBRIDGE_START ds5 48056 ctm-ds5-3";
    ok(n == strlen(want) && strcmp(text, want) == 0, "a fixed first line, the nonce, the command");
    ok(agent_proof_signed_text("00112233445566778899aabbccddeeff", "BRIDGE_START ds5 48056 ctm-ds5-3",
                               text, 20) == 0, "0 when it does not fit");

    printf("\nthe line sent\n");
    char line[600];
    const size_t m = agent_proof_line("BRIDGE_STOP ctm-ds5-3", "00112233445566778899aabbccddeeff",
                                      "0842278520b2043b5d3a153a71b4d87f", "QLhk==", line, sizeof(line));
    ok(m > 0 && strcmp(line, "BRIDGE_STOP ctm-ds5-3 PROOF=00112233445566778899aabbccddeeff:"
                             "0842278520b2043b5d3a153a71b4d87f:QLhk==") == 0, "the command, then the proof");
    ok(agent_proof_line("BRIDGE_STOP ctm-ds5-3", "x", "y", "z", line, 10) == 0, "0 when it does not fit");
    /* ⓘ A full one: a 2048-bit key's signature is 344 base64 characters, and
     * the listener's line holds 2047. */
    char sig[345];
    memset(sig, 'A', 344);
    sig[344] = '\0';
    ok(agent_proof_line("BRIDGE_START ds5_usb 48056 ctm-ds5-123", "00112233445566778899aabbccddeeff",
                        "0842278520b2043b5d3a153a71b4d87f", sig, line, sizeof(line)) > 400,
       "a real-sized proof fits the line");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
