#define _POSIX_C_SOURCE 200809L
#include "fpv/config.h"
#include "fpv/io.h"
#include "fpv/rtp.h"
#include "fpv/platform.h"
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t stopping;
static void stop(int signo)
{
    (void)signo;
    stopping = 1;
}
typedef struct {
    FILE *file;
    uint64_t frames;
    int failure;
} ground;
static int emit_nal(const uint8_t *data, size_t size, uint32_t ts, int marker, void *opaque)
{
    ground *g = opaque;
    int result = -1;
    if (g->file)
        result = fwrite(data, 1, size, g->file) == size ? 0 : -1;
#ifdef FPV_HAVE_HISILICON
    else
        result = fpv_display_write(data, size, ts, marker);
#else
    (void)ts;
#endif
    if (result < 0) {
        g->failure = 1;
        return -1;
    }
    if (marker)
        g->frames++;
    return 0;
}
static void stats(const ground *g, const fpv_depacketizer *d, uint64_t oversized)
{
    const fpv_rtp_stats *s = fpv_depacketizer_stats(d);
    fprintf(stderr,
            "{\"role\":\"ground\",\"frames\":%" PRIu64 ",\"packets\":%" PRIu64 ",\"nalus\":%" PRIu64
            ",\"lost\":%" PRIu64 ",\"malformed\":%" PRIu64 ",\"abandoned\":%" PRIu64
            ",\"duplicates\":%" PRIu64 ",\"reordered\":%" PRIu64 ",\"timeouts\":%" PRIu64
            ",\"oversized\":%" PRIu64 "}\n",
            g->frames, s->packets, s->nalus, s->lost, s->malformed, s->abandoned, s->duplicates,
            s->reordered, s->timeouts, oversized);
}
int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--config")) {
        fprintf(stderr, "usage: fpv-ground --config PATH\n");
        return 2;
    }
    fpv_config cfg;
    char error[256];
    if (fpv_config_load(argv[2], &cfg, error, sizeof error) || strcmp(cfg.role, "ground")) {
        fprintf(stderr, "configuration: %s\n", *error ? error : "role must be ground");
        return 2;
    }
#ifndef FPV_HAVE_HISILICON
    if (strcmp(cfg.backend, "file")) {
        fprintf(stderr, "hisilicon backend is unavailable: rebuild with FPV_ENABLE_HISILICON=ON "
                        "and the target toolchain\n");
        return 2;
    }
#endif
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    fpv_socket socket = {-1};
    ground g = {0};
    int result = 0;
    if (fpv_socket_open(&socket, &cfg, 1, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    if (!strcmp(cfg.backend, "file")) {
        g.file = fopen(cfg.output, "wb");
        if (!g.file) {
            perror("open received H265");
            fpv_socket_close(&socket);
            return 1;
        }
    }
#ifdef FPV_HAVE_HISILICON
    else if (fpv_display_open(&cfg, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        fpv_socket_close(&socket);
        return 1;
    }
#endif
    fpv_depacketizer *rx = fpv_depacketizer_create(FPV_MAX_NAL, (unsigned)cfg.payload_type,
                                                   (unsigned)cfg.fragment_timeout_ms, emit_nal, &g);
    if (!rx) {
        result = 1;
        goto cleanup;
    }
    uint64_t last = fpv_now_ms(), report = last, oversized = 0;
    uint8_t packet[FPV_MAX_PACKET];
    while (!stopping && (!cfg.max_frames || g.frames < (uint64_t)cfg.max_frames)) {
        ssize_t n = fpv_socket_receive(&socket, packet, sizeof packet, 100);
        uint64_t now = fpv_now_ms();
        if (n == -1) {
            perror("RTP receive");
            result = 1;
            break;
        }
        if (n == -2)
            oversized++;
        if (n > 0) {
            if (fpv_depacketizer_push(rx, packet, (size_t)n, now) == 0)
                last = now;
            if (g.failure) {
                fprintf(stderr, "video sink failed\n");
                result = 1;
                break;
            }
        }
        fpv_depacketizer_expire(rx, now);
        if (now - report >= (uint64_t)cfg.stats_interval_ms) {
            stats(&g, rx, oversized);
            report = now;
        }
        if (cfg.idle_exit_ms && now - last >= (uint64_t)cfg.idle_exit_ms)
            break;
    }
    stats(&g, rx, oversized);
    fpv_depacketizer_destroy(rx);
cleanup:
    if (g.file) {
        if (fclose(g.file))
            result = 1;
    }
#ifdef FPV_HAVE_HISILICON
    else
        fpv_display_close();
#endif
    fpv_socket_close(&socket);
    return result;
}
