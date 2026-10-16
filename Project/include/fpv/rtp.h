#ifndef FPV_RTP_H
#define FPV_RTP_H
#include <stddef.h>
#include <stdint.h>

#define FPV_MAX_NAL (4u * 1024u * 1024u)
#define FPV_MAX_PACKET 4096u
typedef int (*fpv_packet_callback)(const uint8_t *, size_t, void *);
typedef int (*fpv_nal_callback)(const uint8_t *, size_t, uint32_t, int, void *);
typedef struct {
    uint16_t sequence;
    uint32_t ssrc;
    unsigned payload_type;
    size_t mtu;
    fpv_packet_callback emit;
    void *opaque;
} fpv_packetizer;
typedef struct {
    const uint8_t *payload;
    size_t payload_size;
    uint16_t sequence;
    uint32_t timestamp, ssrc;
    unsigned payload_type;
    int marker;
} fpv_rtp_view;
typedef struct fpv_depacketizer fpv_depacketizer;
typedef struct {
    uint64_t packets, nalus, malformed, lost, duplicates, reordered, abandoned, timeouts,
        ssrc_changes;
} fpv_rtp_stats;

int fpv_rtp_parse(const uint8_t *packet, size_t size, fpv_rtp_view *view);
int fpv_packetizer_init(fpv_packetizer *p, size_t mtu, unsigned payload_type, uint32_t ssrc,
                        uint16_t sequence, fpv_packet_callback emit, void *opaque);
int fpv_packetize(fpv_packetizer *p, const uint8_t *nal, size_t size, uint32_t timestamp,
                  int marker);
fpv_depacketizer *fpv_depacketizer_create(size_t max_nal, unsigned payload_type,
                                          unsigned timeout_ms, fpv_nal_callback emit, void *opaque);
int fpv_depacketizer_push(fpv_depacketizer *d, const uint8_t *packet, size_t size, uint64_t now_ms);
void fpv_depacketizer_expire(fpv_depacketizer *d, uint64_t now_ms);
const fpv_rtp_stats *fpv_depacketizer_stats(const fpv_depacketizer *d);
void fpv_depacketizer_destroy(fpv_depacketizer *d);
#endif
