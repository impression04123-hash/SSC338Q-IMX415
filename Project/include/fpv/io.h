#ifndef FPV_IO_H
#define FPV_IO_H
#include "fpv/config.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
typedef struct {
    int fd;
} fpv_socket;
int fpv_socket_open(fpv_socket *, const fpv_config *, int receive, char *, size_t);
int fpv_socket_send(fpv_socket *, const uint8_t *, size_t);
/* 0 timeout/EINTR; -1 error; -2 oversized datagram discarded. */
ssize_t fpv_socket_receive(fpv_socket *, uint8_t *, size_t, int timeout_ms);
void fpv_socket_close(fpv_socket *);
uint64_t fpv_now_ms(void);
void fpv_wait_until(uint64_t deadline_ms);
typedef struct {
    FILE *file;
    uint8_t *nal;
    size_t size, capacity;
    int started;
} fpv_annexb;
int fpv_annexb_open(fpv_annexb *, const char *path);
/* 1 NAL available, 0 EOF, -1 malformed/oversized/I/O failure. */
int fpv_annexb_next(fpv_annexb *);
int fpv_annexb_rewind(fpv_annexb *);
void fpv_annexb_close(fpv_annexb *);
#endif
