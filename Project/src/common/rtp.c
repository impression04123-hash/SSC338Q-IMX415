/* RFC 7798 H.265: single NAL, AP without DONL, FU. */
#include "fpv/rtp.h"
#include <stdlib.h>
#include <string.h>
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)n;
}
static void put32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
}
static int valid_nal(const uint8_t *p, size_t n)
{
    return n >= 2 && !(p[0] & 128u) && (p[1] & 7u) && ((p[0] >> 1) & 63u) < 48;
}
int fpv_rtp_parse(const uint8_t *p, size_t n, fpv_rtp_view *v)
{
    if (!p || !v || n < 12 || (p[0] >> 6) != 2)
        return -1;
    size_t off = 12 + 4u * (p[0] & 15u), end = n;
    if (off > n)
        return -1;
    if (p[0] & 16u) {
        if (n - off < 4)
            return -1;
        size_t ext = 4u + 4u * get16(p + off + 2);
        if (ext > n - off)
            return -1;
        off += ext;
    }
    if (p[0] & 32u) {
        unsigned pad = p[n - 1];
        if (!pad || pad > end - off)
            return -1;
        end -= pad;
    }
    if (end <= off)
        return -1;
    v->payload = p + off;
    v->payload_size = end - off;
    v->payload_type = p[1] & 127u;
    v->marker = !!(p[1] & 128u);
    v->sequence = get16(p + 2);
    v->timestamp = get32(p + 4);
    v->ssrc = get32(p + 8);
    return 0;
}
int fpv_packetizer_init(fpv_packetizer *p, size_t mtu, unsigned pt, uint32_t ssrc, uint16_t seq,
                        fpv_packet_callback emit, void *opaque)
{
    if (!p || !emit || mtu < 64 || mtu > FPV_MAX_PACKET || pt > 127)
        return -1;
    p->sequence = seq;
    p->ssrc = ssrc;
    p->payload_type = pt;
    p->mtu = mtu;
    p->emit = emit;
    p->opaque = opaque;
    return 0;
}
static void header(fpv_packetizer *p, uint8_t *b, uint32_t ts, int mark)
{
    b[0] = 0x80;
    b[1] = (uint8_t)(p->payload_type | (mark ? 128u : 0u));
    put16(b + 2, p->sequence++);
    put32(b + 4, ts);
    put32(b + 8, p->ssrc);
}
int fpv_packetize(fpv_packetizer *p, const uint8_t *nal, size_t n, uint32_t ts, int marker)
{
    uint8_t b[FPV_MAX_PACKET];
    if (!p || !p->emit || p->mtu < 64 || p->mtu > FPV_MAX_PACKET || p->payload_type > 127 || !nal ||
        n > FPV_MAX_NAL || !valid_nal(nal, n))
        return -1;
    if (n <= p->mtu - 12) {
        header(p, b, ts, marker);
        memcpy(b + 12, nal, n);
        return p->emit(b, n + 12, p->opaque);
    }
    size_t off = 2, chunk = p->mtu - 15;
    while (off < n) {
        size_t count = n - off;
        if (count > chunk)
            count = chunk;
        int last = off + count == n;
        header(p, b, ts, last && marker);
        b[12] = (uint8_t)((nal[0] & 0x81u) | (49u << 1));
        b[13] = nal[1];
        b[14] = (uint8_t)(((nal[0] >> 1) & 63u) | (off == 2 ? 128u : 0u) | (last ? 64u : 0u));
        memcpy(b + 15, nal + off, count);
        if (p->emit(b, count + 15, p->opaque) < 0)
            return -1;
        off += count;
    }
    return 0;
}
struct fpv_depacketizer {
    uint8_t *buffer;
    size_t used, max;
    unsigned pt, timeout;
    int sequence_valid, active;
    uint16_t expected;
    uint32_t ssrc, timestamp;
    uint8_t h0, h1;
    uint64_t last_ms;
    fpv_nal_callback emit;
    void *opaque;
    fpv_rtp_stats stats;
};
static void abandon(fpv_depacketizer *d)
{
    if (d->active)
        d->stats.abandoned++;
    d->active = 0;
    d->used = 0;
}
fpv_depacketizer *fpv_depacketizer_create(size_t max, unsigned pt, unsigned timeout,
                                          fpv_nal_callback emit, void *opaque)
{
    if (max < 2 || max > FPV_MAX_NAL || pt > 127 || !timeout || !emit)
        return NULL;
    fpv_depacketizer *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->buffer = malloc(max + 4);
    if (!d->buffer) {
        free(d);
        return NULL;
    }
    d->max = max;
    d->pt = pt;
    d->timeout = timeout;
    d->emit = emit;
    d->opaque = opaque;
    d->buffer[0] = d->buffer[1] = d->buffer[2] = 0;
    d->buffer[3] = 1;
    return d;
}
void fpv_depacketizer_expire(fpv_depacketizer *d, uint64_t now)
{
    if (d && d->active && now >= d->last_ms && now - d->last_ms >= d->timeout) {
        d->stats.timeouts++;
        abandon(d);
    }
}
static int deliver(fpv_depacketizer *d, const uint8_t *p, size_t n, uint32_t ts, int marker)
{
    if (n > d->max)
        return -1;
    memcpy(d->buffer + 4, p, n);
    if (d->emit(d->buffer, n + 4, ts, marker, d->opaque) < 0)
        return -1;
    d->stats.nalus++;
    return 0;
}
int fpv_depacketizer_push(fpv_depacketizer *d, const uint8_t *packet, size_t n, uint64_t now)
{
    fpv_rtp_view v;
    if (!d)
        return -1;
    fpv_depacketizer_expire(d, now);
    d->stats.packets++;
    if (fpv_rtp_parse(packet, n, &v) || v.payload_type != d->pt)
        goto malformed;
    if (d->sequence_valid && v.ssrc != d->ssrc) {
        d->stats.ssrc_changes++;
        abandon(d);
        d->sequence_valid = 0;
    }
    if (d->sequence_valid) {
        uint16_t delta = (uint16_t)(v.sequence - d->expected);
        if (delta >= 32768u) {
            if (v.sequence == (uint16_t)(d->expected - 1))
                d->stats.duplicates++;
            else
                d->stats.reordered++;
            return 0;
        }
        if (delta) {
            d->stats.lost += delta;
            abandon(d);
        }
    }
    d->sequence_valid = 1;
    d->ssrc = v.ssrc;
    d->expected = (uint16_t)(v.sequence + 1);
    const uint8_t *p = v.payload;
    size_t len = v.payload_size;
    if (len < 2 || (p[0] & 128u) || !(p[1] & 7u))
        goto malformed;
    unsigned type = (p[0] >> 1) & 63u;
    if (type < 48) {
        abandon(d);
        if (!valid_nal(p, len) || len > d->max)
            goto malformed;
        return deliver(d, p, len, v.timestamp, v.marker);
    }
    if (type == 48) {
        abandon(d);
        size_t off = 2;
        unsigned count = 0, min_layer = 63, min_tid = 7;
        /* Atomic validation prevents partial output from malformed aggregation. */
        while (off < len) {
            if (len - off < 2)
                goto malformed;
            size_t size = get16(p + off);
            off += 2;
            if (size > len - off || size > d->max || !valid_nal(p + off, size))
                goto malformed;
            unsigned layer = ((unsigned)(p[off] & 1u) << 5) | (p[off + 1] >> 3),
                     tid = p[off + 1] & 7u;
            if (layer < min_layer)
                min_layer = layer;
            if (tid < min_tid)
                min_tid = tid;
            off += size;
            count++;
        }
        if (count < 2 || min_layer != (((unsigned)(p[0] & 1u) << 5) | (p[1] >> 3)) ||
            min_tid != (p[1] & 7u))
            goto malformed;
        off = 2;
        while (off < len) {
            size_t size = get16(p + off);
            off += 2;
            if (deliver(d, p + off, size, v.timestamp, v.marker && off + size == len) < 0)
                return -1;
            off += size;
        }
        return 0;
    }
    if (type == 49) {
        if (len < 4)
            goto malformed;
        unsigned fu = p[2], original = fu & 63u;
        int start = !!(fu & 128u), end = !!(fu & 64u);
        if (original >= 48 || (start && end) || (v.marker && !end))
            goto malformed;
        uint8_t h0 = (uint8_t)((p[0] & 0x81u) | (original << 1)), h1 = p[1];
        if (start) {
            abandon(d);
            d->active = 1;
            d->used = 2;
            d->buffer[4] = h0;
            d->buffer[5] = h1;
            d->timestamp = v.timestamp;
            d->h0 = h0;
            d->h1 = h1;
        } else if (!d->active)
            return 0;
        if (d->timestamp != v.timestamp || d->h0 != h0 || d->h1 != h1 || len - 3 > d->max - d->used)
            goto malformed;
        memcpy(d->buffer + 4 + d->used, p + 3, len - 3);
        d->used += len - 3;
        d->last_ms = now;
        if (end) {
            size_t total = d->used;
            d->active = 0;
            d->used = 0;
            if (d->emit(d->buffer, total + 4, v.timestamp, v.marker, d->opaque) < 0)
                return -1;
            d->stats.nalus++;
        }
        return 0;
    }
malformed:
    d->stats.malformed++;
    abandon(d);
    return -1;
}
const fpv_rtp_stats *fpv_depacketizer_stats(const fpv_depacketizer *d)
{
    return d ? &d->stats : NULL;
}
void fpv_depacketizer_destroy(fpv_depacketizer *d)
{
    if (d) {
        free(d->buffer);
        free(d);
    }
}
