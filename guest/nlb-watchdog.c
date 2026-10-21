/*
 * nix-linux-builder — Guest build watchdog
 * Copyright 2025 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group
 * Apache License 2.0
 *
 * Runs the builder on a pseudo-terminal and relays its output to our stdout
 * (the hvc0 console that nix reads as the build log).  Seeing every byte
 * lets the watchdog tell when a build has gone silent — the same signal
 * nix's max-silent-time acts on — and diagnose it before nix kills the VM
 * and every clue with it.
 *
 * While the build is silent, at these thresholds (seconds of silence, 0
 * disables, set on the kernel command line or with the flags below):
 *
 *   nlb.hang_snapshot=N  (-s)  write a snapshot of every build process and
 *                              thread (state, wchan, syscall, kernel stack,
 *                              CPU and I/O since the last snapshot), PSI,
 *                              CPU steal and the kernel log
 *                              (with the hung-task detector's reports)
 *   nlb.hang_interval=N  (-i)  ...and again every N seconds after that
 *   nlb.hang_stopcont=N  (-c)  SIGSTOP + SIGCONT every build process once,
 *                              and note whether output or CPU comes back
 *                              (it does for a lost futex wakeup)
 *   nlb.hang_fail=N      (-f)  print a summary and verdict to the build log,
 *                              kill the build and exit 124.  Keep this below
 *                              nix's max-silent-time: printing resets nix's
 *                              silence timer, so the verdict has to come
 *                              with the failure, not before it.
 *
 * Nothing is printed to the build log before the fail threshold, for the
 * same reason.  Snapshots go to a directory (-d, nlb.diag_dir) — on the
 * farm a VirtioFS share that outlives the VM.  They are first written to
 * the tmpfs root and then copied by a child process, so a VirtioFS stall
 * (one of the things being diagnosed) cannot wedge the watchdog itself.
 *
 * Usage: nlb-watchdog [-s N] [-i N] [-c N] [-f N] [-d DIR] [-H TEXT] -- CMD...
 * Exit code: CMD's exit code, 128+signal if it was killed, 124 on hang.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define HANG_EXIT_CODE 124

/* Snapshots are written here (tmpfs) before publish() copies them out.
 * /run/nlb/staging in the VM; under $TMPDIR when testing as a normal user. */
static char staging[256] = "/run/nlb/staging";

/* ── Configuration ─────────────────────────────────────────────────────── */

typedef struct {
    long snapshot_after;   /* first snapshot after this much silence */
    long interval;         /* then every interval seconds */
    long stopcont_after;   /* SIGSTOP/SIGCONT probe once */
    long fail_after;       /* summary + kill */
    const char *diag_dir;  /* where snapshots end up */
    const char *diag_host; /* the host's name for diag_dir, for the summary */
} config;

static long parse_secs(const char *s, const char *what)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0) {
        fprintf(stderr, "nlb-watchdog: invalid %s: %s\n", what, s);
        exit(2);
    }
    return v;
}

/* Read nlb.* settings from the kernel command line.  Flags given on our own
 * command line are applied afterwards and win. */
static void config_from_cmdline(config *cfg)
{
    static char buf[4096];
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f)
        return;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    for (char *tok = strtok(buf, " \n"); tok; tok = strtok(NULL, " \n")) {
        char *eq = strchr(tok, '=');
        if (!eq || strncmp(tok, "nlb.", 4) != 0)
            continue;
        *eq = '\0';
        const char *k = tok + 4, *v = eq + 1;
        if (strcmp(k, "hang_snapshot") == 0)
            cfg->snapshot_after = parse_secs(v, tok);
        else if (strcmp(k, "hang_interval") == 0)
            cfg->interval = parse_secs(v, tok);
        else if (strcmp(k, "hang_stopcont") == 0)
            cfg->stopcont_after = parse_secs(v, tok);
        else if (strcmp(k, "hang_fail") == 0)
            cfg->fail_after = parse_secs(v, tok);
        else if (strcmp(k, "diag_dir") == 0)
            cfg->diag_dir = strdup(v);
        else if (strcmp(k, "diag_host") == 0)
            cfg->diag_host = strdup(v);
    }
}

/* ── Small helpers ─────────────────────────────────────────────────────── */

static double now_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Read a whole (small) file into buf, NUL-terminated.  Returns length or -1. */
static ssize_t read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < size - 1) {
        ssize_t r = read(fd, buf + off, size - 1 - off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            break;
        off += (size_t)r;
    }
    close(fd);
    buf[off] = '\0';
    return (ssize_t)off;
}

/* Strip one trailing newline. */
static char *chomp(char *s)
{
    size_t n = strlen(s);
    if (n && s[n - 1] == '\n')
        s[n - 1] = '\0';
    return s;
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        buf += w;
        len -= (size_t)w;
    }
}

/* ── Process and thread sampling ───────────────────────────────────────── */

typedef struct {
    int pid, tid, ppid, uid;
    char state;
    char comm[64];
    unsigned long long ticks;        /* utime + stime */
    unsigned long long vcsw, ivcsw;  /* context switches */
    int cpu;                         /* last CPU it ran on */
    char wchan[64];
    char syscall[128];
} thread_sample;

typedef struct {
    thread_sample *t;
    size_t n, cap;
    double when;
} sample_set;

static void sample_free(sample_set *s)
{
    free(s->t);
    memset(s, 0, sizeof(*s));
}

static const thread_sample *sample_find(const sample_set *s, int tid)
{
    for (size_t i = 0; i < s->n; i++)
        if (s->t[i].tid == tid)
            return &s->t[i];
    return NULL;
}

/* Parse /proc/<pid>/task/<tid>/stat.  comm can hold spaces and parens, so
 * fields are counted from the LAST ')'. */
static bool parse_stat(const char *path, thread_sample *t)
{
    char buf[1024];
    if (read_file(path, buf, sizeof(buf)) <= 0)
        return false;
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
    if (!lp || !rp || rp < lp)
        return false;
    size_t cl = (size_t)(rp - lp - 1);
    if (cl >= sizeof(t->comm))
        cl = sizeof(t->comm) - 1;
    memcpy(t->comm, lp + 1, cl);
    t->comm[cl] = '\0';

    /* Fields after ") ": 3=state 4=ppid ... 14=utime 15=stime ... 39=processor */
    char *save = NULL;
    int field = 3;
    unsigned long long utime = 0, stime = 0;
    for (char *tok = strtok_r(rp + 2, " ", &save); tok;
         tok = strtok_r(NULL, " ", &save), field++) {
        switch (field) {
        case 3:  t->state = tok[0]; break;
        case 4:  t->ppid = atoi(tok); break;
        case 14: utime = strtoull(tok, NULL, 10); break;
        case 15: stime = strtoull(tok, NULL, 10); break;
        case 39: t->cpu = atoi(tok); break;
        default: break;
        }
    }
    t->ticks = utime + stime;
    return true;
}

static void parse_status(const char *path, thread_sample *t)
{
    char buf[4096];
    if (read_file(path, buf, sizeof(buf)) <= 0)
        return;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (strncmp(line, "Uid:", 4) == 0)
            t->uid = atoi(line + 4);
        else if (strncmp(line, "voluntary_ctxt_switches:", 24) == 0)
            t->vcsw = strtoull(line + 24, NULL, 10);
        else if (strncmp(line, "nonvoluntary_ctxt_switches:", 27) == 0)
            t->ivcsw = strtoull(line + 27, NULL, 10);
    }
}

static bool is_numeric(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return false;
    return true;
}

/* The builder process, for keep_build(). */
static pid_t build_root = -1;

/* Narrow a sample to the build: the builder's process tree, plus (when we
 * run as root, as in the VM) every non-root process, which catches anything
 * the build daemonised out of the tree.  Kernel threads, init and this
 * watchdog are root and outside the tree.  As a normal user (tests outside
 * the VM) everything else of ours is non-root too, so only the tree counts. */
static void keep_build(sample_set *s)
{
    bool *keep = calloc(s->n ? s->n : 1, sizeof(*keep));
    if (!keep)
        return;
    bool root = getuid() == 0;
    for (size_t i = 0; i < s->n; i++) {
        int pid = s->t[i].pid;
        for (int hops = 0; pid > 1 && pid != build_root && hops < 64; hops++) {
            int ppid = 0;
            for (size_t j = 0; j < s->n; j++)
                if (s->t[j].tid == pid) {
                    ppid = s->t[j].ppid;
                    break;
                }
            pid = ppid;
        }
        keep[i] = pid == build_root || (root && s->t[i].uid != 0);
    }
    size_t kept = 0;
    for (size_t i = 0; i < s->n; i++)
        if (keep[i])
            s->t[kept++] = s->t[i];
    s->n = kept;
    free(keep);
}

/* Sample every thread of the build (see keep_build). */
static void sample_take(sample_set *s)
{
    s->n = 0;
    s->when = now_mono();
    DIR *proc = opendir("/proc");
    if (!proc)
        return;
    struct dirent *pe;
    while ((pe = readdir(proc))) {
        if (!is_numeric(pe->d_name))
            continue;
        char path[512];
        snprintf(path, sizeof(path), "/proc/%s/task", pe->d_name);
        DIR *task = opendir(path);
        if (!task)
            continue;
        struct dirent *te;
        while ((te = readdir(task))) {
            if (!is_numeric(te->d_name))
                continue;
            thread_sample t;
            memset(&t, 0, sizeof(t));
            t.pid = atoi(pe->d_name);
            t.tid = atoi(te->d_name);
            char base[64];
            snprintf(base, sizeof(base), "/proc/%d/task/%d", t.pid, t.tid);
            snprintf(path, sizeof(path), "%s/stat", base);
            if (!parse_stat(path, &t))
                continue;
            snprintf(path, sizeof(path), "%s/status", base);
            parse_status(path, &t);
            if (t.pid == getpid() || t.pid == 1 || t.pid == 2 || t.ppid == 2)
                continue;
            snprintf(path, sizeof(path), "%s/wchan", base);
            if (read_file(path, t.wchan, sizeof(t.wchan)) < 0)
                strcpy(t.wchan, "?");
            chomp(t.wchan);
            snprintf(path, sizeof(path), "%s/syscall", base);
            if (read_file(path, t.syscall, sizeof(t.syscall)) < 0)
                strcpy(t.syscall, "?");
            chomp(t.syscall);

            if (s->n == s->cap) {
                size_t cap = s->cap ? s->cap * 2 : 256;
                thread_sample *nt = realloc(s->t, cap * sizeof(*nt));
                if (!nt)
                    break;
                s->t = nt;
                s->cap = cap;
            }
            s->t[s->n++] = t;
        }
        closedir(task);
    }
    closedir(proc);
    keep_build(s);
}

/* Name of the syscall a thread sits in, from the first field of
 * /proc/<pid>/task/<tid>/syscall.  Rosetta translates x86_64 syscalls into
 * native ones, so in the VM these are always arm64 numbers. */
static const char *syscall_name(const char *line)
{
    static char buf[32];
    if (strncmp(line, "running", 7) == 0 || line[0] == '?')
        return line;
    long nr = strtol(line, NULL, 10);
#if defined(__aarch64__)
    static const struct { long nr; const char *name; } names[] = {
        { 22, "epoll_pwait" }, { 29, "ioctl" }, { 56, "openat" }, { 57, "close" },
        { 63, "read" }, { 64, "write" }, { 72, "pselect6" }, { 73, "ppoll" },
        { 79, "newfstatat" }, { 80, "fstat" }, { 98, "futex" }, { 101, "nanosleep" },
        { 115, "clock_nanosleep" }, { 124, "sched_yield" }, { 215, "munmap" },
        { 222, "mmap" }, { 226, "mprotect" }, { 260, "wait4" },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].nr == nr) {
            snprintf(buf, sizeof(buf), "%s(%ld)", names[i].name, nr);
            return buf;
        }
#endif
    snprintf(buf, sizeof(buf), "syscall %ld", nr);
    return buf;
}

/* ── Analysis ──────────────────────────────────────────────────────────── */

typedef struct {
    size_t threads, running, sleeping, disk, stopped, zombie;
    size_t futex, fuse;               /* threads waiting in those (by wchan) */
    double cpu_pct;                   /* % of one CPU, all build threads */
    double busiest_pct;
    thread_sample busiest;
    double secs;                      /* length of the measured interval */
    char d_wchans[256];               /* distinct wchans of D threads */
} analysis;

static bool wchan_is_fuse(const char *w)
{
    return strstr(w, "fuse") || strstr(w, "request_wait_answer")
        || strstr(w, "virtio");
}

static void analyse(const sample_set *prev, const sample_set *cur, analysis *a)
{
    memset(a, 0, sizeof(*a));
    long hz = sysconf(_SC_CLK_TCK);
    a->secs = prev && prev->when > 0 ? cur->when - prev->when : 0;
    unsigned long long total_dt = 0;
    for (size_t i = 0; i < cur->n; i++) {
        const thread_sample *t = &cur->t[i];
        a->threads++;
        switch (t->state) {
        case 'R': a->running++; break;
        case 'S': a->sleeping++; break;
        case 'D': a->disk++; break;
        case 'T': case 't': a->stopped++; break;
        case 'Z': a->zombie++; break;
        default: break;
        }
        if (strstr(t->wchan, "futex"))
            a->futex++;
        if (wchan_is_fuse(t->wchan))
            a->fuse++;
        if (t->state == 'D' && !strstr(a->d_wchans, t->wchan)) {
            size_t l = strlen(a->d_wchans);
            snprintf(a->d_wchans + l, sizeof(a->d_wchans) - l, "%s%s",
                     l ? " " : "", t->wchan);
        }
        const thread_sample *p = prev ? sample_find(prev, t->tid) : NULL;
        if (p && a->secs > 0 && t->ticks >= p->ticks) {
            unsigned long long dt = t->ticks - p->ticks;
            total_dt += dt;
            double pct = 100.0 * (double)dt / (double)hz / a->secs;
            if (pct > a->busiest_pct) {
                a->busiest_pct = pct;
                a->busiest = *t;
            }
        }
    }
    if (a->secs > 0)
        a->cpu_pct = 100.0 * (double)total_dt / (double)hz / a->secs;
}

/* One line saying what the evidence points at.  Deliberately blunt: the
 * snapshots hold the detail, this is what goes in the Hydra log. */
static void verdict(const analysis *a, const char *stopcont, char *out, size_t size)
{
    if (a->threads == 0) {
        snprintf(out, size, "NO-BUILD-PROCESSES: nothing of the build is "
                 "running, yet its output is still open");
    } else if (a->disk > 0 && a->fuse > 0) {
        snprintf(out, size, "VIRTIOFS: %zu thread(s) in uninterruptible sleep, "
                 "%zu of them waiting on the VirtioFS/FUSE server (wchan: %s) "
                 "— the host side of a share stopped answering", a->disk,
                 a->fuse, a->d_wchans);
    } else if (a->disk > 0) {
        snprintf(out, size, "KERNEL-BLOCKED: %zu thread(s) in uninterruptible "
                 "sleep (wchan: %s)", a->disk, a->d_wchans);
    } else if (a->secs > 0 && a->cpu_pct >= 50.0) {
        snprintf(out, size, "BUSY: the build used %.0f%% CPU over the last %.0fs "
                 "without printing anything — spinning, livelocked or looping; "
                 "busiest: %s (pid %d tid %d) %.0f%%", a->cpu_pct, a->secs,
                 a->busiest.comm, a->busiest.pid, a->busiest.tid, a->busiest_pct);
    } else if (a->secs > 0) {
        snprintf(out, size, "IDLE: %zu build thread(s), %zu asleep on a futex, "
                 "%.1f%% CPU over the last %.0fs — a deadlock or a lost wakeup%s%s",
                 a->threads, a->futex, a->cpu_pct, a->secs,
                 stopcont ? "; " : "", stopcont ? stopcont : "");
    } else {
        snprintf(out, size, "UNKNOWN: only one sample taken");
    }
}

/* ── Snapshots ─────────────────────────────────────────────────────────── */

static void dump_file(FILE *out, const char *title, const char *path, size_t max)
{
    char *buf = malloc(max + 1);
    if (!buf)
        return;
    if (read_file(path, buf, max + 1) >= 0)
        fprintf(out, "── %s (%s)\n%s%s", title, path, buf,
                buf[0] && buf[strlen(buf) - 1] == '\n' ? "" : "\n");
    free(buf);
}

/* The cpu lines of /proc/stat: user nice system idle iowait irq softirq
 * steal ...  Steal is time the host did not give the vCPU. */
static void dump_cpu_stat(FILE *out)
{
    char buf[8192];
    if (read_file("/proc/stat", buf, sizeof(buf)) <= 0)
        return;
    fprintf(out, "── /proc/stat cpu (user nice system idle iowait irq softirq steal)\n");
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
        if (strncmp(line, "cpu", 3) == 0)
            fprintf(out, "%s\n", line);
}

/* Append the tail of the kernel log.  The hung-task detector reports there;
 * printk is silenced on the console, not in the buffer. */
static void dump_kernel_log(FILE *out, size_t max)
{
    int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
    if (size <= 0)
        return;
    char *buf = malloc((size_t)size + 1);
    if (!buf)
        return;
    int n = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, buf, size);
    if (n > 0) {
        buf[n] = '\0';
        const char *start = (size_t)n > max ? buf + n - max : buf;
        fprintf(out, "── kernel log (last %zu bytes)\n%s\n", max, start);
    }
    free(buf);
}

/* Lines of the kernel log that name blocked tasks, for the summary. */
static void kernel_log_highlights(FILE *out, int max_lines)
{
    int size = klogctl(10, NULL, 0);
    if (size <= 0)
        return;
    char *buf = malloc((size_t)size + 1);
    if (!buf)
        return;
    int n = klogctl(3, buf, size);
    if (n > 0) {
        buf[n] = '\0';
        int shown = 0;
        for (char *line = strtok(buf, "\n"); line && shown < max_lines;
             line = strtok(NULL, "\n")) {
            if (strstr(line, "blocked for more than") || strstr(line, "hung_task")
                || strstr(line, "soft lockup") || strstr(line, "rcu_sched")
                || strstr(line, "Out of memory")) {
                fprintf(out, "nix-linux-builder:   kernel: %s\n", line);
                shown++;
            }
        }
    }
    free(buf);
}

static void write_threads(FILE *out, const sample_set *prev, const sample_set *cur,
                          bool stacks)
{
    long hz = sysconf(_SC_CLK_TCK);
    double secs = prev && prev->when > 0 ? cur->when - prev->when : 0;
    fprintf(out, "── build threads (%zu)%s\n", cur->n,
            secs > 0 ? "" : " — first sample, no CPU deltas");
    fprintf(out, "%7s %7s %7s %-16s %s %6s %5s %8s %8s %-24s %s\n",
            "pid", "tid", "ppid", "comm", "S", "cpu%", "lcpu", "+vcsw",
            "+ivcsw", "wchan", "syscall");
    for (size_t i = 0; i < cur->n; i++) {
        const thread_sample *t = &cur->t[i];
        const thread_sample *p = prev ? sample_find(prev, t->tid) : NULL;
        char pct[16] = "-", dv[24] = "-", di[24] = "-";
        if (p && secs > 0 && t->ticks >= p->ticks && t->vcsw >= p->vcsw
            && t->ivcsw >= p->ivcsw) {
            snprintf(pct, sizeof(pct), "%.1f",
                     100.0 * (double)(t->ticks - p->ticks) / (double)hz / secs);
            snprintf(dv, sizeof(dv), "%llu", t->vcsw - p->vcsw);
            snprintf(di, sizeof(di), "%llu", t->ivcsw - p->ivcsw);
        }
        fprintf(out, "%7d %7d %7d %-16s %c %6s %5d %8s %8s %-24s %s\n",
                t->pid, t->tid, t->ppid, t->comm, t->state, pct, t->cpu, dv, di,
                t->wchan, t->syscall);
    }

    /* Per process: command line and I/O counters.  cmdline is skipped for a
     * process with a thread in D state: reading it takes the target's
     * mmap lock, which a thread stuck in a page fault on VirtioFS holds. */
    fprintf(out, "── build processes\n");
    for (size_t i = 0; i < cur->n; i++) {
        const thread_sample *t = &cur->t[i];
        if (t->tid != t->pid)
            continue;
        bool blocked = false;
        for (size_t j = 0; j < cur->n; j++)
            if (cur->t[j].pid == t->pid && cur->t[j].state == 'D')
                blocked = true;
        char path[128], buf[2048];
        fprintf(out, "pid %d (%s)\n", t->pid, t->comm);
        if (!blocked) {
            snprintf(path, sizeof(path), "/proc/%d/cmdline", t->pid);
            ssize_t n = read_file(path, buf, 1024);
            for (ssize_t k = 0; k < n; k++)
                if (buf[k] == '\0')
                    buf[k] = ' ';
            fprintf(out, "  cmdline: %s\n", n > 0 ? buf : "?");
        } else {
            fprintf(out, "  cmdline: (not read: a thread is in D state)\n");
        }
        snprintf(path, sizeof(path), "/proc/%d/io", t->pid);
        if (read_file(path, buf, sizeof(buf)) > 0) {
            for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
                fprintf(out, "  %s\n", line);
        }
    }

    if (!stacks)
        return;
    fprintf(out, "── kernel stacks of sleeping and blocked build threads\n");
    for (size_t i = 0; i < cur->n; i++) {
        const thread_sample *t = &cur->t[i];
        if (t->state == 'R')
            continue;
        char path[128], buf[4096];
        snprintf(path, sizeof(path), "/proc/%d/task/%d/stack", t->pid, t->tid);
        if (read_file(path, buf, sizeof(buf)) > 0)
            fprintf(out, "tid %d (%s) %c %s:\n%s", t->tid, t->comm, t->state,
                    t->wchan, buf);
    }
}

static int snapshot_seq = 0;

/* Copy a staged file into the diag dir from a child process, so a stalled
 * VirtioFS share blocks only the child. */
static void publish(const char *diag_dir, const char *staged, const char *name)
{
    if (!diag_dir)
        return;
    pid_t pid = fork();
    if (pid != 0)
        return; /* parent, or fork failed: either way carry on */
    char dst[512];
    mkdir(diag_dir, 0755);
    snprintf(dst, sizeof(dst), "%s/%s", diag_dir, name);
    int in = open(staged, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in >= 0 && out >= 0) {
        char buf[65536];
        ssize_t r;
        while ((r = read(in, buf, sizeof(buf))) > 0)
            write_all(out, buf, (size_t)r);
    }
    _exit(0);
}

/* Write snapshot-<seq>.txt.  `why` says what triggered it. */
static void snapshot(const config *cfg, const sample_set *prev, const sample_set *cur,
                     const char *why, double silent, const char *stopcont,
                     analysis *a_out)
{
    analysis a;
    analyse(prev, cur, &a);
    if (a_out)
        *a_out = a;

    /* No SysRq-w here: it prints its header to the console whatever the
     * printk level, and console output is build output — it would reset
     * nix's silence timer.  The kernel stacks below cover the same ground. */

    char name[64], staged[512];
    snprintf(name, sizeof(name), "snapshot-%03d.txt", ++snapshot_seq);
    snprintf(staged, sizeof(staged), "%s/%s", staging, name);
    FILE *out = fopen(staged, "w");
    if (!out)
        return;

    char v[1024];
    verdict(&a, stopcont, v, sizeof(v));
    time_t wall = time(NULL);
    fprintf(out, "nix-linux-builder hang snapshot %d (%s)\n", snapshot_seq, why);
    fprintf(out, "wall clock: %s", ctime(&wall));
    fprintf(out, "silent for: %.0fs\n", silent);
    fprintf(out, "verdict:    %s\n", v);
    if (stopcont)
        fprintf(out, "stop/cont:  %s\n", stopcont);
    dump_file(out, "uptime", "/proc/uptime", 256);
    dump_file(out, "loadavg", "/proc/loadavg", 256);
    dump_file(out, "pressure cpu", "/proc/pressure/cpu", 512);
    dump_file(out, "pressure io", "/proc/pressure/io", 512);
    dump_file(out, "pressure memory", "/proc/pressure/memory", 512);
    dump_cpu_stat(out);
    dump_file(out, "meminfo", "/proc/meminfo", 4096);
    write_threads(out, prev, cur, true);
    dump_kernel_log(out, 64 * 1024);
    fclose(out);
    publish(cfg->diag_dir, staged, name);
}

/* ── Signalling the build ──────────────────────────────────────────────── */

static size_t signal_build(int sig)
{
    sample_set s = {0};
    sample_take(&s);
    size_t n = 0;
    for (size_t i = 0; i < s.n; i++)
        if (s.t[i].tid == s.t[i].pid && kill(s.t[i].pid, sig) == 0)
            n++;
    sample_free(&s);
    return n;
}

/* ── Main loop ─────────────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr, "usage: nlb-watchdog [-s secs] [-i secs] [-c secs] [-f secs] "
                    "[-d dir] [-H text] -- command [args...]\n");
    exit(2);
}

static int status_to_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

int main(int argc, char *argv[])
{
    config cfg = { .interval = 600 };
    config_from_cmdline(&cfg);

    int opt;
    while ((opt = getopt(argc, argv, "+s:i:c:f:d:H:")) != -1) {
        switch (opt) {
        case 's': cfg.snapshot_after = parse_secs(optarg, "-s"); break;
        case 'i': cfg.interval = parse_secs(optarg, "-i"); break;
        case 'c': cfg.stopcont_after = parse_secs(optarg, "-c"); break;
        case 'f': cfg.fail_after = parse_secs(optarg, "-f"); break;
        case 'd': cfg.diag_dir = optarg; break;
        case 'H': cfg.diag_host = optarg; break;
        default: usage();
        }
    }
    if (optind >= argc)
        usage();

    if (getuid() == 0) {
        mkdir("/run/nlb", 0700);
    } else {
        const char *tmp = getenv("TMPDIR");
        snprintf(staging, sizeof(staging), "%s/nlb-staging-%d",
                 tmp && *tmp ? tmp : "/tmp", (int)getpid());
    }
    mkdir(staging, 0700);

    /* The builder gets a terminal, as it had with hvc0 (and has under the
     * nix daemon).  No output processing: the console was set -opost so
     * '\n' is not turned into "\r\n", which nix's log reader mishandles. */
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        /* No pty: run the builder directly rather than not at all. */
        fprintf(stderr, "nlb-watchdog: openpty: %s; running without watchdog\n",
                strerror(errno));
        execvp(argv[optind], &argv[optind]);
        _exit(127);
    }
    struct termios tio;
    if (tcgetattr(slave, &tio) == 0) {
        tio.c_oflag &= ~(tcflag_t)OPOST;
        tio.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(slave, TCSANOW, &tio);
    }

    pid_t child = fork();
    if (child < 0) {
        perror("nlb-watchdog: fork");
        return 1;
    }
    if (child == 0) {
        close(master);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO)
            close(slave);
        execvp(argv[optind], &argv[optind]);
        fprintf(stderr, "nlb-watchdog: exec %s: %s\n", argv[optind], strerror(errno));
        _exit(127);
    }
    close(slave);
    build_root = child;
    signal(SIGPIPE, SIG_IGN);

    double last_output = now_mono();
    double next_snapshot = 0;      /* silence at which to snapshot next */
    bool stopcont_done = false, stopcont_resumed = false;
    double stopcont_at = 0;
    char stopcont_note[256] = "";
    sample_set prev = {0}, cur = {0};
    int status = 0;
    bool child_done = false;
    double child_done_at = 0;

    for (;;) {
        struct pollfd pfd = { .fd = master, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr > 0) {
            char buf[65536];
            ssize_t r = read(master, buf, sizeof(buf));
            if (r > 0) {
                write_all(STDOUT_FILENO, buf, (size_t)r);
                double t = now_mono();
                if (stopcont_done && !stopcont_resumed && stopcont_at > 0) {
                    stopcont_resumed = true;
                    snprintf(stopcont_note, sizeof(stopcont_note),
                             "output resumed %.0fs after SIGSTOP/SIGCONT",
                             t - stopcont_at);
                }
                if (snapshot_seq > 0 && next_snapshot > 0) {
                    /* Output came back after we started snapshotting: note
                     * it, so a slow-but-alive build is told apart from a
                     * hang in the diag dir. */
                    char staged[512], name[64];
                    snprintf(name, sizeof(name), "resumed-after-%03d.txt", snapshot_seq);
                    snprintf(staged, sizeof(staged), "%s/%s", staging, name);
                    FILE *f = fopen(staged, "w");
                    if (f) {
                        fprintf(f, "output resumed after %.0fs of silence%s%s\n",
                                t - last_output, stopcont_note[0] ? "; " : "",
                                stopcont_note);
                        fclose(f);
                        publish(cfg.diag_dir, staged, name);
                    }
                }
                last_output = t;
                next_snapshot = 0;
                sample_free(&prev);
                continue;
            }
            if (r < 0 && errno == EINTR)
                continue;
            /* EIO: every writer has closed the terminal. */
            if (!child_done) {
                waitpid(child, &status, 0);
                child_done = true;
            }
            break;
        }

        /* Reap the builder and any snapshot-publishing children. */
        int st;
        for (pid_t w; (w = waitpid(-1, &st, WNOHANG)) > 0;)
            if (w == child) {
                status = st;
                child_done = true;
                child_done_at = now_mono();
            }
        if (child_done) {
            /* The builder exited but something it started still holds the
             * terminal open — the build is over, as it was before. */
            if (now_mono() - child_done_at > 1.0)
                break;
            continue;
        }

        double t = now_mono(), silent = t - last_output;

        if (cfg.snapshot_after > 0 && silent >= (double)cfg.snapshot_after
            && (next_snapshot == 0 || silent >= next_snapshot)) {
            sample_take(&cur);
            snapshot(&cfg, prev.n ? &prev : NULL, &cur,
                     next_snapshot == 0 ? "silence threshold" : "interval",
                     silent, stopcont_note[0] ? stopcont_note : NULL, NULL);
            sample_free(&prev);
            prev = cur;
            memset(&cur, 0, sizeof(cur));
            next_snapshot = silent + (double)(cfg.interval > 0 ? cfg.interval : 600);
        }

        if (cfg.stopcont_after > 0 && !stopcont_done
            && silent >= (double)cfg.stopcont_after) {
            /* Snapshot first, so the "before" state is on record. */
            sample_take(&cur);
            snapshot(&cfg, prev.n ? &prev : NULL, &cur, "before SIGSTOP/SIGCONT",
                     silent, NULL, NULL);
            sample_free(&prev);
            prev = cur;
            memset(&cur, 0, sizeof(cur));
            size_t n = signal_build(SIGSTOP);
            struct timespec pause = { 1, 0 };
            nanosleep(&pause, NULL);
            signal_build(SIGCONT);
            stopcont_done = true;
            stopcont_at = now_mono();
            snprintf(stopcont_note, sizeof(stopcont_note),
                     "sent SIGSTOP/SIGCONT to %zu process(es) at %.0fs of silence; "
                     "no output since", n, silent);
            /* Look again a minute later: did CPU come back? */
            next_snapshot = silent + 60;
        }

        if (cfg.fail_after > 0 && silent >= (double)cfg.fail_after) {
            analysis a;
            if (!prev.n || now_mono() - prev.when < 10) {
                /* No earlier sample far enough back to measure CPU against. */
                sample_free(&prev);
                sample_take(&prev);
                struct timespec pause = { 10, 0 };
                nanosleep(&pause, NULL);
            }
            sample_take(&cur);
            snapshot(&cfg, prev.n ? &prev : NULL, &cur, "fail threshold", silent,
                     stopcont_note[0] ? stopcont_note : NULL, &a);

            /* The summary goes to the build log; it is the only part of all
             * this that reaches Hydra. */
            char v[1024];
            verdict(&a, stopcont_note[0] ? stopcont_note : NULL, v, sizeof(v));
            FILE *log = fdopen(dup(STDOUT_FILENO), "w");
            if (log) {
                fprintf(log, "\nnix-linux-builder: no build output for %.0fs (%.0f min); "
                             "diagnosing and failing the build\n", silent, silent / 60);
                fprintf(log, "nix-linux-builder: verdict: %s\n", v);
                fprintf(log, "nix-linux-builder: %zu thread(s): %zu R, %zu S, %zu D, "
                             "%zu T; %zu on a futex, %zu in FUSE/VirtioFS\n",
                        a.threads, a.running, a.sleeping, a.disk, a.stopped,
                        a.futex, a.fuse);
                if (stopcont_note[0])
                    fprintf(log, "nix-linux-builder: stop/cont: %s\n", stopcont_note);
                int shown = 0;
                for (size_t i = 0; i < cur.n && shown < 12; i++) {
                    const thread_sample *th = &cur.t[i];
                    fprintf(log, "nix-linux-builder:   %d/%d %-16s %c %-24s %s\n",
                            th->pid, th->tid, th->comm, th->state, th->wchan,
                            syscall_name(th->syscall));
                    shown++;
                }
                if (cur.n > (size_t)shown)
                    fprintf(log, "nix-linux-builder:   ... %zu more thread(s)\n",
                            cur.n - (size_t)shown);
                kernel_log_highlights(log, 10);
                fprintf(log, "nix-linux-builder: %d snapshot(s) in %s\n", snapshot_seq,
                        cfg.diag_host ? cfg.diag_host
                        : cfg.diag_dir ? cfg.diag_dir : "the VM only (lost with it)");
                fclose(log);
            }

            /* A thread in D state does not die until it wakes, so never wait
             * on the build unboundedly: init powers the VM off regardless. */
            for (int i = 0; i < 50 && signal_build(SIGKILL) > 0; i++) {
                struct timespec pause = { 0, 200 * 1000 * 1000 };
                nanosleep(&pause, NULL);
            }
            /* Give publish() children a moment to land the last snapshot. */
            for (int i = 0; i < 50 && waitpid(-1, NULL, WNOHANG) >= 0; i++) {
                struct timespec pause = { 0, 100 * 1000 * 1000 };
                nanosleep(&pause, NULL);
            }
            return HANG_EXIT_CODE;
        }
    }

    /* Let publish() children land their copies before init powers off. */
    for (int i = 0; i < 50 && waitpid(-1, NULL, WNOHANG) >= 0; i++) {
        struct timespec pause = { 0, 100 * 1000 * 1000 };
        nanosleep(&pause, NULL);
    }
    sample_free(&prev);
    sample_free(&cur);
    return status_to_code(status);
}
