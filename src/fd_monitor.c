/*
 * nix-linux-builder — Host open-file sampling
 * Copyright 2025 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group
 * Apache License 2.0
 *
 * The VM runs in a com.apple.Virtualization.VirtualMachine XPC process that
 * launchd starts for us.  It serves the VirtioFS shares and holds a host fd
 * for every file the guest has looked up, so a busy build can leave it with
 * 100k or more.  When the host refuses it more, the guest sees ENFILE ("Too
 * many open files in system") on /nix/store, even with the host's
 * kern.num_files well below kern.maxfiles.
 *
 * This samples that process's open files and kern.num_files on a thread of
 * its own.  The XPC process runs as our uid (the nix build user), which is
 * unique to this build, so it is the newest process of ours running the
 * Virtualization XPC service.  Samples are kept in memory and only written
 * out if the build fails.
 */

#include "fd_monitor.h"
#include "log.h"

#include <errno.h>
#include <libproc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#ifndef VM_SERVICE_PATH
#define VM_SERVICE_PATH "/com.apple.Virtualization.VirtualMachine.xpc/"
#endif

typedef struct {
    time_t wall;
    double at;        /* seconds since the monitor started */
    int vm_fds;       /* -1: VM process not found or not readable */
    int host_files;   /* kern.num_files, -1 if unreadable */
} fd_sample;

struct nlb_fd_monitor {
    uint32_t interval;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool stop;
    double start;
    time_t start_wall;
    pid_t vm_pid;
    int vm_errno;     /* why the last vm_fds was -1 */
    fd_sample *s;
    size_t n, cap;
};

static double now_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int sysctl_int(const char *name)
{
    int v = -1;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return -1;
    return v;
}

/* Our newest Virtualization XPC process started since not_before, or 0. */
static pid_t find_vm_process(time_t not_before)
{
    int n = proc_listallpids(NULL, 0);
    if (n <= 0)
        return 0;
    size_t cap = (size_t)n + 64;
    pid_t *pids = calloc(cap, sizeof(*pids));
    if (!pids)
        return 0;
    n = proc_listallpids(pids, (int)(cap * sizeof(*pids)));
    uid_t uid = getuid();
    pid_t best = 0;
    uint64_t best_start = 0;
    for (int i = 0; i < n; i++) {
        struct proc_bsdinfo bi;
        if (proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != (int)sizeof(bi))
            continue;
        if (bi.pbi_uid != uid || bi.pbi_start_tvsec + 1 < (uint64_t)not_before)
            continue;
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pids[i], path, sizeof(path)) <= 0
            || !strstr(path, VM_SERVICE_PATH))
            continue;
        if (bi.pbi_start_tvsec >= best_start) {
            best = pids[i];
            best_start = bi.pbi_start_tvsec;
        }
    }
    free(pids);
    return best;
}

/* Open fds of pid, or -1 with errno set. */
static int count_fds(pid_t pid)
{
    /* With no buffer this gives the size of the fd table, an upper bound. */
    int size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0);
    if (size <= 0)
        return -1;
    struct proc_fdinfo *buf = malloc((size_t)size);
    if (!buf)
        return -1;
    int got = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, buf, size);
    int saved = errno;
    free(buf);
    errno = saved;
    return got <= 0 ? -1 : got / (int)sizeof(struct proc_fdinfo);
}

static void take_sample(nlb_fd_monitor *m)
{
    fd_sample s = { .wall = time(NULL), .at = now_mono() - m->start, .vm_fds = -1 };
    if (!m->vm_pid)
        m->vm_pid = find_vm_process(m->start_wall);
    if (m->vm_pid) {
        s.vm_fds = count_fds(m->vm_pid);
        if (s.vm_fds < 0)
            m->vm_errno = errno;
    }
    s.host_files = sysctl_int("kern.num_files");

    pthread_mutex_lock(&m->lock);
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 256;
        fd_sample *ns = realloc(m->s, cap * sizeof(*ns));
        if (ns) {
            m->s = ns;
            m->cap = cap;
        }
    }
    if (m->n < m->cap)
        m->s[m->n++] = s;
    pthread_mutex_unlock(&m->lock);
}

static void *run(void *arg)
{
    nlb_fd_monitor *m = arg;
    pthread_mutex_lock(&m->lock);
    while (!m->stop) {
        pthread_mutex_unlock(&m->lock);
        take_sample(m);
        pthread_mutex_lock(&m->lock);
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += m->interval;
        while (!m->stop && pthread_cond_timedwait(&m->cond, &m->lock, &deadline) != ETIMEDOUT)
            ;
    }
    pthread_mutex_unlock(&m->lock);
    return NULL;
}

nlb_fd_monitor *nlb_fd_monitor_start(uint32_t interval_secs)
{
    if (interval_secs == 0)
        return NULL;
    nlb_fd_monitor *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->interval = interval_secs;
    m->start = now_mono();
    m->start_wall = time(NULL);
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->cond, NULL);
    if (pthread_create(&m->thread, NULL, run, m) != 0) {
        LOG_WARN("fd sampling: pthread_create failed; running without it");
        pthread_mutex_destroy(&m->lock);
        pthread_cond_destroy(&m->cond);
        free(m);
        return NULL;
    }
    return m;
}

static void write_samples(const nlb_fd_monitor *m, const char *path, int maxfiles)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        LOG_WARN("fd sampling: %s: %s", path, strerror(errno));
        return;
    }
    fprintf(f, "# host open files every %us: UTC time, time since the VM started, "
               "files open in the VM process (pid %d), kern.num_files of kern.maxfiles %d\n",
            m->interval, (int)m->vm_pid, maxfiles);
    for (size_t i = 0; i < m->n; i++) {
        char stamp[32];
        struct tm tm;
        gmtime_r(&m->s[i].wall, &tm);
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
        fprintf(f, "%s %7.0fs %9d %9d\n", stamp, m->s[i].at, m->s[i].vm_fds,
                m->s[i].host_files);
    }
    fclose(f);
}

void nlb_fd_monitor_finish(nlb_fd_monitor *m, const char *diag_build_dir, bool failed)
{
    if (!m)
        return;
    pthread_mutex_lock(&m->lock);
    m->stop = true;
    pthread_cond_signal(&m->cond);
    pthread_mutex_unlock(&m->lock);
    pthread_join(m->thread, NULL);

    if (failed) {
        int maxfiles = sysctl_int("kern.maxfiles");
        const fd_sample *vm_peak = NULL, *host_peak = NULL;
        for (size_t i = 0; i < m->n; i++) {
            if (m->s[i].vm_fds >= 0 && (!vm_peak || m->s[i].vm_fds > vm_peak->vm_fds))
                vm_peak = &m->s[i];
            if (!host_peak || m->s[i].host_files > host_peak->host_files)
                host_peak = &m->s[i];
        }
        char path[1024] = "";
        if (diag_build_dir) {
            snprintf(path, sizeof(path), "%s/host-fd-samples.txt", diag_build_dir);
            write_samples(m, path, maxfiles);
        }
        char vm[160];
        if (vm_peak)
            snprintf(vm, sizeof(vm), "VM process (pid %d) peak %d at %.0f min",
                     (int)m->vm_pid, vm_peak->vm_fds, vm_peak->at / 60);
        else if (m->vm_pid)
            snprintf(vm, sizeof(vm), "VM process (pid %d) unreadable: %s",
                     (int)m->vm_pid, strerror(m->vm_errno));
        else
            snprintf(vm, sizeof(vm), "VM process not found");
        LOG_INFO("host open files: %s; kern.num_files peak %d of kern.maxfiles %d "
                 "at %.0f min (%zu samples%s%s)",
                 vm, host_peak ? host_peak->host_files : -1, maxfiles,
                 host_peak ? host_peak->at / 60 : 0.0, m->n,
                 path[0] ? " in " : "", path);
    }

    pthread_mutex_destroy(&m->lock);
    pthread_cond_destroy(&m->cond);
    free(m->s);
    free(m);
}
