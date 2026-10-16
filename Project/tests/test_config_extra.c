#define _POSIX_C_SOURCE 200809L
#include "fpv/config.h"
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks, failures;
static char error[256];
static void check(int condition, const char *name)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL %s [%s]\n", name, error);
    }
}
static int load_text(const char *text, fpv_config *cfg)
{
    char path[] = "/tmp/fpv-config-extra-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        exit(2);
    }
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlink(path);
        exit(2);
    }
    if (fputs(text, file) < 0 || fclose(file)) {
        unlink(path);
        exit(2);
    }
    error[0] = 0;
    int result = fpv_config_load(path, cfg, error, sizeof(error));
    unlink(path);
    return result;
}
typedef struct {
    const char *name;
    size_t offset;
    int min, max;
} bound;
#define B(member, low, high)                                                                       \
    {                                                                                              \
        #member, offsetof(fpv_config, member), low, high                                           \
    }
int main(int argc, char **argv)
{
    fpv_config cfg;
    const bound bounds[] = {B(width, 64, 4096),
                            B(height, 64, 2160),
                            B(fps, 1, 120),
                            B(bitrate_kbps, 128, 100000),
                            B(gop, 1, 600),
                            B(sensor_mode, 0, 255),
                            B(display_buffers, 0, 8),
                            B(max_frames, 0, INT_MAX),
                            B(loop, 0, 1),
                            B(port, 1, 65535),
                            B(mtu, 64, 4096),
                            B(payload_type, 96, 127),
                            B(socket_rcvbuf, 4096, 67108864),
                            B(fragment_timeout_ms, 1, 60000),
                            B(wireless_enabled, 0, 1),
                            B(channel, 1, 196),
                            B(mcs, 0, 7),
                            B(fec_k, 1, 255),
                            B(fec_n, 1, 255),
                            B(radio_port, 0, 255),
                            B(txpower_mbm, -1, 5000),
                            B(osd_enabled, 0, 1),
                            B(serial_baud, 1200, 2000000),
                            B(osd_fps, 1, 50),
                            B(random_ssrc, 0, 1),
                            B(stats_interval_ms, 100, 60000),
                            B(idle_exit_ms, 0, INT_MAX),
                            B(restart_limit, 0, 100),
                            B(restart_delay_ms, 0, 60000)};
    fpv_config_defaults(&cfg);
    check(fpv_config_validate(&cfg, error, sizeof(error)) == 0, "defaults valid");
    for (size_t i = 0; i < sizeof(bounds) / sizeof(bounds[0]); i++) {
        fpv_config_defaults(&cfg);
        *(int *)((char *)&cfg + bounds[i].offset) = bounds[i].min - 1;
        check(fpv_config_validate(&cfg, error, sizeof(error)) < 0, bounds[i].name);
        if (bounds[i].max < INT_MAX) {
            fpv_config_defaults(&cfg);
            *(int *)((char *)&cfg + bounds[i].offset) = bounds[i].max + 1;
            check(fpv_config_validate(&cfg, error, sizeof(error)) < 0, bounds[i].name);
        }
    }
    const char *bad[] = {"role=air\n",
                         "[system\nrole=air\n",
                         "[]\n",
                         "[system] trailing\n",
                         "[system]\nrole\n",
                         "[video]\nfps_typo=30\n",
                         "[video]\nfps=30x\n",
                         "[video]\nfps=\n",
                         "[video]\nfps=2147483648\n",
                         "[video]\nfps=-2147483649\n",
                         "[video]\nfps=99999999999999999999999999999\n",
                         "[video]\nsource=\"missing quote\n",
                         "[video]\nsource=''\n",
                         "[system]\nrole=ground\n[video]\nbackend=sigmastar\n",
                         "[system]\nrole=air\n[video]\nbackend=hisilicon\n",
                         "[transport]\nssrc=4294967296\n",
                         "[transport]\nssrc=-1\n",
                         "[transport]\nssrc=0x100000000\n",
                         "[wireless]\nlink_id=0x1000000\n",
                         "[wireless]\nfec_k=13\nfec_n=12\n",
                         "[wireless]\nbandwidth=80\n",
                         "[wireless]\niface=wlan0;touch\n",
                         "[wireless]\ndriver=../mod\n",
                         "[video]\nwidth=1919\n",
                         "[video]\nheight=1079\n",
                         "[transport]\ntype=tcp\n",
                         "[video]\noutput_mode=2160p60\n",
                         "[transport]\nunix_name=''\n",
                         "[runtime]\nrun_dir=''\n",
                         "[osd]\nserial_device=''\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        check(load_text(bad[i], &cfg) < 0, bad[i]);
    check(load_text(" ; comment\r\n[system]\r\nrole = air\r\n[video]\r\nsource='a "
                    "path/#literal;name.h265'\r\n[transport]\r\nssrc=0xFFFFFFFF",
                    &cfg) == 0,
          "CRLF, comments, quoted literals, final line without newline");
    check(cfg.ssrc == UINT32_MAX && !strcmp(cfg.source, "a path/#literal;name.h265"),
          "parsed literals");
    check(load_text("[transport]\nport=5601\nport=5602\n", &cfg) == 0 && cfg.port == 5602,
          "duplicate key last value wins");
    char longline[2200];
    memset(longline, 'x', sizeof(longline));
    memcpy(longline, "[video]\nsource=", 15);
    longline[sizeof(longline) - 2] = '\n';
    longline[sizeof(longline) - 1] = 0;
    check(load_text(longline, &cfg) < 0, "oversized input line");
    char longvalue[650];
    memset(longvalue, 'x', sizeof(longvalue));
    memcpy(longvalue, "[video]\nsource=", 15);
    longvalue[sizeof(longvalue) - 2] = '\n';
    longvalue[sizeof(longvalue) - 1] = 0;
    check(load_text(longvalue, &cfg) < 0, "oversized string field");
    check(fpv_config_load(NULL, &cfg, NULL, 0) < 0, "null path and error buffer");
    check(fpv_config_validate(NULL, NULL, 0) < 0, "null configuration");
    char short_error[2] = {'x', 'x'};
    check(fpv_config_validate(NULL, short_error, sizeof(short_error)) < 0 && short_error[1] == 0,
          "small error buffer terminated");
    const char *names[] = {"air.ini", "ground.ini", "demo-air.ini", "demo-ground.ini"};
    for (size_t i = 0; i < 4; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/configs/%s", argc > 1 ? argv[1] : ".", names[i]);
        check(fpv_config_load(path, &cfg, error, sizeof(error)) == 0, names[i]);
        if (i < 2)
            check(cfg.wireless_enabled == 1, names[i]);
        else
            check(cfg.wireless_enabled == 0 && cfg.osd_enabled == 0, names[i]);
    }
    /* Regressions: signs hidden behind quoted whitespace and WFB payload size. */
    check(load_text("[transport]\nssrc=' -0'\n", &cfg) < 0, "quoted negative U32 rejected");
    check(load_text("[wireless]\nenabled=1\n[transport]\nmtu=4096\n", &cfg) < 0,
          "wireless MTU cannot exceed WFB payload");
    printf("config extra: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
