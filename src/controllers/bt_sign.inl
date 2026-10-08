/* The checksum every Bluetooth output report to a DualSense or a DS4 ends
 * with: CRC32 (reflected, polynomial 0xedb88320) over the byte 0xa2 and then
 * the report up to the checksum, little-endian in the report's last four
 * bytes.
 *
 * ⛔ WHY ONE FILE (code review, 2026-10-05). There were three copies of it:
 * this one, one in the Bluetooth signal and one in the microphone safety. The
 * other two each built a lookup table the first time they ran, with no lock,
 * and each copied the whole report into a buffer just to put 0xa2 in front.
 * One copy can be tested; three can drift.
 *
 * ⓘ Bit by bit, no table: a 398-byte report is a few microseconds, and there
 * is nothing to build or race over. Its own file so a test can pin it. */

#include <stddef.h>
#include <stdint.h>

static inline uint32_t bt_crc32_step(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

/* Sign a whole report in place: the checksum goes in its last four bytes. */
static inline void bt_sign_output(uint8_t *data, size_t len)
{
    if (!data || len < 8) return;
    const uint8_t seed = 0xa2;
    uint32_t crc = bt_crc32_step(0xffffffffu, &seed, 1);
    crc = ~bt_crc32_step(crc, data, len - 4);
    data[len - 4] = (uint8_t)(crc & 0xffu);
    data[len - 3] = (uint8_t)((crc >> 8) & 0xffu);
    data[len - 2] = (uint8_t)((crc >> 16) & 0xffu);
    data[len - 1] = (uint8_t)((crc >> 24) & 0xffu);
}
