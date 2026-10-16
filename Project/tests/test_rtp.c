#include "fpv/rtp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x);                            \
            return -1;                                                                             \
        }                                                                                          \
    } while (0)
typedef struct {
    unsigned char data[128][FPV_MAX_PACKET];
    size_t size[128];
    int count;
} packets;
typedef struct {
    unsigned char data[8][8192];
    size_t size[8];
    uint32_t ts[8];
    int mark[8], count;
} output;
static int save_packet(const uint8_t *p, size_t n, void *v)
{
    packets *s = v;
    if (s->count >= 128 || n > FPV_MAX_PACKET)
        return -1;
    memcpy(s->data[s->count], p, n);
    s->size[s->count++] = n;
    return 0;
}
static int save_nal(const uint8_t *p, size_t n, uint32_t t, int m, void *v)
{
    output *s = v;
    if (s->count >= 8 || n > 8192)
        return -1;
    memcpy(s->data[s->count], p, n);
    s->size[s->count] = n;
    s->ts[s->count] = t;
    s->mark[s->count++] = m;
    return 0;
}
static int make_packets(packets *p, size_t n, uint16_t seq)
{
    unsigned char nal[4096];
    fpv_packetizer tx;
    if (n > sizeof nal)
        return -1;
    for (size_t i = 0; i < n; i++)
        nal[i] = (unsigned char)(i * 17u);
    nal[0] = 0x26;
    nal[1] = 1;
    memset(p, 0, sizeof *p);
    if (fpv_packetizer_init(&tx, 128, 96, 0x12345678u, seq, save_packet, p))
        return -1;
    return fpv_packetize(&tx, nal, n, 9000, 1);
}
static int roundtrip(void)
{
    packets p;
    output out = {0};
    unsigned char expected[2004] = {0, 0, 0, 1};
    REQUIRE(make_packets(&p, 2000, 65534) == 0);
    REQUIRE(p.count > 2);
    fpv_depacketizer *rx = fpv_depacketizer_create(8192, 96, 250, save_nal, &out);
    REQUIRE(rx);
    for (int i = 0; i < p.count; i++) {
        REQUIRE(p.size[i] <= 128);
        REQUIRE(fpv_depacketizer_push(rx, p.data[i], p.size[i], (uint64_t)i) >= 0);
    }
    for (size_t i = 0; i < 2000; i++)
        expected[i + 4] = (unsigned char)(i * 17u);
    expected[4] = 0x26;
    expected[5] = 1;
    REQUIRE(out.count == 1);
    REQUIRE(out.size[0] == sizeof expected);
    REQUIRE(memcmp(out.data[0], expected, sizeof expected) == 0);
    REQUIRE(out.ts[0] == 9000 && out.mark[0] == 1);
    REQUIRE(fpv_depacketizer_stats(rx)->lost == 0);
    fpv_depacketizer_destroy(rx);
    return 0;
}
static int single(void)
{
    packets p;
    output out = {0};
    REQUIRE(make_packets(&p, 20, 1) == 0);
    REQUIRE(p.count == 1);
    fpv_depacketizer *rx = fpv_depacketizer_create(100, 96, 20, save_nal, &out);
    REQUIRE(rx);
    REQUIRE(fpv_depacketizer_push(rx, p.data[0], p.size[0], 0) >= 0);
    REQUIRE(out.count == 1 && out.size[0] == 24);
    fpv_depacketizer_destroy(rx);
    return 0;
}
static int loss_and_recovery(void)
{
    packets p, q;
    output out = {0};
    REQUIRE(make_packets(&p, 1000, 100) == 0);
    REQUIRE(make_packets(&q, 20, (uint16_t)(100 + p.count)) == 0);
    fpv_depacketizer *rx = fpv_depacketizer_create(4096, 96, 50, save_nal, &out);
    REQUIRE(rx);
    for (int i = 0; i < p.count; i++)
        if (i != 2)
            REQUIRE(fpv_depacketizer_push(rx, p.data[i], p.size[i], (uint64_t)i) >= 0);
    REQUIRE(out.count == 0);
    REQUIRE(fpv_depacketizer_push(rx, q.data[0], q.size[0], 100) >= 0);
    REQUIRE(out.count == 1);
    REQUIRE(fpv_depacketizer_stats(rx)->lost == 1);
    REQUIRE(fpv_depacketizer_stats(rx)->abandoned >= 1);
    fpv_depacketizer_destroy(rx);
    return 0;
}
static int duplicates(void)
{
    packets p;
    output out = {0};
    REQUIRE(make_packets(&p, 800, 44) == 0);
    fpv_depacketizer *rx = fpv_depacketizer_create(4096, 96, 50, save_nal, &out);
    REQUIRE(rx);
    for (int i = 0; i < p.count; i++) {
        REQUIRE(fpv_depacketizer_push(rx, p.data[i], p.size[i], 1) >= 0);
        REQUIRE(fpv_depacketizer_push(rx, p.data[i], p.size[i], 1) >= 0);
    }
    REQUIRE(out.count == 1);
    REQUIRE(fpv_depacketizer_stats(rx)->duplicates == (uint64_t)p.count);
    fpv_depacketizer_destroy(rx);
    return 0;
}
static int timeout(void)
{
    packets p;
    output out = {0};
    REQUIRE(make_packets(&p, 800, 4) == 0);
    fpv_depacketizer *rx = fpv_depacketizer_create(4096, 96, 50, save_nal, &out);
    REQUIRE(rx);
    REQUIRE(fpv_depacketizer_push(rx, p.data[0], p.size[0], 100) >= 0);
    fpv_depacketizer_expire(rx, 151);
    for (int i = 1; i < p.count; i++)
        REQUIRE(fpv_depacketizer_push(rx, p.data[i], p.size[i], 152) >= 0);
    REQUIRE(!out.count);
    REQUIRE(fpv_depacketizer_stats(rx)->timeouts == 1);
    fpv_depacketizer_destroy(rx);
    return 0;
}
static int headers(void)
{
    unsigned char b[32] = {0xB1, 0xE0, 0, 9, 0, 0, 0, 10, 0,    0, 0, 11, 1, 2, 3, 4,
                           0xBE, 0xDE, 0, 1, 0, 0, 0, 0,  0x40, 1, 0, 0,  0, 0, 0, 6};
    fpv_rtp_view v;
    REQUIRE(fpv_rtp_parse(b, sizeof b, &v) == 0);
    REQUIRE(v.payload_size == 2 && v.payload[0] == 0x40 && v.sequence == 9 && v.marker == 1);
    b[31] = 20;
    REQUIRE(fpv_rtp_parse(b, sizeof b, &v) < 0);
    b[0] = 0x41;
    REQUIRE(fpv_rtp_parse(b, sizeof b, &v) < 0);
    for (size_t n = 0; n < 12; n++)
        REQUIRE(fpv_rtp_parse(b, n, &v) < 0);
    return 0;
}
static int ap(void)
{
    unsigned char b[] = {0x80, 0xE0, 0, 1, 0,    0, 0,    4, 0, 0,    0, 1,
                         0x60, 1,    0, 3, 0x40, 1, 0xAA, 0, 3, 0x42, 1, 0xBB};
    output out = {0};
    fpv_depacketizer *rx = fpv_depacketizer_create(100, 96, 10, save_nal, &out);
    REQUIRE(rx);
    REQUIRE(fpv_depacketizer_push(rx, b, sizeof b, 0) >= 0);
    REQUIRE(out.count == 2 && out.size[0] == 7 && out.mark[0] == 0 && out.mark[1] == 1);
    fpv_depacketizer_destroy(rx);
    memset(&out, 0, sizeof out);
    b[20] = 30;
    rx = fpv_depacketizer_create(100, 96, 10, save_nal, &out);
    REQUIRE(rx);
    REQUIRE(fpv_depacketizer_push(rx, b, sizeof b, 0) < 0);
    REQUIRE(out.count == 0);
    fpv_depacketizer_destroy(rx);
    return 0;
}
static int invalid_fu(void)
{
    packets p;
    output out = {0};
    REQUIRE(make_packets(&p, 800, 1) == 0);
    p.data[0][14] |= 0x40;
    fpv_depacketizer *rx = fpv_depacketizer_create(100, 96, 10, save_nal, &out);
    REQUIRE(rx);
    REQUIRE(fpv_depacketizer_push(rx, p.data[0], p.size[0], 0) < 0);
    REQUIRE(!out.count);
    fpv_depacketizer_destroy(rx);
    return 0;
}
static int ssrc_change(void)
{
    packets p, q;
    output out = {0};
    REQUIRE(make_packets(&p, 800, 9) == 0);
    REQUIRE(make_packets(&q, 20, 1) == 0);
    q.data[0][11] ^= 1;
    fpv_depacketizer *rx = fpv_depacketizer_create(2000, 96, 10, save_nal, &out);
    REQUIRE(rx);
    REQUIRE(fpv_depacketizer_push(rx, p.data[0], p.size[0], 0) >= 0);
    REQUIRE(fpv_depacketizer_push(rx, q.data[0], q.size[0], 1) >= 0);
    REQUIRE(out.count == 1);
    REQUIRE(fpv_depacketizer_stats(rx)->ssrc_changes == 1);
    fpv_depacketizer_destroy(rx);
    return 0;
}
int main(void)
{
    int (*tests[])(void) = {roundtrip, single, loss_and_recovery, duplicates, timeout,
                            headers,   ap,     invalid_fu,        ssrc_change};
    for (size_t i = 0; i < sizeof tests / sizeof *tests; i++) {
        if (tests[i]())
            return 1;
    }
    puts("PASS RTP: single/FU/AP, loss, duplicates, wrap, timeout, headers, SSRC");
    return 0;
}
