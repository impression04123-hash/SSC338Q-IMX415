#define _POSIX_C_SOURCE 200809L
#include "fpv/config.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Every command is exec'd directly. No config field ever reaches a shell. */
#define MAX_ARGS 48
#define MAX_COMMANDS 12
#define LOG_LIMIT (1024U * 1024U)
typedef struct command {
    char name[32], storage[MAX_ARGS][1024];
    char *argv[MAX_ARGS + 1];
    int argc, setup, reader, log_fd, restarts, ready_kind, ready_seen;
    unsigned ready_length;
    struct command *dependency;
    pid_t pid, group;
    size_t log_size;
    int64_t retry_at;
    char log_path[1024];
} command;
typedef struct plan {
    command cmds[MAX_COMMANDS];
    int count;
} plan;
static volatile sig_atomic_t stopping;
static int lock_fd = -1;

static void on_signal(int sig)
{
    stopping = sig;
}
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void pause_ms(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fpvctl: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
static void format(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        fail("command/path too long");
        exit(2);
    }
}
static void arg(command *c, const char *s)
{
    if (c->argc == MAX_ARGS) {
        fail("too many arguments");
        exit(2);
    }
    format(c->storage[c->argc], sizeof(c->storage[0]), "%s", s);
    c->argv[c->argc] = c->storage[c->argc];
    c->argv[++c->argc] = NULL;
}
static void number(command *c, const char *flag, unsigned long value)
{
    char s[32];
    format(s, sizeof(s), "%lu", value);
    arg(c, flag);
    arg(c, s);
}
static command *add(plan *p, const fpv_config *cfg, const char *name, int setup)
{
    command *c;
    char path[1024];
    if (p->count == MAX_COMMANDS) {
        fail("too many commands");
        exit(2);
    }
    c = &p->cmds[p->count++];
    memset(c, 0, sizeof(*c));
    c->reader = c->log_fd = -1;
    c->setup = setup;
    format(c->name, sizeof(c->name), "%s", name);
    if (cfg->bin_dir[0])
        format(path, sizeof(path), "%s/%s", cfg->bin_dir, name);
    else
        format(path, sizeof(path), "%s", name);
    arg(c, path);
    return c;
}
static command *app(plan *p, const fpv_config *cfg, const char *path)
{
    command *c = add(p, cfg, !strcmp(cfg->role, "air") ? "fpv-air" : "fpv-ground", 0);
    arg(c, "--config");
    arg(c, path);
    if (cfg->wireless_enabled && !strcmp(cfg->role, "ground"))
        c->ready_kind = !strcmp(cfg->transport, "unix") ? 2 : 1;
    if (cfg->osd_enabled && !strcmp(cfg->role, "air"))
        c->ready_kind = 3;
    return c;
}
static void wfb(plan *p, const fpv_config *cfg, int tx)
{
    command *c = add(p, cfg, tx ? "wfb_tx" : "wfb_rx", 0);
    arg(c, "-K");
    arg(c, cfg->key_path);
    number(c, "-p", (unsigned)cfg->radio_port);
    number(c, "-i", cfg->link_id);
    if (!strcmp(cfg->transport, "unix")) {
        arg(c, "-U");
        arg(c, cfg->unix_name);
    } else {
        number(c, "-u", (unsigned)cfg->port);
        if (!tx) {
            arg(c, "-c");
            arg(c, cfg->bind_host);
        }
    }
    if (tx) {
        number(c, "-k", (unsigned)cfg->fec_k);
        number(c, "-n", (unsigned)cfg->fec_n);
        number(c, "-B", (unsigned)cfg->bandwidth);
        number(c, "-M", (unsigned)cfg->mcs);
        c->ready_kind = !strcmp(cfg->transport, "unix") ? 2 : 1;
    }
    arg(c, cfg->iface);
}
static void build_plan(plan *p, const fpv_config *cfg, const char *path)
{
    command *c, *video;
    int air = !strcmp(cfg->role, "air");
    memset(p, 0, sizeof(*p));
    if (cfg->wireless_enabled) {
        c = add(p, cfg, "modprobe", 1);
        arg(c, cfg->driver);
        c = add(p, cfg, "ip", 1);
        arg(c, "link");
        arg(c, "set");
        arg(c, "dev");
        arg(c, cfg->iface);
        arg(c, "down");
        c = add(p, cfg, "iw", 1);
        arg(c, "dev");
        arg(c, cfg->iface);
        arg(c, "set");
        arg(c, "type");
        arg(c, "monitor");
        c = add(p, cfg, "iw", 1);
        arg(c, "dev");
        arg(c, cfg->iface);
        arg(c, "set");
        number(c, "channel", (unsigned)cfg->channel);
        arg(c, cfg->bandwidth == 40 ? "HT40+" : "HT20");
        if (cfg->txpower_mbm > 0) {
            char power[32];
            c = add(p, cfg, "iw", 1);
            arg(c, "dev");
            arg(c, cfg->iface);
            arg(c, "set");
            arg(c, "txpower");
            arg(c, "fixed");
            format(power, sizeof(power), "%d", cfg->txpower_mbm);
            arg(c, power);
        }
        c = add(p, cfg, "ip", 1);
        arg(c, "link");
        arg(c, "set");
        arg(c, "dev");
        arg(c, cfg->iface);
        arg(c, "up");
        if (air)
            wfb(p, cfg, 1);
    }
    video = app(p, cfg, path);
    if (cfg->wireless_enabled && !air)
        wfb(p, cfg, 0);
    if (cfg->osd_enabled && air) {
        char resolution[32];
        c = add(p, cfg, "msposd", 0);
        c->dependency = video;
        arg(c, "--master");
        arg(c, cfg->serial_device);
        number(c, "--baudrate", (unsigned)cfg->serial_baud);
        format(resolution, sizeof(resolution), "%dx%d", cfg->width, cfg->height);
        arg(c, "--size");
        arg(c, resolution);
        arg(c, "--osd");
        number(c, "-r", (unsigned)cfg->osd_fps);
    }
}
static void json_string(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    putchar('"');
    for (; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar(*p);
        } else if (*p < 32)
            printf("\\u%04x", *p);
        else
            putchar(*p);
    }
    putchar('"');
}
static void print_plan(const plan *p)
{
    int i, j;
    for (i = 0; i < p->count; ++i) {
        putchar('[');
        for (j = 0; j < p->cmds[i].argc; ++j) {
            if (j)
                putchar(',');
            json_string(p->cmds[i].argv[j]);
        }
        puts("]");
    }
}
static int regular_readable(const char *path, int key)
{
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || access(path, R_OK) ||
        (key && st.st_size != 64)) {
        fail("%s must be a readable %s", path, key ? "64-byte WFB key" : "regular file");
        return -1;
    }
    return 0;
}
static int executable(const char *name)
{
    char paths[4096], full[1024], *item, *save;
    const char *env = getenv("PATH");
    if (strchr(name, '/'))
        return access(name, X_OK) == 0;
    format(paths, sizeof(paths), "%s", env ? env : "/usr/sbin:/usr/bin:/sbin:/bin");
    item = strtok_r(paths, ":", &save);
    while (item) {
        format(full, sizeof(full), "%s/%s", item, name);
        if (!access(full, X_OK))
            return 1;
        item = strtok_r(NULL, ":", &save);
    }
    return 0;
}
static int preflight(const plan *p, const fpv_config *cfg)
{
    int i;
    struct stat st;
    for (i = 0; i < p->count; ++i)
        if (!executable(p->cmds[i].argv[0])) {
            fail("missing executable: %s", p->cmds[i].argv[0]);
            return -1;
        }
    if (!strcmp(cfg->role, "air") && !strcmp(cfg->backend, "file") &&
        regular_readable(cfg->source, 0))
        return -1;
    if (cfg->wireless_enabled) {
        if (geteuid() != 0) {
            fail("wireless initialization requires root");
            return -1;
        }
        if (regular_readable(cfg->key_path, 1))
            return -1;
        if (!strcmp(cfg->transport, "udp") &&
            (strcmp(cfg->host, "127.0.0.1") || strcmp(cfg->bind_host, "127.0.0.1"))) {
            fail("wireless UDP handoff requires host and bind_host=127.0.0.1");
            return -1;
        }
    }
    if (cfg->osd_enabled && (!strcmp(cfg->role, "ground") || stat(cfg->serial_device, &st) ||
                             !S_ISCHR(st.st_mode) || access(cfg->serial_device, R_OK | W_OK))) {
        fail("OSD requires air role and a readable/writable serial device");
        return -1;
    }
    return 0;
}
static int mkdirs(const char *path)
{
    char tmp[1024], *s;
    struct stat st;
    format(tmp, sizeof(tmp), "%s", path);
    for (s = tmp + 1;; ++s) {
        if (!*s || *s == '/') {
            char ch = *s;
            *s = 0;
            if ((mkdir(tmp, 0750) && errno != EEXIST) || stat(tmp, &st) || !S_ISDIR(st.st_mode)) {
                fail("directory %s: %s", tmp, strerror(errno));
                return -1;
            }
            *s = ch;
            if (!ch)
                break;
        }
    }
    return 0;
}
static int lock_role(const fpv_config *cfg)
{
    char path[1024], pid[32];
    int n;
    format(path, sizeof(path), "%s/fpv-%s.lock", cfg->run_dir, cfg->role);
    lock_fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB)) {
        fail("role %s is already running or lock unavailable", cfg->role);
        return -1;
    }
    n = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
    if (ftruncate(lock_fd, 0) || write(lock_fd, pid, (size_t)n) != n)
        return -1;
    return 0;
}
static int open_log(command *c, const fpv_config *cfg)
{
    struct stat st;
    format(c->log_path, sizeof(c->log_path), "%s/%s-%s.log", cfg->log_dir, cfg->role, c->name);
    c->log_fd = open(c->log_path,
                     O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0640);
    if (c->log_fd < 0 || fstat(c->log_fd, &st) || !S_ISREG(st.st_mode)) {
        fail("cannot open regular log %s", c->log_path);
        return -1;
    }
    c->log_size = (size_t)st.st_size;
    return 0;
}
static int write_log(command *c, const char *data, size_t size)
{
    if (c->log_size + size > LOG_LIMIT) {
        char old[1100];
        format(old, sizeof(old), "%s.1", c->log_path);
        if (rename(c->log_path, old))
            return -1;
        close(c->log_fd);
        c->log_fd = open(c->log_path,
                         O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0640);
        if (c->log_fd < 0)
            return -1;
        c->log_size = 0;
    }
    while (size) {
        ssize_t n = write(c->log_fd, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        data += n;
        size -= (size_t)n;
        c->log_size += (size_t)n;
    }
    return 0;
}
static int drain(command *c)
{
    static const char ready_line[] = "FPV_READY";
    char data[4096];
    int budget = 16;
    if (c->reader < 0)
        return 0;
    while (budget--) {
        ssize_t n = read(c->reader, data, sizeof(data));
        if (n > 0) {
            if (write_log(c, data, (size_t)n)) {
                fail("write log %s: %s", c->log_path, strerror(errno));
                return -1;
            }
            if (c->ready_kind == 3 && !c->ready_seen) {
                ssize_t i;
                for (i = 0; i < n; ++i) {
                    if (data[i] == '\n') {
                        if (c->ready_length == sizeof(ready_line) - 1)
                            c->ready_seen = 1;
                        c->ready_length = 0;
                    } else if (c->ready_length < sizeof(ready_line) - 1 &&
                               data[i] == ready_line[c->ready_length])
                        ++c->ready_length;
                    else
                        /* Saturate until newline; even huge log lines stay bounded. */
                        c->ready_length = sizeof(ready_line);
                }
            }
        } else if (!n) {
            close(c->reader);
            c->reader = -1;
            break;
        } else if (errno == EINTR)
            continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        else
            return -1;
    }
    return 0;
}
static int spawn(command *c)
{
    int pipes[2];
    pid_t pid;
    if (pipe(pipes))
        return -1;
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid < 0) {
        close(pipes[0]);
        close(pipes[1]);
        return -1;
    }
    if (!pid) {
        int nullfd;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGPIPE, &sa, NULL);
        if (setpgid(0, 0))
            _exit(126);
        nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0 || dup2(pipes[1], STDOUT_FILENO) < 0 ||
            dup2(pipes[1], STDERR_FILENO) < 0)
            _exit(126);
        if (nullfd > STDERR_FILENO)
            close(nullfd);
        close(pipes[0]);
        close(pipes[1]);
        execvp(c->argv[0], c->argv);
        dprintf(STDERR_FILENO, "exec %s: %s\n", c->argv[0], strerror(errno));
        _exit(127);
    }
    close(pipes[1]);
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    (void)setpgid(pid, pid);
    c->pid = c->group = pid;
    c->reader = pipes[0];
    c->ready_seen = 0;
    c->ready_length = 0;
    fail("started %s pid=%ld attempt=%d", c->name, (long)pid, c->restarts + 1);
    return 0;
}
static void signal_group(command *c, int sig)
{
    if (c->group > 0)
        (void)kill(-c->group, sig);
}
/* Reap the leader last: its PID cannot be reused while its group is cleaned. */
static void terminate_group(command *c)
{
    int i;
    if (c->group <= 0)
        return;
    signal_group(c, SIGTERM);
    for (i = 0; i < 20; ++i) {
        (void)drain(c);
        pause_ms(10);
    }
    signal_group(c, SIGKILL);
    if (c->pid > 0)
        while (waitpid(c->pid, NULL, 0) < 0 && errno == EINTR) {
        }
    c->pid = c->group = 0;
    (void)drain(c);
    if (c->reader >= 0) {
        close(c->reader);
        c->reader = -1;
    }
    c->ready_seen = 0;
    c->ready_length = 0;
}
/* waitid WNOWAIT preserves PID ownership until descendants have been signalled. */
static int exited(command *c, int *code)
{
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    if (waitid(P_PID, (id_t)c->pid, &info, WEXITED | WNOHANG | WNOWAIT))
        return -1;
    if (!info.si_pid)
        return 0;
    *code = info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status;
    return 1;
}
static int endpoint_ready(const fpv_config *cfg, int kind)
{
    int fd, ok;
    if (kind == 2) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path + 1, cfg->unix_name, strlen(cfg->unix_name));
        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0)
            return 0;
        ok = connect(fd, (struct sockaddr *)&addr,
                     (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 +
                                 strlen(cfg->unix_name))) == 0;
    } else {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)cfg->port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            return 0;
        ok = bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno == EADDRINUSE;
    }
    close(fd);
    return ok;
}
static int wait_ready(command *c, const fpv_config *cfg)
{
    int code = 0;
    int64_t deadline = now_ms() + 3000;
    if (!c->ready_kind)
        return 0;
    while (!stopping && now_ms() < deadline) {
        int state;
        if (drain(c))
            break;
        state = exited(c, &code);
        if (state == 1 && !code && !strcmp(cfg->backend, "file") &&
            (!strcmp(c->name, "fpv-air") || !strcmp(c->name, "fpv-ground")))
            return 1;
        if (state != 0)
            break;
        if (c->ready_kind == 3 ? c->ready_seen : endpoint_ready(cfg, c->ready_kind))
            return 0;
        pause_ms(10);
    }
    fail("%s %s not ready", c->name,
         c->ready_kind == 3 ? "video initialization" : "handoff endpoint");
    return -1;
}
/* 0: running and ready, 1: file application finished, -1: failure. */
static int start_service(plan *p, command *c, const fpv_config *cfg)
{
    for (;;) {
        int ready;
        int64_t deadline;
        if (spawn(c))
            return -1;
        ready = wait_ready(c, cfg);
        if (!ready)
            return 0;
        terminate_group(c);
        if (ready > 0)
            return 1;
        if (stopping)
            return -1;
        if (c->restarts >= cfg->restart_limit) {
            fail("%s restart budget exhausted before readiness", c->name);
            return -1;
        }
        ++c->restarts;
        deadline =
            now_ms() + (int64_t)cfg->restart_delay_ms * (c->restarts < 10 ? c->restarts : 10);
        while (!stopping && now_ms() < deadline) {
            int i;
            for (i = 0; i < p->count; ++i)
                if (drain(&p->cmds[i]))
                    return -1;
            pause_ms(10);
        }
        if (stopping)
            return -1;
    }
}
static int setup_command(command *c)
{
    int code = 0, state = 0;
    int64_t deadline = now_ms() + 10000;
    if (spawn(c))
        return -1;
    while (!stopping && now_ms() < deadline) {
        if (drain(c))
            break;
        state = exited(c, &code);
        if (state != 0)
            break;
        pause_ms(10);
    }
    terminate_group(c);
    if (state != 1 || code || stopping) {
        fail("setup %s failed (exit %d or timeout)", c->name, code);
        return -1;
    }
    return 0;
}
static int supervise(plan *p, const fpv_config *cfg)
{
    int i, result = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (mkdirs(cfg->run_dir) || lock_role(cfg) || mkdirs(cfg->log_dir))
        goto done;
    /* Reject an occupied handoff before creating children; readiness belongs to us. */
    if (cfg->wireless_enabled && endpoint_ready(cfg, !strcmp(cfg->transport, "unix") ? 2 : 1)) {
        fail("local handoff endpoint already in use");
        goto done;
    }
    for (i = 0; i < p->count && !stopping; ++i) {
        command *c = &p->cmds[i];
        if (open_log(c, cfg))
            goto done;
        if (c->setup) {
            if (setup_command(c))
                goto done;
        } else {
            int state = start_service(p, c, cfg);
            if (state) {
                if (state > 0)
                    result = 0;
                goto done;
            }
        }
    }
    while (!stopping) {
        for (i = 0; i < p->count; ++i) {
            command *c = &p->cmds[i];
            int code = 0, state;
            if (c->setup)
                continue;
            if (drain(c))
                goto done;
            if (!c->pid) {
                if (c->dependency && (!c->dependency->pid || !c->dependency->ready_seen))
                    continue;
                if (now_ms() >= c->retry_at) {
                    state = start_service(p, c, cfg);
                    if (state) {
                        if (state > 0)
                            result = 0;
                        goto done;
                    }
                }
                continue;
            }
            state = exited(c, &code);
            if (state < 0)
                goto done;
            if (!state)
                continue;
            fail("%s exited status=%d", c->name, code);
            {
                int j;
                for (j = p->count - 1; j >= 0; --j) {
                    command *dependent = &p->cmds[j];
                    if (dependent->dependency == c) {
                        /* Old OSD region cleanup must finish before new VPE opens. */
                        terminate_group(dependent);
                        dependent->retry_at = 0;
                    }
                }
            }
            terminate_group(c);
            if (!code && !strcmp(cfg->backend, "file") &&
                (!strcmp(c->name, "fpv-air") || !strcmp(c->name, "fpv-ground"))) {
                result = 0;
                goto done;
            }
            if (c->restarts >= cfg->restart_limit) {
                fail("%s restart budget exhausted", c->name);
                goto done;
            }
            ++c->restarts;
            /* Linear backoff, capped to keep signal responsiveness predictable. */
            c->retry_at =
                now_ms() + (int64_t)cfg->restart_delay_ms * (c->restarts < 10 ? c->restarts : 10);
        }
        pause_ms(20);
    }
    result = 0;
done:
    for (i = p->count - 1; i >= 0; --i)
        terminate_group(&p->cmds[i]);
    for (i = 0; i < p->count; ++i)
        if (p->cmds[i].log_fd >= 0)
            close(p->cmds[i].log_fd);
    /* Keep the inode: unlinking a live flock creates a split-lock race. */
    if (lock_fd >= 0) {
        close(lock_fd);
        lock_fd = -1;
    }
    return stopping ? 0 : result;
}
int main(int argc, char **argv)
{
    static plan p;
    fpv_config cfg;
    char error[512] = {0};
    const char *mode, *path;
    if (argc != 4 || strcmp(argv[2], "--config")) {
        fprintf(stderr, "Usage: fpvctl {check|dry-run|run} --config PATH\n");
        return 2;
    }
    mode = argv[1];
    path = argv[3];
    if (!strcmp(mode, "--dry-run"))
        mode = "dry-run";
    if (strcmp(mode, "check") && strcmp(mode, "dry-run") && strcmp(mode, "run")) {
        fail("unknown command %s", mode);
        return 2;
    }
    if (fpv_config_load(path, &cfg, error, sizeof(error)) ||
        fpv_config_validate(&cfg, error, sizeof(error))) {
        fail("%s", error);
        return 2;
    }
    if (cfg.wireless_enabled && cfg.bandwidth != 20 && cfg.bandwidth != 40) {
        fail("supervisor wireless channel setup supports bandwidth 20 or 40");
        return 2;
    }
    if (cfg.osd_enabled && strcmp(cfg.role, "air")) {
        fail("OSD is only supported on air");
        return 2;
    }
    build_plan(&p, &cfg, path);
    if (!strcmp(mode, "check")) {
        printf("configuration valid: role=%s backend=%s transport=%s\n", cfg.role, cfg.backend,
               cfg.transport);
        return 0;
    }
    if (!strcmp(mode, "dry-run")) {
        print_plan(&p);
        return 0;
    }
    if (preflight(&p, &cfg))
        return 1;
    return supervise(&p, &cfg);
}
