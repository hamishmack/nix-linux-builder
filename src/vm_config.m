/*
 * nix-linux-builder — Virtualization.framework VM configuration
 * Copyright 2025 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group
 * Apache License 2.0
 *
 * Creates a VZVirtualMachineConfiguration with:
 *   - VZLinuxBootLoader: kernel Image + initrd, console=hvc0
 *   - VZVirtioConsoleDeviceSerialPortConfiguration: hvc0 → host stdout
 *   - VZVirtioFileSystemDeviceConfiguration "nix-store": /nix/store (host r/w)
 *   - VZVirtioFileSystemDeviceConfiguration "buildroot": topTmpDir (host r/w)
 *   - VZVirtioFileSystemDeviceConfiguration "rosetta": Rosetta share (x86_64 only)
 *   - VZVirtioFileSystemDeviceConfiguration "diag": hang snapshots (--diag-dir)
 *   - VZVirtioEntropyDeviceConfiguration: guest /dev/random
 *   - VZVirtioTraditionalMemoryBalloonDeviceConfiguration
 *   - Optional VZNATNetworkDeviceAttachment for networking
 */

#import "vm_config.h"
#include "log.h"

#import <Foundation/Foundation.h>
#include <unistd.h>

/* Build the kernel command line for the guest.
 * Flags like ramdisk_tmp are passed here so the guest init can read
 * /proc/cmdline — no separate communication channel needed. */
static NSString *kernel_cmdline(const nlb_cli_opts *opts)
{
    /* console=hvc0 routes all output through the Virtio console serial port,
     * which we attach to host stdout so nix can read build logs. */
    NSMutableString *cmdline = [NSMutableString string];

    if (opts->verbose)
        [cmdline appendString:@"console=hvc0 loglevel=7"];
    else
        [cmdline appendString:@"console=hvc0 loglevel=0 quiet"];

    /* Tell guest init to use tmpfs for /tmp instead of VirtioFS. */
    if (opts->ramdisk_tmp)
        [cmdline appendString:@" ramdisk_tmp"];

    /* Interactive shell mode flags — guest init reads these from /proc/cmdline
     * to decide whether to drop to an interactive shell or run the builder.
     * Namespaced with "nlb." to avoid conflicts with real kernel parameters
     * (e.g., the kernel's "debug" early_param sets console_loglevel=10). */
    if (opts->shell)
        [cmdline appendString:@" nlb.shell"];
    if (opts->debug)
        [cmdline appendString:@" nlb.debug"];

    /* Hang diagnostics, read by guest/nlb-watchdog.c.  A threshold of 0 is
     * left out (off).  nlb.diag_host only labels the summary in the build
     * log, so a reader knows where on this host the snapshots are. */
    if (opts->hang_snapshot)
        [cmdline appendFormat:@" nlb.hang_snapshot=%u", opts->hang_snapshot];
    if (opts->hang_interval)
        [cmdline appendFormat:@" nlb.hang_interval=%u", opts->hang_interval];
    if (opts->hang_stopcont)
        [cmdline appendFormat:@" nlb.hang_stopcont=%u", opts->hang_stopcont];
    if (opts->hang_fail)
        [cmdline appendFormat:@" nlb.hang_fail=%u", opts->hang_fail];
    if (opts->fd_sample)
        [cmdline appendFormat:@" nlb.fd_sample=%u", opts->fd_sample];
    if (opts->drop_caches)
        [cmdline appendFormat:@" nlb.drop_caches=%u", opts->drop_caches];
    if (opts->diag_build_dir) {
        /* gethostname, not NSProcessInfo.hostName: the latter can block on
         * a reverse DNS lookup. */
        char host[256] = "localhost";
        gethostname(host, sizeof(host) - 1);
        [cmdline appendFormat:@" nlb.diag_dir=/run/nlb/diag nlb.diag_host=%s:%s",
            host, opts->diag_build_dir];
    }

    return cmdline;
}

/* Create a VirtioFS share for a host directory. */
static VZVirtioFileSystemDeviceConfiguration *make_virtiofs_share(
    NSString *tag, NSString *path)
{
    NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
    VZSingleDirectoryShare *share =
        [[VZSingleDirectoryShare alloc] initWithDirectory:
            [[VZSharedDirectory alloc] initWithURL:url readOnly:NO]];

    VZVirtioFileSystemDeviceConfiguration *fs =
        [[VZVirtioFileSystemDeviceConfiguration alloc] initWithTag:tag];
    fs.share = share;
    return fs;
}

VZVirtualMachineConfiguration *nlb_create_vm_config(
    const nlb_cli_opts *opts,
    const nlb_build_spec *spec)
{
    @autoreleasepool {
        NSError *error = nil;
        VZVirtualMachineConfiguration *config =
            [[VZVirtualMachineConfiguration alloc] init];

        /* ── Boot loader ───────────────────────────────────────────────── */
        NSURL *kernelURL = [NSURL fileURLWithPath:
            [NSString stringWithUTF8String:opts->kernel_path]];
        NSURL *initrdURL = [NSURL fileURLWithPath:
            [NSString stringWithUTF8String:opts->initrd_path]];

        VZLinuxBootLoader *bootLoader =
            [[VZLinuxBootLoader alloc] initWithKernelURL:kernelURL];
        bootLoader.initialRamdiskURL = initrdURL;
        bootLoader.commandLine = kernel_cmdline(opts);
        config.bootLoader = bootLoader;

        /* ── Platform ──────────────────────────────────────────────────── */
        config.platform = [[VZGenericPlatformConfiguration alloc] init];

        /* ── CPU + Memory ──────────────────────────────────────────────── */
        config.CPUCount = opts->cpu_count;
        config.memorySize = opts->memory_size;

        /* ── Serial console (hvc0 → host stdout) ──────────────────────── */
        /* The guest's hvc0 is connected to our stdout via file handle
         * attachment. nix reads build logs from this fd, and the guest
         * emits the \2\n ready signal through it. */
        NSFileHandle *stdoutHandle = [NSFileHandle fileHandleWithStandardOutput];
        NSFileHandle *stdinHandle;
        if (opts->shell) {
            /* Interactive mode: connect host stdin → guest hvc0 so the
             * user can type commands into the guest shell. */
            stdinHandle = [NSFileHandle fileHandleWithStandardInput];
        } else {
            /* Build mode: open /dev/null for the reading end — the guest
             * doesn't need host→guest input, but VZFileHandleSerialPortAttachment
             * requires a valid file descriptor. */
            stdinHandle = [NSFileHandle fileHandleForReadingAtPath:@"/dev/null"];
            if (!stdinHandle) {
                LOG_ERR("failed to open /dev/null for reading");
                return nil;
            }
        }

        VZFileHandleSerialPortAttachment *serialAttach =
            [[VZFileHandleSerialPortAttachment alloc]
                initWithFileHandleForReading:stdinHandle
                fileHandleForWriting:stdoutHandle];

        VZVirtioConsoleDeviceSerialPortConfiguration *serial =
            [[VZVirtioConsoleDeviceSerialPortConfiguration alloc] init];
        serial.attachment = serialAttach;
        config.serialPorts = @[serial];

        /* ── VirtioFS shares ───────────────────────────────────────────── */
        NSMutableArray<VZVirtioFileSystemDeviceConfiguration *> *fsDevices =
            [NSMutableArray array];

        /* nix-store share: maps host /nix/store into the guest.
         * The nix daemon ensures build user has write access to
         * scratch output paths, which flows through VirtioFS. */
        NSString *storeDir = [NSString stringWithUTF8String:
            spec->real_store_dir ? spec->real_store_dir : "/nix/store"];
        [fsDevices addObject:make_virtiofs_share(@"nix-store", storeDir)];

        /* buildroot share: maps topTmpDir into the guest.
         * Contains build.json and receives .exitcode on completion. */
        NSString *topTmpDir = [NSString stringWithUTF8String:spec->top_tmp_dir];
        [fsDevices addObject:make_virtiofs_share(@"buildroot", topTmpDir)];

        /* Rosetta share: only for x86_64-linux builds.
         * Provides the Rosetta binary translator so aarch64 VM can
         * run x86_64 Linux binaries via binfmt_misc. */
        if (nlb_build_spec_needs_rosetta(spec)) {
            LOG_DBG("enabling Rosetta share for x86_64-linux build");

            if (@available(macOS 13.0, *)) {
                VZLinuxRosettaDirectoryShare *rosettaShare =
                    [[VZLinuxRosettaDirectoryShare alloc] initWithError:&error];
                if (!rosettaShare) {
                    LOG_ERR("failed to create Rosetta share: %s",
                            error ? error.localizedDescription.UTF8String : "unknown error");
                    return nil;
                }

                VZVirtioFileSystemDeviceConfiguration *rosettaFS =
                    [[VZVirtioFileSystemDeviceConfiguration alloc]
                        initWithTag:@"rosetta"];
                rosettaFS.share = rosettaShare;
                [fsDevices addObject:rosettaFS];
            } else {
                LOG_ERR("Rosetta requires macOS 13.0+");
                return nil;
            }
        }

        /* diag share: this build's hang-snapshot directory (--diag-dir).
         * Unlike topTmpDir it outlives the build, so the snapshots of a
         * build that nix kills are still there afterwards. */
        if (opts->diag_build_dir) {
            NSString *diagDir = [NSString stringWithUTF8String:opts->diag_build_dir];
            [fsDevices addObject:make_virtiofs_share(@"diag", diagDir)];
        }

        config.directorySharingDevices = fsDevices;

        /* ── Entropy device ────────────────────────────────────────────── */
        config.entropyDevices = @[
            [[VZVirtioEntropyDeviceConfiguration alloc] init]
        ];

        /* ── Memory balloon ────────────────────────────────────────────── */
        config.memoryBalloonDevices = @[
            [[VZVirtioTraditionalMemoryBalloonDeviceConfiguration alloc] init]
        ];

        /* ── Optional NAT networking ───────────────────────────────────── */
        if (opts->network) {
            VZNATNetworkDeviceAttachment *nat =
                [[VZNATNetworkDeviceAttachment alloc] init];
            VZVirtioNetworkDeviceConfiguration *net =
                [[VZVirtioNetworkDeviceConfiguration alloc] init];
            net.attachment = nat;
            config.networkDevices = @[net];
            LOG_DBG("NAT networking enabled");
        }

        /* ── Validate ──────────────────────────────────────────────────── */
        if (![config validateWithError:&error]) {
            LOG_ERR("VM configuration validation failed: %s",
                    error ? error.localizedDescription.UTF8String : "unknown error");
            return nil;
        }

        LOG_DBG("VM config: %u CPUs, %llu MB RAM, %lu FS shares",
                opts->cpu_count,
                (unsigned long long)(opts->memory_size / (1024 * 1024)),
                (unsigned long)fsDevices.count);

        return config;
    }
}
