/* Deterministic malformed-input stress and random single/FU roundtrips. */
#include "fpv/rtp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t seed = 0x52747031u;
static uint32_t random32(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
typedef struct {
    fpv_depacketizer *rx;
    uint8_t *expected;
    size_t size;
    int calls, failed;
    uint64_t now;
} state;
static int sink(const uint8_t *b, size_t n, uint32_t ts, int marker, void *opaque)
{
    state *s = opaque;
    (void)ts;
    if (s->expected && (n != s->size + 4 || memcmp(b, (uint8_t[]){0, 0, 0, 1}, 4) ||
                        memcmp(b + 4, s->expected, s->size) || !marker))
        s->failed = 1;
    s->calls++;
    return 0;
}
static int packet(const uint8_t *b, size_t n, void *opaque)
{
    state *s = opaque;
    return fpv_depacketizer_push(s->rx, b, n, s->now++);
}
int main(void)
{
    uint8_t bytes[8192];
    state s = {0};
    s.rx = fpv_depacketizer_create(sizeof bytes, 96, 250, sink, &s);
    if (!s.rx)
        return 1;
    for (unsigned i = 0; i < 50000; i++) {
        size_t n = random32() % sizeof bytes;
        for (size_t j = 0; j < n; j++)
            bytes[j] = (uint8_t)random32();
        if (i % 2 == 0 && n >= 12) {
            bytes[0] = 0x80;
            bytes[1] = 96;
        }
        fpv_depacketizer_push(s.rx, bytes, n, i);
        fpv_depacketizer_expire(s.rx, (uint64_t)i + 300);
    }
    fpv_depacketizer_destroy(s.rx);
    s.calls = 0;
    s.rx = fpv_depacketizer_create(sizeof bytes, 96, 250, sink, &s);
    if (!s.rx)
        return 1;
    fpv_packetizer tx;
    if (fpv_packetizer_init(&tx, 400, 96, 123, 65530, packet, &s))
        return 1;
    for (unsigned i = 0; i < 500; i++) {
        size_t n = 2 + random32() % (sizeof bytes - 2);
        for (size_t j = 0; j < n; j++)
            bytes[j] = (uint8_t)random32();
        bytes[0] = 0x26;
        bytes[1] = 1;
        s.expected = bytes;
        s.size = n;
        if (fpv_packetize(&tx, bytes, n, i * 3000, 1) || s.failed) {
            fprintf(stderr, "random roundtrip %u failed\n", i);
            return 1;
        }
    }
    fpv_depacketizer_destroy(s.rx);
    if (s.calls != 500)
        return 1;
    puts("PASS: 50000 malformed inputs and 500 randomized NAL roundtrips");
    return 0;
}
