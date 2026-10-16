#define _POSIX_C_SOURCE 200809L
#include "fpv/io.h"
#include "fpv/rtp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr);                             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static int fixture(fpv_annexb *reader, const unsigned char *data, size_t size)
{
    char path[] = "/tmp/fpv-annexb-test-XXXXXX";
    int fd = mkstemp(path);
    FILE *file;
    int result;
    if (fd < 0)
        return -1;
    file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(path);
        return -1;
    }
    result = size && fwrite(data, 1, size, file) != size;
    if (fclose(file))
        result = 1;
    if (result) {
        unlink(path);
        return -1;
    }
    result = fpv_annexb_open(reader, path);
    unlink(path);
    return result;
}

static void delimiters_eof_rewind(void)
{
    /* Catches a reader consuming the next NAL header with its delimiter. */
    static const unsigned char source[] = {0, 0,    0,    1,    0x40, 0x01, 0x55, 0,    0, 3,
                                           1, 0x66, 0,    0,    1,    0x42, 0x01, 0x77, 0, 0,
                                           0, 1,    0x26, 0x01, 0x80, 0x88, 0,    0};
    static const unsigned char first[] = {0x40, 0x01, 0x55, 0, 0, 3, 1, 0x66};
    static const unsigned char second[] = {0x42, 0x01, 0x77};
    static const unsigned char third[] = {0x26, 0x01, 0x80, 0x88};
    fpv_annexb r;
    int round;
    CHECK(fixture(&r, source, sizeof(source)) == 0);
    if (!r.file)
        return;
    for (round = 0; round < 2; ++round) {
        CHECK(fpv_annexb_next(&r) == 1);
        CHECK(r.size == sizeof(first));
        CHECK(r.size == sizeof(first) && !memcmp(r.nal, first, sizeof(first)));
        CHECK(fpv_annexb_next(&r) == 1);
        CHECK(r.size == sizeof(second) && !memcmp(r.nal, second, sizeof(second)));
        CHECK(fpv_annexb_next(&r) == 1);
        CHECK(r.size == sizeof(third) && !memcmp(r.nal, third, sizeof(third)));
        CHECK(fpv_annexb_next(&r) == 0);
        CHECK(fpv_annexb_next(&r) == 0);
        if (!round)
            CHECK(fpv_annexb_rewind(&r) == 0);
    }
    fpv_annexb_close(&r);
    fpv_annexb_close(&r);
    CHECK(!r.file && !r.nal);
}

static void empty_and_malformed(void)
{
    static const unsigned char garbage[] = {0x40, 0x01, 0x55};
    static const unsigned char short_nal[] = {0, 0, 1, 0x40};
    static const unsigned char consecutive[] = {0, 0, 1, 0, 0, 0, 1, 0x40, 1};
    static const unsigned char startcode_only[] = {0, 0, 0, 1};
    static const unsigned char final_empty[] = {0, 0, 1, 0x40, 1, 0x55, 0, 0, 1};
    static const unsigned char zeros_only[] = {0, 0, 0, 0};
    struct {
        const unsigned char *bytes;
        size_t size;
        const char *name;
    } bad[] = {{garbage, sizeof(garbage), "missing start code"},
               {short_nal, sizeof(short_nal), "short HEVC header"},
               {consecutive, sizeof(consecutive), "empty consecutive NAL"},
               {startcode_only, sizeof(startcode_only), "start code without NAL"},
               {zeros_only, sizeof(zeros_only), "zero bytes without start code"}};
    fpv_annexb r;
    size_t i;
    CHECK(fixture(&r, NULL, 0) == 0);
    if (r.file) {
        CHECK(fpv_annexb_next(&r) == 0);
        CHECK(fpv_annexb_rewind(&r) == 0);
        CHECK(fpv_annexb_next(&r) == 0);
        fpv_annexb_close(&r);
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(fixture(&r, bad[i].bytes, bad[i].size) == 0);
        if (!r.file)
            continue;
        if (fpv_annexb_next(&r) != -1) {
            fprintf(stderr, "malformed Annex B accepted: %s\n", bad[i].name);
            ++failures;
        }
        fpv_annexb_close(&r);
    }
    CHECK(fixture(&r, final_empty, sizeof(final_empty)) == 0);
    if (r.file) {
        CHECK(fpv_annexb_next(&r) == 1);
        CHECK(fpv_annexb_next(&r) == -1);
        fpv_annexb_close(&r);
    }
}

static void size_limit_and_io_failures(void)
{
    /* Catches off-by-one at the actual 4 MiB limit, with no smaller test cap. */
    const size_t size = (size_t)FPV_MAX_NAL + 5;
    unsigned char *source = malloc(size);
    fpv_annexb r;
    CHECK(source != NULL);
    if (!source)
        return;
    memset(source, 0x55, size);
    source[0] = source[1] = source[2] = 0;
    source[3] = 1;
    source[4] = 0x40;
    source[5] = 1;
    CHECK(fixture(&r, source, size - 1) == 0);
    if (r.file) {
        CHECK(fpv_annexb_next(&r) == 1);
        CHECK(r.size == FPV_MAX_NAL);
        CHECK(fpv_annexb_next(&r) == 0);
        fpv_annexb_close(&r);
    }
    CHECK(fixture(&r, source, size) == 0);
    if (r.file) {
        CHECK(fpv_annexb_next(&r) == -1);
        fpv_annexb_close(&r);
    }
    free(source);
    CHECK(fpv_annexb_open(&r, "/nonexistent/fpv-io-test/input.h265") == -1);
    fpv_annexb_close(&r);
    /* Reading a directory is an fopen success on Linux but a read failure. */
    if (!fpv_annexb_open(&r, "/tmp")) {
        CHECK(fpv_annexb_next(&r) == -1);
        fpv_annexb_close(&r);
    }
}

static void datagram_truncation(int unix_transport)
{
    /* Catches treating a truncated packet as valid or leaving its tail queued. */
    fpv_config cfg;
    fpv_socket rx = {-1}, tx = {-1};
    unsigned char large[1024], small[16], result[32];
    char error[256] = {0};
    fpv_config_defaults(&cfg);
    memset(large, 0x55, sizeof(large));
    memset(small, 0x77, sizeof(small));
    if (unix_transport) {
        strcpy(cfg.transport, "unix");
        snprintf(cfg.unix_name, sizeof(cfg.unix_name), "fpv-io-%ld", (long)getpid());
    } else {
        struct sockaddr_in addr;
        socklen_t length = sizeof(addr);
        cfg.port = 0; /* Bind a free local port; no hard-coded test port races. */
        CHECK(fpv_socket_open(&rx, &cfg, 1, error, sizeof(error)) == 0);
        if (rx.fd < 0)
            return;
        CHECK(getsockname(rx.fd, (struct sockaddr *)&addr, &length) == 0);
        cfg.port = (int)ntohs(addr.sin_port);
    }
    if (unix_transport)
        CHECK(fpv_socket_open(&rx, &cfg, 1, error, sizeof(error)) == 0);
    if (rx.fd < 0)
        return;
    CHECK(fpv_socket_open(&tx, &cfg, 0, error, sizeof(error)) == 0);
    if (tx.fd < 0) {
        fpv_socket_close(&rx);
        return;
    }
    CHECK(fpv_socket_receive(&rx, result, sizeof(result), 0) == 0);
    CHECK(fpv_socket_send(&tx, large, sizeof(large)) == 0);
    CHECK(fpv_socket_send(&tx, small, sizeof(small)) == 0);
    CHECK(fpv_socket_receive(&rx, result, sizeof(result), 1000) == -2);
    CHECK(fpv_socket_receive(&rx, result, sizeof(result), 1000) == (ssize_t)sizeof(small));
    CHECK(!memcmp(result, small, sizeof(small)));
    CHECK(fpv_socket_receive(&rx, result, sizeof(result), 0) == 0);
    fpv_socket_close(&tx);
    fpv_socket_close(&rx);
    CHECK(tx.fd == -1 && rx.fd == -1);
    fpv_socket_close(&tx);
    fpv_socket_close(&rx);
    if (unix_transport) {
        CHECK(fpv_socket_open(&rx, &cfg, 1, error, sizeof(error)) == 0);
        fpv_socket_close(&rx);
    }
}

static void socket_open_failure_cleanup(void)
{
    fpv_config cfg;
    fpv_socket s = {-1};
    char error[256];
    fpv_config_defaults(&cfg);
    strcpy(cfg.host, "invalid-address");
    CHECK(fpv_socket_open(&s, &cfg, 0, error, sizeof(error)) == -1);
    CHECK(s.fd == -1);
    fpv_socket_close(&s);
    strcpy(cfg.transport, "unix");
    snprintf(cfg.unix_name, sizeof(cfg.unix_name), "fpv-io-absent-%ld", (long)getpid());
    CHECK(fpv_socket_open(&s, &cfg, 0, error, sizeof(error)) == -1);
    CHECK(s.fd == -1);
    fpv_socket_close(&s);
}

int main(void)
{
    delimiters_eof_rewind();
    empty_and_malformed();
    size_limit_and_io_failures();
    datagram_truncation(0);
    datagram_truncation(1);
    socket_open_failure_cleanup();
    if (failures) {
        fprintf(stderr, "%d I/O test failure(s)\n", failures);
        return 1;
    }
    puts("PASS Annex B boundaries/EOF/rewind/errors/limits and UDP/Unix truncation/cleanup");
    return 0;
}
