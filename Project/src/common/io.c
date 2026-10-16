#define _POSIX_C_SOURCE 200809L
#include "fpv/io.h"
#include "fpv/rtp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

uint64_t fpv_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
void fpv_wait_until(uint64_t deadline)
{
    uint64_t now = fpv_now_ms();
    if (deadline <= now)
        return;
    struct timespec ts = {(time_t)((deadline - now) / 1000u),
                          (long)(((deadline - now) % 1000u) * 1000000u)};
    /* EINTR returns control to the caller so signals remain responsive. */
    nanosleep(&ts, NULL);
}
int fpv_socket_open(fpv_socket *s, const fpv_config *c, int receive, char *error, size_t error_size)
{
    s->fd = -1;
    int domain = !strcmp(c->transport, "unix") ? AF_UNIX : AF_INET;
    int fd = socket(domain, SOCK_DGRAM, 0);
    if (fd < 0)
        goto fail;
    if (receive &&
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &c->socket_rcvbuf, sizeof c->socket_rcvbuf))
        goto fail;
    struct timeval timeout = {0, 200000};
    if (!receive && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout))
        goto fail;
    if (domain == AF_INET) {
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)c->port);
        if (inet_pton(AF_INET, receive ? c->bind_host : c->host, &addr.sin_addr) != 1) {
            errno = EINVAL;
            goto fail;
        }
        if ((receive ? bind(fd, (struct sockaddr *)&addr, sizeof addr)
                     : connect(fd, (struct sockaddr *)&addr, sizeof addr)) < 0)
            goto fail;
    } else {
        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        size_t len = strlen(c->unix_name);
        if (!len || len > sizeof addr.sun_path - 2) {
            errno = EINVAL;
            goto fail;
        }
        memcpy(addr.sun_path + 1, c->unix_name, len);
        socklen_t size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + len);
        if ((receive ? bind(fd, (struct sockaddr *)&addr, size)
                     : connect(fd, (struct sockaddr *)&addr, size)) < 0)
            goto fail;
    }
    s->fd = fd;
    return 0;
fail:
    if (error && error_size)
        snprintf(error, error_size, "%s %s socket: %s", receive ? "bind" : "connect", c->transport,
                 strerror(errno));
    if (fd >= 0)
        close(fd);
    return -1;
}
int fpv_socket_send(fpv_socket *s, const uint8_t *data, size_t size)
{
    ssize_t n;
    do {
        n = send(s->fd, data, size, 0);
    } while (n < 0 && errno == EINTR);
    return n == (ssize_t)size ? 0 : -1;
}
ssize_t fpv_socket_receive(fpv_socket *s, uint8_t *data, size_t capacity, int timeout_ms)
{
    struct pollfd p = {s->fd, POLLIN, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (!r)
        return 0;
    if (!(p.revents & POLLIN)) {
        errno = EIO;
        return -1;
    }
    struct iovec io = {data, capacity};
    struct msghdr msg = {0};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    ssize_t n = recvmsg(s->fd, &msg, 0);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    if (msg.msg_flags & MSG_TRUNC)
        return -2;
    return n;
}
void fpv_socket_close(fpv_socket *s)
{
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
}

int fpv_annexb_open(fpv_annexb *r, const char *path)
{
    memset(r, 0, sizeof *r);
    r->file = fopen(path, "rb");
    if (!r->file)
        return -1;
    r->capacity = FPV_MAX_NAL;
    r->nal = malloc(r->capacity);
    if (!r->nal) {
        fclose(r->file);
        r->file = NULL;
        return -1;
    }
    return 0;
}
int fpv_annexb_next(fpv_annexb *r)
{
    size_t zeros = 0;
    r->size = 0;
    int c;
    if (r->started == 2)
        return 0;
    while ((c = fgetc(r->file)) != EOF) {
        if (c == 0) {
            if (++zeros > r->capacity)
                return -1;
            continue;
        }
        if (c == 1 && zeros >= 2) {
            zeros = 0;
            if (r->started && r->size)
                return r->size >= 2 ? 1 : -1;
            if (r->started)
                return -1; /* Empty consecutive NAL. */
            r->started = 1;
            continue;
        }
        if (!r->started)
            return -1;
        if (zeros + 1 > r->capacity - r->size)
            return -1;
        memset(r->nal + r->size, 0, zeros);
        r->size += zeros;
        zeros = 0;
        r->nal[r->size++] = (uint8_t)c;
    }
    if (ferror(r->file))
        return -1;
    if (r->size) {
        r->started = 2;
        return r->size >= 2 ? 1 : -1;
    }
    if (r->started || zeros)
        return -1;
    r->started = 2;
    return 0;
}
int fpv_annexb_rewind(fpv_annexb *r)
{
    if (fseek(r->file, 0, SEEK_SET))
        return -1;
    clearerr(r->file);
    r->started = 0;
    r->size = 0;
    return 0;
}
void fpv_annexb_close(fpv_annexb *r)
{
    if (r->file)
        fclose(r->file);
    free(r->nal);
    memset(r, 0, sizeof *r);
}
