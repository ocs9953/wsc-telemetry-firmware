/* Host-side test harness for the REAL Core/Src/cobs.c and Core/Src/packet_builder.c
 * Compiled with native gcc, not the ARM cross toolchain,
 * so it actually executes on this machine and can fuzz thousands of cases quickly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cobs.h"
#include "packet_builder.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

/* ---------------------------------------------------------------------------
 * Property 4/5: COBS round-trip + no-zero-byte-in-output
 * --------------------------------------------------------------------------- */
static void test_cobs(void)
{
    printf("[COBS] round-trip + zero-byte-free, 20000 random cases...\n");
    srand(42);
    int cases = 0;
    for (int trial = 0; trial < 20000; trial++)
    {
        uint8_t src[16];
        int len = 16; /* fixed LOG size, but also fuzz shorter lengths below */
        if (trial % 4 == 0) len = rand() % 17; /* 0..16, edge lengths too */
        for (int i = 0; i < len; i++) src[i] = (uint8_t)(rand() % 256);

        uint8_t encoded[32];
        size_t enc_len = COBS_Encode(src, (size_t)len, encoded);

        /* Property 5: no 0x00 in the encoded region */
        for (size_t i = 0; i < enc_len; i++)
            CHECK(encoded[i] != 0x00, "COBS output contains 0x00");

        uint8_t decoded[32];
        size_t dec_len = COBS_Decode(encoded, enc_len, decoded);

        /* Property 4: round trip */
        CHECK(dec_len == (size_t)len, "COBS decoded length mismatch");
        CHECK(memcmp(decoded, src, (size_t)len) == 0, "COBS round-trip data mismatch");
        cases++;
    }
    printf("  %d cases checked.\n", cases);

    /* Decoder must reject malformed input (0x00 inside "encoded" stream) */
    uint8_t bad[3] = {0x02, 0x00, 0x01};
    uint8_t out[8];
    size_t r = COBS_Decode(bad, sizeof(bad), out);
    CHECK(r == 0, "COBS_Decode should reject embedded 0x00 in data");
}

/* ---------------------------------------------------------------------------
 * Property 2: checksum integrity — CalcChecksum == VerifyChecksum, and any
 * single-bit flip in bytes[0..14] must break verification.
 * --------------------------------------------------------------------------- */
static void test_checksum(void)
{
    printf("[CRC-8] checksum integrity, single-bit-flip detection...\n");
    srand(7);
    int cases = 0;
    for (int trial = 0; trial < 2000; trial++)
    {
        uint8_t value[8];
        for (int i = 0; i < 8; i++) value[i] = (uint8_t)(rand() % 256);

        LOG pkt;
        PacketBuilder_Create(&pkt, (LOG_LEVEL)(rand() % 4), (LOG_SOURCE)(rand() % 13),
                             (uint8_t)(rand() % 256), value, 8);

        CHECK(PacketBuilder_VerifyChecksum(&pkt), "fresh packet must verify OK");
        CHECK(pkt.checksum == PacketBuilder_CalcChecksum(&pkt),
              "checksum field must equal CalcChecksum()");

        /* Flip every single bit across the 15 covered bytes; must always fail */
        uint8_t *raw = (uint8_t *)&pkt;
        for (int byte = 0; byte < 15; byte++)
        {
            for (int bit = 0; bit < 8; bit++)
            {
                raw[byte] ^= (uint8_t)(1u << bit);
                CHECK(!PacketBuilder_VerifyChecksum(&pkt),
                      "single-bit flip must invalidate checksum");
                raw[byte] ^= (uint8_t)(1u << bit); /* restore */
            }
        }
        cases++;
    }
    printf("  %d packets x 120 bit-flips checked.\n", cases);
}

static uint8_t crc8_reference(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
        {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void test_crc8_table_equivalence(void)
{
    printf("[CRC-8] table lookup == bitwise reference...\n");

    for (int i = 0; i < 256; i++)
    {
        LOG pkt;
        uint8_t raw[15];
        memset(&pkt, 0, sizeof(pkt));
        memset(raw, 0, sizeof(raw));
        raw[0] = (uint8_t)i;
        memcpy(&pkt, raw, sizeof(raw));
        CHECK(PacketBuilder_CalcChecksum(&pkt) == crc8_reference(raw, 15),
              "table CRC must match bitwise CRC for every byte value");
    }

    int cases = 0;
    for (int trial = 0; trial < 20000; trial++)
    {
        LOG pkt;
        uint8_t raw[15];
        for (int i = 0; i < 15; i++) raw[i] = (uint8_t)(rand() % 256);
        memset(&pkt, 0, sizeof(pkt));
        memcpy(&pkt, raw, sizeof(raw));
        CHECK(PacketBuilder_CalcChecksum(&pkt) == crc8_reference(raw, 15),
              "table CRC must match bitwise CRC for random payload");
        cases++;
    }

    {
        const uint8_t kat[9] = {'1','2','3','4','5','6','7','8','9'};
        CHECK(crc8_reference(kat, 9) == 0xF4, "reference CRC-8('123456789') == 0xF4");
    }

    printf("  256 exhaustive + %d random payloads + 1 known-answer vector.\n", cases);
}

/* ---------------------------------------------------------------------------
 * Property 3 + new int packers: pack/unpack round trips
 * --------------------------------------------------------------------------- */
static void test_packing(void)
{
    printf("[Pack] float2 / u16x4 / i16x4 / u32x2 round-trips...\n");
    srand(99);

    /* float2 */
    for (int trial = 0; trial < 5000; trial++)
    {
        float f1 = (float)(rand() % 200000 - 100000) / 100.0f;
        float f2 = (float)(rand() % 200000 - 100000) / 100.0f;
        uint8_t v[8];
        PacketBuilder_PackFloat2(v, f1, f2);
        float o1, o2;
        PacketBuilder_UnpackFloat2(v, &o1, &o2);
        CHECK(o1 == f1 && o2 == f2, "PackFloat2/UnpackFloat2 round-trip");
    }

    /* u16x4 */
    for (int trial = 0; trial < 5000; trial++)
    {
        uint16_t a = (uint16_t)(rand() % 65536);
        uint16_t b = (uint16_t)(rand() % 65536);
        uint16_t c = (uint16_t)(rand() % 65536);
        uint16_t d = (uint16_t)(rand() % 65536);
        uint8_t v[8];
        PacketBuilder_PackU16x4(v, a, b, c, d);
        uint16_t oa, ob, oc, od;
        memcpy(&oa, &v[0], 2); memcpy(&ob, &v[2], 2);
        memcpy(&oc, &v[4], 2); memcpy(&od, &v[6], 2);
        CHECK(oa == a && ob == b && oc == c && od == d, "PackU16x4 round-trip");
    }

    /* i16x4 — include boundary values explicitly */
    {
        int16_t a = INT16_MIN, b = INT16_MAX, c = 0, d = -1;
        uint8_t v[8];
        PacketBuilder_PackI16x4(v, a, b, c, d);
        int16_t oa, ob, oc, od;
        memcpy(&oa, &v[0], 2); memcpy(&ob, &v[2], 2);
        memcpy(&oc, &v[4], 2); memcpy(&od, &v[6], 2);
        CHECK(oa == a && ob == b && oc == c && od == d,
              "PackI16x4 boundary values (INT16_MIN/MAX/0/-1)");
    }
    for (int trial = 0; trial < 5000; trial++)
    {
        int16_t a = (int16_t)(rand() % 65536 - 32768);
        int16_t b = (int16_t)(rand() % 65536 - 32768);
        int16_t c = (int16_t)(rand() % 65536 - 32768);
        int16_t d = (int16_t)(rand() % 65536 - 32768);
        uint8_t v[8];
        PacketBuilder_PackI16x4(v, a, b, c, d);
        int16_t oa, ob, oc, od;
        memcpy(&oa, &v[0], 2); memcpy(&ob, &v[2], 2);
        memcpy(&oc, &v[4], 2); memcpy(&od, &v[6], 2);
        CHECK(oa == a && ob == b && oc == c && od == d, "PackI16x4 round-trip");
    }

    /* u32x2 */
    for (int trial = 0; trial < 5000; trial++)
    {
        uint32_t a = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        uint32_t b = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        uint8_t v[8];
        PacketBuilder_PackU32x2(v, a, b);
        uint32_t oa, ob;
        memcpy(&oa, &v[0], 4); memcpy(&ob, &v[4], 4);
        CHECK(oa == a && ob == b, "PackU32x2 round-trip");
    }
    printf("  20000+ pack/unpack cases checked.\n");
}

/* ---------------------------------------------------------------------------
 * Sanity: LOG struct really is 16 bytes packed (matches _Static_assert in types.h,
 * but re-check here since host compiler's struct packing rules could differ)
 * --------------------------------------------------------------------------- */
static void test_struct_layout(void)
{
    printf("[Layout] sizeof(LOG) == 16 on host compiler...\n");
    CHECK(sizeof(LOG) == 16, "sizeof(LOG) must be 16 on host gcc too");

    CHECK(LOG_QUEUE_MSG_WORDS * 4u == sizeof(LOG),
          "LOG_QUEUE_MSG_WORDS must cover exactly sizeof(LOG)");
}

int main(void)
{
    test_struct_layout();
    test_cobs();
    test_checksum();
    test_crc8_table_equivalence();
    test_packing();

    printf("\n=========================================\n");
    if (g_fail == 0)
    {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
