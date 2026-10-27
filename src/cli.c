/*
 * nix-linux-builder — CLI argument parsing
 * Copyright 2025 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group
 * Apache License 2.0
 */

#include "cli.h"
#include "log.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

/* Default memory: 8 GiB */
#define DEFAULT_MEMORY_SIZE (8ULL * 1024 * 1024 * 1024)

/* Query host CPU count via sysctl. Falls back to 4 on failure. */
static uint32_t host_cpu_count(void)
{
    int mib[2] = { CTL_HW, HW_NCPU };
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == 0 && ncpu > 0)
        return (uint32_t)ncpu;
    return 4;
}

static const struct option long_options[] = {
    { "kernel",       required_argument, NULL, 'k' },
    { "initrd",       required_argument, NULL, 'i' },
    { "memory-size",  required_argument, NULL, 'm' },
    { "cpu-count",    required_argument, NULL, 'c' },
    { "timeout",      required_argument, NULL, 't' },
    { "network",      no_argument,       NULL, 'n' },
    { "ramdisk-tmp",  no_argument,       NULL, 'R' },
    { "shell",        no_argument,       NULL, 's' },
    { "debug",        no_argument,       NULL, 'd' },
    { "verbose",      no_argument,       NULL, 'v' },
    { "diag-dir",      required_argument, NULL, 'D' },
    { "hang-snapshot", required_argument, NULL, 'S' },
    { "hang-interval", required_argument, NULL, 'I' },
    { "hang-stopcont", required_argument, NULL, 'C' },
    { "hang-fail",     required_argument, NULL, 'F' },
    { "fd-sample",     required_argument, NULL, 'N' },
    { "help",         no_argument,       NULL, 'h' },
    { NULL, 0, NULL, 0 }
};

void nlb_cli_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] <build.json>\n"
        "       %s --shell [OPTIONS] [<build.json>]\n"
        "\n"
        "Boot a lightweight Linux VM to execute a nix build.\n"
        "\n"
        "Options:\n"
        "  --kernel <path>         Linux ARM64 kernel Image (required)\n"
        "  --initrd <path>         initrd CPIO archive (required)\n"
        "  --memory-size <bytes>   VM memory in bytes (default: 8589934592)\n"
        "  --cpu-count <n>         vCPU count (default: host CPU count)\n"
        "  --timeout <seconds>     Build timeout, 0 = none (default: 0)\n"
        "  --network               Enable NAT networking in guest\n"
        "  --ramdisk-tmp           Use tmpfs for /tmp instead of VirtioFS (faster, limited by RAM)\n"
        "  --shell                 Interactive shell mode (build.json optional)\n"
        "  --debug                 Drop to shell on build failure (requires --shell and build.json)\n"
        "  -v, --verbose           Verbose logging to stderr\n"
        "\n"
        "Hang diagnostics (seconds of build-log silence, 0 = off):\n"
        "  --diag-dir <dir>        Keep hang snapshots in <dir>/<time>-<name>-<pid>/\n"
        "  --hang-snapshot <s>     Snapshot the build's processes after <s> (default: 0)\n"
        "  --hang-interval <s>     ...and every <s> after that (default: 600)\n"
        "  --hang-stopcont <s>     SIGSTOP/SIGCONT the build once after <s> (default: 0)\n"
        "  --hang-fail <s>         Print a diagnosis and fail the build after <s> (default: 0);\n"
        "                          keep it below nix's max-silent-time\n"
        "  --fd-sample <s>         Sample the guest's and the VM process's open files every\n"
        "                          <s>, and report them if the build fails (default: 0)\n"
        "  -h, --help              Show this help\n",
        prog, prog);
}

/* Parse a non-negative number of seconds for option `name`. */
static int parse_secs(const char *arg, const char *name, uint32_t *out)
{
    if (arg[0] == '-') {
        LOG_ERR("invalid --%s: %s", name, arg);
        return -1;
    }
    char *endptr;
    errno = 0;
    unsigned long val = strtoul(arg, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || endptr == arg || val > UINT32_MAX) {
        LOG_ERR("invalid --%s: %s", name, arg);
        return -1;
    }
    *out = (uint32_t)val;
    return 0;
}

int nlb_cli_parse(int argc, char *argv[], nlb_cli_opts *opts)
{
    /* Set defaults */
    memset(opts, 0, sizeof(*opts));
    opts->memory_size = DEFAULT_MEMORY_SIZE;
    opts->cpu_count = host_cpu_count();

    int opt;
    while ((opt = getopt_long(argc, argv, "vh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'k':
            opts->kernel_path = optarg;
            break;
        case 'i':
            opts->initrd_path = optarg;
            break;
        case 'm': {
            /* Reject negative values: strtoull silently wraps them. */
            if (optarg[0] == '-') {
                LOG_ERR("invalid --memory-size: %s", optarg);
                return -1;
            }
            char *endptr;
            errno = 0;
            opts->memory_size = strtoull(optarg, &endptr, 10);
            if (errno != 0 || *endptr != '\0' || endptr == optarg ||
                opts->memory_size == 0) {
                LOG_ERR("invalid --memory-size: %s", optarg);
                return -1;
            }
            break;
        }
        case 'c': {
            if (optarg[0] == '-') {
                LOG_ERR("invalid --cpu-count: %s", optarg);
                return -1;
            }
            char *endptr;
            errno = 0;
            unsigned long val = strtoul(optarg, &endptr, 10);
            if (errno != 0 || *endptr != '\0' || endptr == optarg ||
                val == 0 || val > UINT32_MAX) {
                LOG_ERR("invalid --cpu-count: %s", optarg);
                return -1;
            }
            opts->cpu_count = (uint32_t)val;
            break;
        }
        case 't': {
            if (optarg[0] == '-') {
                LOG_ERR("invalid --timeout: %s", optarg);
                return -1;
            }
            char *endptr;
            errno = 0;
            unsigned long val = strtoul(optarg, &endptr, 10);
            if (errno != 0 || *endptr != '\0' || endptr == optarg ||
                val > UINT32_MAX) {
                LOG_ERR("invalid --timeout: %s", optarg);
                return -1;
            }
            opts->timeout_secs = (uint32_t)val;
            break;
        }
        case 'n':
            opts->network = true;
            break;
        case 'R':
            opts->ramdisk_tmp = true;
            break;
        case 's':
            opts->shell = true;
            break;
        case 'd':
            opts->debug = true;
            break;
        case 'v':
            opts->verbose = true;
            break;
        case 'D':
            if (!optarg[0]) {
                LOG_ERR("--diag-dir cannot be empty");
                return -1;
            }
            opts->diag_dir = optarg;
            break;
        case 'S':
            if (parse_secs(optarg, "hang-snapshot", &opts->hang_snapshot) != 0)
                return -1;
            break;
        case 'I':
            if (parse_secs(optarg, "hang-interval", &opts->hang_interval) != 0)
                return -1;
            break;
        case 'C':
            if (parse_secs(optarg, "hang-stopcont", &opts->hang_stopcont) != 0)
                return -1;
            break;
        case 'F':
            if (parse_secs(optarg, "hang-fail", &opts->hang_fail) != 0)
                return -1;
            break;
        case 'N':
            if (parse_secs(optarg, "fd-sample", &opts->fd_sample) != 0)
                return -1;
            break;
        case 'h':
            nlb_cli_usage(argv[0]);
            return 1;
        default:
            nlb_cli_usage(argv[0]);
            return -1;
        }
    }

    /* --debug requires --shell (can't debug interactively via nix daemon pipes) */
    if (opts->debug && !opts->shell) {
        LOG_ERR("--debug requires --shell");
        return -1;
    }

    /* --shell disables timeout: an interactive session has no meaningful
     * deadline. Warn if the user explicitly set one. */
    if (opts->shell && opts->timeout_secs > 0) {
        LOG_WARN("--shell mode: ignoring --timeout %u (interactive sessions have no timeout)",
                 opts->timeout_secs);
        opts->timeout_secs = 0;
    }

    /* The remaining positional argument is the build.json path.
     * In --shell mode (without --debug), build.json is optional:
     *   --shell alone        → bare shell with /nix/store mounted
     *   --shell <build.json> → shell with build environment loaded
     *   --shell --debug <bj> → run build, drop to shell on failure */
    if (optind < argc) {
        if (optind + 1 < argc) {
            LOG_ERR("unexpected extra argument: %s", argv[optind + 1]);
            return -1;
        }
        opts->build_json_path = argv[optind];
        if (!opts->build_json_path[0]) {
            LOG_ERR("build.json path cannot be empty");
            return -1;
        }
    } else if (!opts->shell) {
        /* Normal (non-shell) mode: build.json is required */
        LOG_ERR("missing required positional argument: <build.json>");
        nlb_cli_usage(argv[0]);
        return -1;
    } else if (opts->debug) {
        /* --shell --debug: need build.json for the build environment */
        LOG_ERR("--shell --debug requires a <build.json> argument");
        return -1;
    }
    /* else: --shell without build.json → bare shell (build_json_path stays NULL) */

    /* Validate required options */
    if (!opts->kernel_path || !opts->kernel_path[0]) {
        LOG_ERR("missing required option: --kernel");
        return -1;
    }
    if (!opts->initrd_path || !opts->initrd_path[0]) {
        LOG_ERR("missing required option: --initrd");
        return -1;
    }

    return 0;
}
