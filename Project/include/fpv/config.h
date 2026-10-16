#ifndef FPV_CONFIG_H
#define FPV_CONFIG_H
#include <stddef.h>
#include <stdint.h>

typedef struct fpv_config {
    char role[16], backend[24], source[512], output[512];
    int width, height, fps, bitrate_kbps, gop, sensor_mode;
    char sensor[32], output_mode[32];
    int display_buffers, max_frames, loop;
    char transport[16], host[64], bind_host[64], unix_name[108];
    int port, mtu, payload_type, socket_rcvbuf, fragment_timeout_ms;
    uint32_t ssrc;
    int random_ssrc;
    int wireless_enabled, channel, bandwidth, mcs, fec_k, fec_n, radio_port;
    uint32_t link_id;
    char iface[32], driver[32], key_path[512];
    int txpower_mbm;
    int osd_enabled, serial_baud, osd_fps;
    char serial_device[128];
    int stats_interval_ms, idle_exit_ms, restart_limit, restart_delay_ms;
    char log_dir[512], run_dir[512], bin_dir[512];
} fpv_config;

void fpv_config_defaults(fpv_config *cfg);
int fpv_config_load(const char *path, fpv_config *cfg, char *error, size_t error_size);
int fpv_config_validate(const fpv_config *cfg, char *error, size_t error_size);
#endif
