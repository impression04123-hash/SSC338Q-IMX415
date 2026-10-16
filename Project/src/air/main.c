#define _POSIX_C_SOURCE 200809L
#include "fpv/config.h"
#include "fpv/io.h"
#include "fpv/rtp.h"
#include "fpv/platform.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fresh_ssrc(uint32_t *value)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t offset = 0;
    while (offset < sizeof *value) {
        ssize_t n = read(fd, (uint8_t *)value + offset, sizeof *value - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        offset += (size_t)n;
    }
    close(fd);
    return 0;
}

static volatile sig_atomic_t stopping;
static void stop(int signo)
{
    (void)signo;
    stopping = 1;
}
typedef struct {
    fpv_socket socket;
    fpv_packetizer packetizer;
    uint64_t packets, bytes, frames, nalus;
} air;
static int emit_packet(const uint8_t *data, size_t size, void *opaque)
{
    air *a = opaque;
    if (fpv_socket_send(&a->socket, data, size))
        return -1;
    a->packets++;
    a->bytes += size;
    return 0;
}
static int emit_nal(const uint8_t *data, size_t size, uint32_t ts, int marker, void *opaque)
{
    air *a = opaque;
    if (fpv_packetize(&a->packetizer, data, size, ts, marker))
        return -1;
    a->nalus++;
    if (marker)
        a->frames++;
    return 0;
}
static void stats(const air *a)
{
    fprintf(stderr,
            "{\"role\":\"air\",\"frames\":%" PRIu64 ",\"nalus\":%" PRIu64 ",\"packets\":%" PRIu64
            ",\"bytes\":%" PRIu64 "}\n",
            a->frames, a->nalus, a->packets, a->bytes);
}
static int file_stream(air *a, const fpv_config *c)
{
    fpv_annexb reader;
    if (fpv_annexb_open(&reader, c->source)) {
        perror("open H265 source");
        return -1;
    }
    uint8_t *current = malloc(FPV_MAX_NAL);
    if (!current) {
        fpv_annexb_close(&reader);
        return -1;
    }
    int status = fpv_annexb_next(&reader), seen_vcl = 0, result = 0;
    uint64_t start = fpv_now_ms(), report = start;
    if (status == 0) {
        fprintf(stderr, "empty Annex B source\n");
        result = -1;
    }
    if (status > 0) {
        fputs("FPV_READY\n", stderr);
        fflush(stderr);
    }
    while (status > 0 && !stopping) {
        size_t size = reader.size;
        memcpy(current, reader.nal, size);
        /* This replay backend handles one base video layer, as used by the camera. */
        if ((current[0] & 1u) || (current[1] >> 3)) {
            fprintf(stderr, "file replay supports H.265 layer 0 only\n");
            result = -1;
            break;
        }
        unsigned type = (current[0] >> 1) & 63u;
        if (type <= 31)
            seen_vcl = 1;
        int next = fpv_annexb_next(&reader);
        if (next < 0) {
            fprintf(stderr, "invalid/oversized Annex B source\n");
            result = -1;
            break;
        }
        unsigned nt = next > 0 ? (reader.nal[0] >> 1) & 63u : 0;
        int prefix = (nt >= 32 && nt <= 37) || nt == 39 || (nt >= 41 && nt <= 44);
        int first_slice = nt <= 31 && reader.size >= 3 && (reader.nal[2] & 128u);
        int boundary = next > 0 && seen_vcl && (prefix || first_slice);
        int marker = !next || boundary;
        uint32_t timestamp = (uint32_t)(a->frames * 90000u / (unsigned)c->fps);
        if (emit_nal(current, size, timestamp, marker, a)) {
            perror("RTP transmit");
            result = -1;
            break;
        }
        if (marker) {
            seen_vcl = 0;
            if (c->max_frames && a->frames >= (uint64_t)c->max_frames)
                break;
            fpv_wait_until(start + a->frames * 1000u / (unsigned)c->fps);
        }
        uint64_t now = fpv_now_ms();
        if (now - report >= (uint64_t)c->stats_interval_ms) {
            stats(a);
            report = now;
        }
        if (!next && c->loop && !stopping) {
            if (fpv_annexb_rewind(&reader)) {
                result = -1;
                break;
            }
            next = fpv_annexb_next(&reader);
        }
        status = next;
    }
    if (status < 0) {
        fprintf(stderr, "invalid Annex B source\n");
        result = -1;
    }
    free(current);
    fpv_annexb_close(&reader);
    return result;
}
int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--config")) {
        fprintf(stderr, "usage: fpv-air --config PATH\n");
        return 2;
    }
    fpv_config cfg;
    char error[256];
    if (fpv_config_load(argv[2], &cfg, error, sizeof error) || strcmp(cfg.role, "air")) {
        fprintf(stderr, "configuration: %s\n", *error ? error : "role must be air");
        return 2;
    }
    if (strcmp(cfg.backend, "file")) {
#ifndef FPV_HAVE_SIGMASTAR
        fprintf(stderr, "sigmastar backend is unavailable: rebuild with FPV_ENABLE_SIGMASTAR=ON "
                        "and the target toolchain\n");
        return 2;
#endif
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    air a = {0};
    a.socket.fd = -1;
    if (fpv_socket_open(&a.socket, &cfg, 0, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    uint16_t sequence = (uint16_t)fpv_now_ms();
    uint32_t session_ssrc = cfg.ssrc;
    if (cfg.random_ssrc && fresh_ssrc(&session_ssrc)) {
        perror("create RTP session SSRC");
        fpv_socket_close(&a.socket);
        return 1;
    }
    fprintf(stderr, "RTP session SSRC=%" PRIu32 "\n", session_ssrc);
    if (fpv_packetizer_init(&a.packetizer, (size_t)cfg.mtu, (unsigned)cfg.payload_type,
                            session_ssrc, sequence, emit_packet, &a)) {
        fpv_socket_close(&a.socket);
        return 1;
    }
    int result = 0;
    if (!strcmp(cfg.backend, "file"))
        result = file_stream(&a, &cfg);
#ifdef FPV_HAVE_SIGMASTAR
    else {
        if (fpv_capture_open(&cfg, emit_nal, &a, error, sizeof error)) {
            fprintf(stderr, "%s\n", error);
            result = -1;
        } else {
            /* The controller may attach OSD only after VPE initialization. */
            fputs("FPV_READY\n", stderr);
            fflush(stderr);
            uint64_t report = fpv_now_ms();
            while (!stopping && (!cfg.max_frames || a.frames < (uint64_t)cfg.max_frames)) {
                if (fpv_capture_poll(100) < 0) {
                    fprintf(stderr, "MI capture failed\n");
                    result = -1;
                    break;
                }
                uint64_t now = fpv_now_ms();
                if (now - report >= (uint64_t)cfg.stats_interval_ms) {
                    stats(&a);
                    report = now;
                }
            }
        }
        fpv_capture_close();
    }
#endif
    stats(&a);
    fpv_socket_close(&a.socket);
    return result ? 1 : 0;
}
