/* Tests for the Bluetooth output reports' checksum.
 *
 * ⭐ WHY THIS EXISTS (code review, 2026-10-05). There were three copies of this
 * checksum, and they became one, src/controllers/bt_sign.inl. A pad drops a
 * Bluetooth report whose checksum is wrong without a word, so a slip here is
 * a light, a tone or a rumble that silently stops. These pin it against the
 * standard CRC32 check value and against the table-driven copy the Bluetooth
 * signal and the microphone safety used, at each report length that is signed.
 *
 * ➡️ Build and run:  cc tests/test_bt_sign.c && ./a.out
 *    Or:             ./tests/run-tests.sh
 */

#include <stdio.h>
#include <string.h>

#include "../src/controllers/bt_sign.inl"

static int checks, failed;

static void ok(int cond, const char *what)
{
    ++checks;
    if (!cond) { ++failed; printf("  FAIL  %s\n", what); }
    else       {           printf("  ok    %s\n", what); }
}

/* The copy the Bluetooth signal and the microphone safety carried until
 * 2026-10-06, kept here as the reference: a lookup table, over 0xa2 and the
 * report copied in behind it. */
static uint32_t reference_crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int built = 0;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = 1;
    }
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

static int signs_like_reference(size_t len)
{
    uint8_t report[600], seeded[601];
    for (size_t i = 0; i < len; ++i) report[i] = (uint8_t)(i * 37u + 11u);
    seeded[0] = 0xa2;
    memcpy(&seeded[1], report, len - 4);
    const uint32_t want = reference_crc32(seeded, len - 3);
    bt_sign_output(report, len);
    const uint32_t got = (uint32_t)report[len - 4] | ((uint32_t)report[len - 3] << 8) |
                         ((uint32_t)report[len - 2] << 16) | ((uint32_t)report[len - 1] << 24);
    int body_kept = 1;
    for (size_t i = 0; i < len - 4; ++i) {
        if (report[i] != (uint8_t)(i * 37u + 11u)) body_kept = 0;
    }
    return got == want && body_kept;
}

int main(void)
{
    printf("\nthe CRC32 itself: the standard check value\n");
    const char *nine = "123456789";
    ok((bt_crc32_step(0xffffffffu, (const uint8_t *)nine, 9) ^ 0xffffffffu) == 0xcbf43926u,
       "CRC32(\"123456789\") is 0xcbf43926");

    printf("\nsigned as the old copies signed, at every length that is signed\n");
    ok(signs_like_reference(78),  "78 bytes: a DualSense's 0x31 report, patched");
    ok(signs_like_reference(142), "142 bytes: the microphone safety's packet");
    ok(signs_like_reference(398), "398 bytes: the Bluetooth signal's report");
    ok(signs_like_reference(270), "270 bytes: a DS4's 0x14 audio report");
    ok(signs_like_reference(462), "462 bytes: a DS4's 0x17 audio report");

    printf("\ntoo short to carry a checksum: left alone\n");
    uint8_t tiny[7] = {1, 2, 3, 4, 5, 6, 7};
    bt_sign_output(tiny, sizeof(tiny));
    ok(tiny[3] == 4 && tiny[6] == 7, "untouched");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
