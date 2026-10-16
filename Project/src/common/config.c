#include "fpv/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
static int fail(char *e, size_t n, const char *fmt, ...)
{
    if (e && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e, n, fmt, ap);
        va_end(ap);
    }
    return -1;
}
void fpv_config_defaults(fpv_config *c)
{
    memset(c, 0, sizeof *c);
    strcpy(c->role, "air");
    strcpy(c->backend, "file");
    strcpy(c->source, "samples/demo.h265");
    strcpy(c->output, "received.h265");
    c->width = 1920;
    c->height = 1080;
    c->fps = 30;
    c->bitrate_kbps = 6000;
    c->gop = 30;
    strcpy(c->sensor, "imx415");
    strcpy(c->output_mode, "720p60");
    c->display_buffers = 2;
    strcpy(c->transport, "udp");
    strcpy(c->host, "127.0.0.1");
    strcpy(c->bind_host, "127.0.0.1");
    strcpy(c->unix_name, "fpv_video");
    c->port = 5600;
    c->mtu = 1200;
    c->payload_type = 96;
    c->ssrc = 0x46505631u;
    c->random_ssrc = 1;
    c->socket_rcvbuf = 1048576;
    c->fragment_timeout_ms = 250;
    c->channel = 161;
    c->bandwidth = 20;
    c->mcs = 2;
    c->fec_k = 8;
    c->fec_n = 12;
    c->link_id = 7669206;
    strcpy(c->iface, "wlan0");
    strcpy(c->driver, "8812eu");
    strcpy(c->key_path, "/etc/drone.key");
    c->txpower_mbm = -1;
    strcpy(c->serial_device, "/dev/ttyS2");
    c->serial_baud = 115200;
    c->osd_fps = 20;
    c->stats_interval_ms = 1000;
    c->restart_limit = 3;
    c->restart_delay_ms = 500;
    strcpy(c->log_dir, "/tmp/fpv/log");
    strcpy(c->run_dir, "/tmp/fpv");
    strcpy(c->bin_dir, "/usr/bin");
}
enum kind { STRING, INTEGER, U32 };
typedef struct {
    const char *key;
    enum kind type;
    size_t offset, size;
} field;
#define S(key, member)                                                                             \
    {                                                                                              \
        key, STRING, offsetof(fpv_config, member), sizeof(((fpv_config *)0)->member)               \
    }
#define I(key, member)                                                                             \
    {                                                                                              \
        key, INTEGER, offsetof(fpv_config, member), 0                                              \
    }
#define U(key, member)                                                                             \
    {                                                                                              \
        key, U32, offsetof(fpv_config, member), 0                                                  \
    }
static const field fields[] = {S("system.role", role),
                               S("video.backend", backend),
                               S("video.source", source),
                               S("video.output", output),
                               I("video.width", width),
                               I("video.height", height),
                               I("video.fps", fps),
                               I("video.bitrate_kbps", bitrate_kbps),
                               I("video.gop", gop),
                               I("video.sensor_mode", sensor_mode),
                               S("video.sensor", sensor),
                               S("video.output_mode", output_mode),
                               I("video.display_buffers", display_buffers),
                               I("video.max_frames", max_frames),
                               I("video.loop", loop),
                               S("transport.type", transport),
                               S("transport.host", host),
                               S("transport.bind_host", bind_host),
                               S("transport.unix_name", unix_name),
                               I("transport.port", port),
                               I("transport.mtu", mtu),
                               I("transport.payload_type", payload_type),
                               U("transport.ssrc", ssrc),
                               I("transport.random_ssrc", random_ssrc),
                               I("transport.socket_rcvbuf", socket_rcvbuf),
                               I("transport.fragment_timeout_ms", fragment_timeout_ms),
                               I("wireless.enabled", wireless_enabled),
                               S("wireless.iface", iface),
                               S("wireless.driver", driver),
                               I("wireless.channel", channel),
                               I("wireless.bandwidth", bandwidth),
                               I("wireless.mcs", mcs),
                               I("wireless.fec_k", fec_k),
                               I("wireless.fec_n", fec_n),
                               U("wireless.link_id", link_id),
                               I("wireless.radio_port", radio_port),
                               S("wireless.key_path", key_path),
                               I("wireless.txpower_mbm", txpower_mbm),
                               I("osd.enabled", osd_enabled),
                               S("osd.serial_device", serial_device),
                               I("osd.serial_baud", serial_baud),
                               I("osd.fps", osd_fps),
                               I("runtime.stats_interval_ms", stats_interval_ms),
                               I("runtime.idle_exit_ms", idle_exit_ms),
                               I("runtime.restart_limit", restart_limit),
                               I("runtime.restart_delay_ms", restart_delay_ms),
                               S("runtime.log_dir", log_dir),
                               S("runtime.run_dir", run_dir),
                               S("runtime.bin_dir", bin_dir)};
static int identifier(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-' && *s != '.')
            return 0;
    return 1;
}
int fpv_config_validate(const fpv_config *c, char *e, size_t n)
{
    if (!c)
        return fail(e, n, "null configuration");
    if (strcmp(c->role, "air") && strcmp(c->role, "ground"))
        return fail(e, n, "system.role must be air or ground");
    if (strcmp(c->backend, "file") && strcmp(c->backend, "sigmastar") &&
        strcmp(c->backend, "hisilicon"))
        return fail(e, n, "unknown video.backend");
    if ((!strcmp(c->backend, "sigmastar") && strcmp(c->role, "air")) ||
        (!strcmp(c->backend, "hisilicon") && strcmp(c->role, "ground")))
        return fail(e, n, "video.backend does not match role");
    if (strcmp(c->transport, "udp") && strcmp(c->transport, "unix"))
        return fail(e, n, "transport.type must be udp or unix");
#define RANGE(member, lo, hi)                                                                      \
    if (c->member < (lo) || c->member > (hi))                                                      \
    return fail(e, n, #member " outside [%d,%d]", (int)(lo), (int)(hi))
    RANGE(width, 64, 4096);
    RANGE(height, 64, 2160);
    if ((c->width | c->height) & 1)
        return fail(e, n, "width and height must be even");
    RANGE(fps, 1, 120);
    RANGE(bitrate_kbps, 128, 100000);
    RANGE(gop, 1, 600);
    RANGE(sensor_mode, 0, 255);
    RANGE(display_buffers, 0, 8);
    if (!strcmp(c->backend, "hisilicon") && !c->display_buffers)
        return fail(e, n, "hisilicon display_buffers must be at least 1");
    RANGE(max_frames, 0, INT_MAX);
    RANGE(loop, 0, 1);
    RANGE(port, 1, 65535);
    RANGE(mtu, 64, 4096);
    RANGE(payload_type, 96, 127);
    RANGE(random_ssrc, 0, 1);
    RANGE(socket_rcvbuf, 4096, 67108864);
    RANGE(fragment_timeout_ms, 1, 60000);
    RANGE(wireless_enabled, 0, 1);
    /* Pinned WFB payload limit: 4045-24-9-16-3 = 3993 bytes. */
    if (c->wireless_enabled && c->mtu > 3993)
        return fail(e, n, "wireless mtu exceeds WFB payload limit 3993");
    RANGE(channel, 1, 196);
    if (c->bandwidth != 20 && c->bandwidth != 40)
        return fail(e, n, "bandwidth must be 20 or 40");
    RANGE(mcs, 0, 7);
    RANGE(fec_k, 1, 255);
    RANGE(fec_n, 1, 255);
    if (c->fec_k > c->fec_n)
        return fail(e, n, "fec_k must be <= fec_n");
    RANGE(radio_port, 0, 255);
    if (c->link_id > 0xffffffu)
        return fail(e, n, "link_id must fit 24 bits");
    RANGE(txpower_mbm, -1, 5000);
    RANGE(osd_enabled, 0, 1);
    RANGE(serial_baud, 1200, 2000000);
    RANGE(osd_fps, 1, 50);
    if (c->osd_enabled) {
        if (c->width < 1280 || c->width > 3840 || c->height < 720)
            return fail(e, n, "msposd size must be 1280..3840 by 720..2160");
        static const int baudrates[] = {9600,   19200,  38400,  57600,  115200,
                                        230400, 460800, 500000, 921600, 1500000};
        int supported = 0;
        for (size_t i = 0; i < sizeof baudrates / sizeof *baudrates; i++)
            if (c->serial_baud == baudrates[i])
                supported = 1;
        if (!supported)
            return fail(e, n, "serial_baud is unsupported by pinned msposd");
    }
    RANGE(stats_interval_ms, 100, 60000);
    RANGE(idle_exit_ms, 0, INT_MAX);
    RANGE(restart_limit, 0, 100);
    RANGE(restart_delay_ms, 0, 60000);
#undef RANGE
    if (!identifier(c->iface) || !identifier(c->driver))
        return fail(e, n, "invalid wireless iface/driver identifier");
    if (!*c->unix_name || strlen(c->unix_name) > 106)
        return fail(e, n, "invalid abstract UNIX socket name");
    if (strcmp(c->output_mode, "720p60") && strcmp(c->output_mode, "1080p60"))
        return fail(e, n, "output_mode must be 720p60 or 1080p60");
    if (!*c->source || !*c->output || !*c->host || !*c->bind_host || !*c->key_path ||
        !*c->serial_device || !*c->run_dir || !*c->log_dir)
        return fail(e, n, "required path or host is empty");
    if (e && n)
        *e = 0;
    return 0;
}
static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    return s;
}
int fpv_config_load(const char *path, fpv_config *c, char *e, size_t n)
{
    if (!path || !c)
        return fail(e, n, "missing configuration path");
    FILE *f = fopen(path, "r");
    if (!f)
        return fail(e, n, "open %s: %s", path, strerror(errno));
    fpv_config_defaults(c);
    char line[2048], section[64] = "";
    unsigned lineno = 0;
    int result = -1;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(f)) {
            fail(e, n, "line %u too long", lineno);
            goto done;
        }
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end || *trim(end + 1) || end - s < 2 || (size_t)(end - s - 1) >= sizeof section) {
                fail(e, n, "line %u invalid section", lineno);
                goto done;
            }
            *end = 0;
            strcpy(section, s + 1);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || !*section) {
            fail(e, n, "line %u expected section/key=value", lineno);
            goto done;
        }
        *eq = 0;
        char *key = trim(s), *value = trim(eq + 1), full[128];
        if (snprintf(full, sizeof full, "%s.%s", section, key) >= (int)sizeof full) {
            fail(e, n, "line %u key too long", lineno);
            goto done;
        }
        size_t len = strlen(value);
        if (len && (*value == '\"' || *value == '\'')) {
            if (len < 2 || value[len - 1] != *value) {
                fail(e, n, "line %u unmatched quote", lineno);
                goto done;
            }
            value[len - 1] = 0;
            value++;
        }
        const field *fieldptr = NULL;
        for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
            if (!strcmp(full, fields[i].key)) {
                fieldptr = &fields[i];
                break;
            }
        if (!fieldptr) {
            fail(e, n, "line %u unknown key %s", lineno, full);
            goto done;
        }
        void *dest = (char *)c + fieldptr->offset;
        if (fieldptr->type == STRING) {
            if (strlen(value) >= fieldptr->size) {
                fail(e, n, "line %u %s too long", lineno, full);
                goto done;
            }
            strcpy(dest, value);
        } else {
            char *end;
            errno = 0;
            if (fieldptr->type == U32) {
                unsigned long long v = strtoull(value, &end, 0);
                if (errno || *value == '-' || *value == '+' || isspace((unsigned char)*value) ||
                    !*value || *end || v > UINT32_MAX) {
                    fail(e, n, "line %u invalid %s", lineno, full);
                    goto done;
                }
                *(uint32_t *)dest = (uint32_t)v;
            } else {
                long v = strtol(value, &end, 10);
                if (errno || !*value || *end || v < INT_MIN || v > INT_MAX) {
                    fail(e, n, "line %u invalid %s", lineno, full);
                    goto done;
                }
                *(int *)dest = (int)v;
            }
        }
    }
    if (ferror(f)) {
        fail(e, n, "read %s: %s", path, strerror(errno));
        goto done;
    }
    result = fpv_config_validate(c, e, n);
done:
    fclose(f);
    return result;
}
