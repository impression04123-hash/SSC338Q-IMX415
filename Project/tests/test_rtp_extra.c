/* Independent edge-case suite. Build with src/common/rtp.c and -Iinclude.
 * Run without arguments for all cases, or select one case by its name.
 * `packetizer-overflow` intentionally exercises a corrupt public packetizer
 * state and should be run under ASan until fpv_packetize validates its MTU. */
#include "fpv/rtp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x);                                \
            return -1;                                                                             \
        }                                                                                          \
    } while (0)
typedef struct {
    int calls, marker[8], fail;
    size_t size[8];
    uint8_t data[8][64];
} output;
static int receive(const uint8_t *p, size_t n, uint32_t ts, int marker, void *opaque)
{
    output *o = opaque;
    (void)ts;
    if (o->fail || o->calls >= 8 || n > 64)
        return -1;
    memcpy(o->data[o->calls], p, n);
    o->marker[o->calls] = marker;
    o->size[o->calls++] = n;
    return 0;
}
static int emitted(const uint8_t *p, size_t n, void *opaque)
{
    (void)p;
    (void)n;
    ++*(int *)opaque;
    return 0;
}
static size_t packet(uint8_t *b, uint16_t seq, uint32_t ts, int mark, const uint8_t *payload,
                     size_t size)
{
    memset(b, 0, 12);
    b[0] = 0x80;
    b[1] = (uint8_t)(96 | (mark ? 128 : 0));
    b[2] = (uint8_t)(seq >> 8);
    b[3] = (uint8_t)seq;
    b[4] = (uint8_t)(ts >> 24);
    b[5] = (uint8_t)(ts >> 16);
    b[6] = (uint8_t)(ts >> 8);
    b[7] = (uint8_t)ts;
    b[11] = 1;
    memcpy(b + 12, payload, size);
    return 12 + size;
}
static int push(fpv_depacketizer *d, uint16_t seq, uint32_t ts, int marker, const uint8_t *payload,
                size_t size, uint64_t now)
{
    uint8_t b[128];
    size_t n = packet(b, seq, ts, marker, payload, size);
    return fpv_depacketizer_push(d, b, n, now);
}

static int fu_capacity(void)
{
    const uint8_t start[] = {98, 1, 0x93, 0xaa, 0xbb};
    const uint8_t end[] = {98, 1, 0x53, 0xcc, 0xdd};
    const uint8_t too_big[] = {98, 1, 0x53, 0xcc, 0xdd, 0xee};
    const uint8_t single[] = {0x26, 1, 9};
    output o = {0};
    fpv_depacketizer *d = fpv_depacketizer_create(6, 96, 10, receive, &o);
    CHECK(d);
    CHECK(push(d, 65535, 5, 0, start, sizeof start, 0) == 0);
    CHECK(push(d, 0, 5, 1, end, sizeof end, 1) == 0);
    CHECK(o.calls == 1 && o.size[0] == 10 && o.marker[0] == 1);
    CHECK(memcmp(o.data[0], (uint8_t[]){0, 0, 0, 1, 0x26, 1, 0xaa, 0xbb, 0xcc, 0xdd}, 10) == 0);
    CHECK(push(d, 1, 6, 0, start, sizeof start, 2) == 0);
    CHECK(push(d, 2, 6, 1, too_big, sizeof too_big, 3) < 0);
    CHECK(o.calls == 1);
    CHECK(push(d, 3, 7, 1, single, sizeof single, 4) == 0);
    CHECK(o.calls == 2 && fpv_depacketizer_stats(d)->malformed == 1);
    fpv_depacketizer_destroy(d);
    d = fpv_depacketizer_create(2, 96, 10, receive, &o);
    CHECK(d && push(d, 1, 1, 0, start, sizeof start, 0) < 0);
    fpv_depacketizer_destroy(d);
    return 0;
}
static int fu_identity(void)
{
    const uint8_t start[] = {98, 1, 0x93, 0xaa};
    const uint8_t end[] = {98, 1, 0x53, 0xbb};
    for (int variant = 0; variant < 4; ++variant) {
        output o = {0};
        uint8_t altered[sizeof end];
        memcpy(altered, end, sizeof end);
        if (variant == 0)
            altered[1] = 2; /* temporal ID */
        if (variant == 1)
            altered[0] |= 1; /* layer ID */
        if (variant == 2)
            altered[2] ^= 1; /* original NAL type */
        fpv_depacketizer *d = fpv_depacketizer_create(64, 96, 10, receive, &o);
        CHECK(d && push(d, 10, 100, 0, start, sizeof start, 0) == 0);
        CHECK(push(d, 11, variant == 3 ? 101U : 100U, 1, altered, sizeof altered, 1) < 0);
        CHECK(o.calls == 0 && fpv_depacketizer_stats(d)->abandoned == 1);
        fpv_depacketizer_destroy(d);
    }
    return 0;
}
static int fu_invalid_flags(void)
{
    const uint8_t bad[][4] = {
        {98, 1, 0x93, 0xaa}, /* start cannot have RTP marker */
        {98, 1, 0xd3, 0xaa}, /* S and E simultaneously */
        {98, 1, 0xb0, 0xaa}, /* FuType = AP */
        {98, 1, 0x93, 0xaa}  /* header only: no FU payload */
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        output o = {0};
        fpv_depacketizer *d = fpv_depacketizer_create(64, 96, 10, receive, &o);
        CHECK(d && push(d, 0, 0, i == 0, bad[i], i == 3 ? 3U : 4U, 0) < 0);
        CHECK(o.calls == 0);
        fpv_depacketizer_destroy(d);
    }
    return 0;
}
static int timeout_edge(void)
{
    const uint8_t start[] = {98, 1, 0x93, 0xaa}, end[] = {98, 1, 0x53, 0xbb};
    output o = {0};
    fpv_depacketizer *d = fpv_depacketizer_create(64, 96, 10, receive, &o);
    CHECK(d && push(d, 0, 0, 0, start, sizeof start, 100) == 0);
    fpv_depacketizer_expire(d, 99); /* no unsigned underflow on backwards time */
    fpv_depacketizer_expire(d, 109);
    CHECK(fpv_depacketizer_stats(d)->timeouts == 0);
    CHECK(push(d, 1, 0, 1, end, sizeof end, 110) == 0);
    CHECK(o.calls == 0 && fpv_depacketizer_stats(d)->timeouts == 1);
    fpv_depacketizer_destroy(d);
    return 0;
}
static int ap_atomic(void)
{
    const uint8_t cases[][12] = {
        {96, 1, 0, 3, 0x40, 1, 0xaa, 0, 4, 0x42, 1, 0xbb}, /* truncated second */
        {96, 1, 0, 3, 0x40, 1, 0xaa, 0, 3, 96, 1, 0xbb},   /* nested AP */
        {96, 1, 0, 3, 0x40, 1, 0xaa, 0, 3, 0x42, 0, 0xbb}, /* zero temporal ID */
        {96, 1, 0, 3, 0x40, 1, 0xaa, 0, 3, 0xc2, 1, 0xbb}  /* forbidden bit */
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        output o = {0};
        fpv_depacketizer *d = fpv_depacketizer_create(64, 96, 10, receive, &o);
        CHECK(d && push(d, 0, 0, 1, cases[i], sizeof cases[i], 0) < 0);
        CHECK(o.calls == 0);
        fpv_depacketizer_destroy(d);
    }
    /* Individually valid NALs, but second one exceeds receiver max. */
    const uint8_t bounded[] = {96, 1, 0, 3, 0x40, 1, 0xaa, 0, 4, 0x42, 1, 0xbb, 0xcc};
    output o = {0};
    fpv_depacketizer *d = fpv_depacketizer_create(3, 96, 10, receive, &o);
    CHECK(d && push(d, 0, 0, 1, bounded, sizeof bounded, 0) < 0 && o.calls == 0);
    fpv_depacketizer_destroy(d);
    return 0;
}
static int ap_single_unit(void)
{
    /* RFC 7798 section 4.4.2: AP MUST carry at least two aggregation units. */
    const uint8_t ap[] = {96, 1, 0, 3, 0x40, 1, 0xaa};
    output o = {0};
    fpv_depacketizer *d = fpv_depacketizer_create(64, 96, 10, receive, &o);
    CHECK(d);
    int rc = push(d, 0, 0, 1, ap, sizeof ap, 0);
    fpv_depacketizer_destroy(d);
    CHECK(rc < 0 && o.calls == 0);
    return 0;
}
static int ap_header_tid(void)
{
    /* AP header TID must be the lowest of all aggregated NAL TIDs. */
    const uint8_t ap[] = {96, 3, 0, 3, 0x40, 1, 0xaa, 0, 3, 0x42, 2, 0xbb};
    output o = {0};
    fpv_depacketizer *d = fpv_depacketizer_create(64, 96, 10, receive, &o);
    CHECK(d);
    int rc = push(d, 0, 0, 1, ap, sizeof ap, 0);
    fpv_depacketizer_destroy(d);
    CHECK(rc < 0 && o.calls == 0);
    return 0;
}
static int ap_header_layer(void)
{
    /* Header layer=1, whereas both contained NALs have layer=0. */
    const uint8_t ap[] = {96, 9, 0, 3, 0x40, 1, 0xaa, 0, 3, 0x42, 1, 0xbb};
    output o = {0};
    fpv_depacketizer *d = fpv_depacketizer_create(64, 96, 10, receive, &o);
    CHECK(d);
    int rc = push(d, 0, 0, 1, ap, sizeof ap, 0);
    fpv_depacketizer_destroy(d);
    CHECK(rc < 0 && o.calls == 0);
    return 0;
}
static int sequence_gap_and_late(void)
{
    const uint8_t start[] = {98, 1, 0x93, 0xaa}, end[] = {98, 1, 0x53, 0xbb};
    const uint8_t middle[] = {98, 1, 0x13, 0xcc}, single[] = {0x26, 1, 9};
    output o = {0};
    fpv_depacketizer *d = fpv_depacketizer_create(64, 96, 10, receive, &o);
    CHECK(d && push(d, 65534, 1, 0, start, sizeof start, 0) == 0);
    CHECK(push(d, 0, 1, 1, end, sizeof end, 1) == 0);           /* missing 65535 */
    CHECK(push(d, 65535, 1, 0, middle, sizeof middle, 2) == 0); /* too late */
    CHECK(o.calls == 0);
    CHECK(push(d, 1, 2, 1, single, sizeof single, 3) == 0 && o.calls == 1);
    CHECK(fpv_depacketizer_stats(d)->lost == 1);
    CHECK(fpv_depacketizer_stats(d)->reordered == 1);
    CHECK(fpv_depacketizer_stats(d)->abandoned == 1);
    fpv_depacketizer_destroy(d);
    return 0;
}
static int packetizer_state(void)
{
    const uint8_t nal[] = {0x26, 1};
    const size_t invalid[] = {0, 12, 63, FPV_MAX_PACKET + 1U, SIZE_MAX};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        fpv_packetizer tx;
        int calls = 0;
        CHECK(fpv_packetizer_init(&tx, 128, 96, 1, 0, emitted, &calls) == 0);
        tx.mtu = invalid[i];
        CHECK(fpv_packetize(&tx, nal, sizeof nal, 0, 1) < 0 && calls == 0);
    }
    fpv_packetizer tx;
    int calls = 0;
    CHECK(fpv_packetizer_init(&tx, 128, 96, 1, 0, emitted, &calls) == 0);
    tx.payload_type = 128;
    CHECK(fpv_packetize(&tx, nal, sizeof nal, 0, 1) < 0 && calls == 0);
    return 0;
}
static int packetizer_overflow(void)
{
    fpv_packetizer tx;
    int calls = 0;
    uint8_t *nal = malloc(8192);
    CHECK(nal);
    memset(nal, 0x55, 8192);
    nal[0] = 0x26;
    nal[1] = 1;
    CHECK(fpv_packetizer_init(&tx, 128, 96, 1, 0, emitted, &calls) == 0);
    tx.mtu = 0;
    int rc = fpv_packetize(&tx, nal, 8192, 0, 1);
    free(nal);
    CHECK(rc < 0 && calls == 0);
    return 0;
}
static int headers_ext_padding(void)
{
    uint8_t b[128] = {0};
    fpv_rtp_view v;
    b[0] = 0x9f;
    b[1] = 96;                           /* 15 CSRC plus extension */
    CHECK(fpv_rtp_parse(b, 72, &v) < 0); /* no extension header */
    b[74] = b[75] = 0xff;
    CHECK(fpv_rtp_parse(b, sizeof b, &v) < 0); /* 65535 extension words */
    b[0] = 0xa0;                               /* padding */
    b[12] = 0x26;
    b[13] = 1;
    b[14] = 0;
    CHECK(fpv_rtp_parse(b, 15, &v) < 0); /* zero pad count */
    b[14] = 3;
    CHECK(fpv_rtp_parse(b, 15, &v) < 0); /* all payload is padding */
    b[14] = 1;
    CHECK(fpv_rtp_parse(b, 15, &v) == 0 && v.payload_size == 2);
    return 0;
}
static int callback_failure(void)
{
    const uint8_t start[] = {98, 1, 0x93, 0xaa}, end[] = {98, 1, 0x53, 0xbb};
    const uint8_t single[] = {0x26, 1, 9};
    output o = {.fail = 1};
    fpv_depacketizer *d = fpv_depacketizer_create(64, 96, 10, receive, &o);
    CHECK(d && push(d, 0, 0, 0, start, sizeof start, 0) == 0);
    CHECK(push(d, 1, 0, 1, end, sizeof end, 1) < 0);
    CHECK(fpv_depacketizer_stats(d)->nalus == 0);
    o.fail = 0;
    CHECK(push(d, 2, 1, 1, single, sizeof single, 2) == 0 && o.calls == 1);
    fpv_depacketizer_destroy(d);
    return 0;
}
int main(int argc, char **argv)
{
    struct {
        const char *name;
        int (*run)(void);
    } cases[] = {{"fu-capacity", fu_capacity},
                 {"fu-identity", fu_identity},
                 {"fu-invalid-flags", fu_invalid_flags},
                 {"timeout-edge", timeout_edge},
                 {"ap-atomic", ap_atomic},
                 {"ap-single-unit", ap_single_unit},
                 {"ap-header-tid", ap_header_tid},
                 {"ap-header-layer", ap_header_layer},
                 {"sequence-gap-late", sequence_gap_and_late},
                 {"packetizer-state", packetizer_state},
                 {"headers-ext-padding", headers_ext_padding},
                 {"callback-failure", callback_failure}};
    if (argc == 2 && !strcmp(argv[1], "packetizer-overflow"))
        return packetizer_overflow() ? 1 : 0;
    int failures = 0, ran = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        if (argc == 2 && strcmp(argv[1], cases[i].name))
            continue;
        ++ran;
        int rc = cases[i].run();
        failures += rc != 0;
        printf("%s %s\n", rc ? "FAIL" : "PASS", cases[i].name);
    }
    if (!ran)
        return 2;
    return failures ? 1 : 0;
}
