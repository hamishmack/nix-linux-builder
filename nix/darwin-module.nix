# nix-linux-builder — nix-darwin module
# Copyright 2025 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group
# Apache License 2.0
#
# Import this module in your nix-darwin configuration to enable
# nix-linux-builder as an external builder for Linux derivations.
#
# Usage in flake.nix:
#   inputs.nix-linux-builder.url = "github:input-output-hk/nix-linux-builder";
#   # then in darwinConfigurations modules:
#   nix-linux-builder.darwinModules.default
#   { services.nix-linux-builder.enable = true; }
#
{ self }:

{ config, lib, pkgs, ... }:

let
  cfg = config.services.nix-linux-builder;

  builder = self.packages.aarch64-darwin.nix-linux-builder;

  # Use prebuilt guest components (fetched on macOS, no linux builder needed)
  # when available. Falls back to building from source (requires an existing
  # aarch64-linux builder, e.g. a remote builder or previous bootstrap).
  hasPrebuilt = self.prebuiltGuest != null;
  usePrebuilt = cfg.usePrebuilt && hasPrebuilt;

  guest-kernel =
    if usePrebuilt
    then self.prebuiltGuest.guest-kernel
    else self.packages.aarch64-linux.guest-kernel;

  guest-initrd =
    if usePrebuilt
    then self.prebuiltGuest.guest-initrd
    else self.packages.aarch64-linux.guest-initrd;

  # The store binary is used directly: it is signed in the derivation's
  # postFixup with the com.apple.security.virtualization entitlement, which
  # survives stdenv's fixup re-signing (see flake.nix).  Earlier versions
  # copied it to /usr/local/lib and re-signed it at activation, because a
  # signature applied in installPhase was replaced by fixup and the entitlement
  # was lost.  Signing after fixup removes the need for that copy — and for
  # any activation script that re-signs a store path in place, which mutates
  # the path's content and permanently breaks `nix-store --verify` and any
  # attempt to copy it to another machine.
  builderBin = "${builder}/bin/nix-linux-builder";

  # Build the args list from module options.
  builderArgs = lib.concatLists [
    [ "--kernel" "${guest-kernel}/Image" ]
    [ "--initrd" "${guest-initrd}/initrd" ]
    (lib.optional (cfg.memorySize != null)
      [ "--memory-size" (toString cfg.memorySize) ])
    (lib.optional (cfg.cpuCount != null)
      [ "--cpu-count" (toString cfg.cpuCount) ])
    (lib.optional (cfg.timeout != 0)
      [ "--timeout" (toString cfg.timeout) ])
    (lib.optional cfg.network [ "--network" ])
    (lib.optional cfg.ramdiskTmp [ "--ramdisk-tmp" ])
    (lib.optional cfg.verbose [ "--verbose" ])
    (lib.optionals hang.enable [
      "--diag-dir" hang.dir
      "--hang-snapshot" (toString hang.snapshotAfter)
      "--hang-interval" (toString hang.interval)
      "--hang-stopcont" (toString hang.stopContAfter)
      "--hang-fail" (toString hang.failAfter)
      "--fd-sample" (toString hang.fdSampleInterval)
    ])
  ];
  hang = cfg.hangDiagnostics;
  # nix-linux-shell wrapper: substitutes kernel/initrd/builder paths
  # so users can interactively debug Linux builds without knowing
  # the store paths of guest components.
  # Uses writeShellApplication to declare runtime dependencies (jq)
  # and get shellcheck validation at build time.
  nixLinuxShellText =
    builtins.replaceStrings
      [ "@kernel@" "@initrd@" "@builder@" ]
      [ "${guest-kernel}/Image" "${guest-initrd}/initrd" builderBin ]
      (builtins.readFile ../scripts/nix-linux-shell);

  nixLinuxShell = pkgs.writeShellApplication {
    name = "nix-linux-shell";
    runtimeInputs = [ pkgs.jq ];
    # The script already has set -euo pipefail and a shebang;
    # writeShellApplication prepends its own, so the originals
    # become harmless comments/no-ops in the body.
    text = nixLinuxShellText;
  };
in {
  options.services.nix-linux-builder = {
    enable = lib.mkEnableOption "nix-linux-builder external builder for Linux derivations";

    usePrebuilt = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Use pre-built guest kernel and initrd from GitHub releases.
        This avoids the chicken-and-egg problem of needing an aarch64-linux
        builder to build the guest components that provide the builder.
        Set to false to build from source (requires an aarch64-linux builder).
      '';
    };

    systems = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ "aarch64-linux" "x86_64-linux" ];
      description = "Linux systems this builder handles.";
    };

    memorySize = lib.mkOption {
      type = lib.types.nullOr lib.types.int;
      default = null;
      description = "VM memory in bytes. Defaults to 8 GiB if unset.";
    };

    cpuCount = lib.mkOption {
      type = lib.types.nullOr lib.types.int;
      default = null;
      description = "Number of vCPUs. Defaults to host CPU count if unset.";
    };

    timeout = lib.mkOption {
      type = lib.types.int;
      default = 0;
      description = "Build timeout in seconds. 0 means unlimited.";
    };

    network = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Enable NAT networking in the guest VM.
        Required for fixed-output derivations (fetchgit, fetchurl, etc.)
        that need network access during build. The nix external-builder
        protocol does not indicate whether a derivation needs network,
        so networking is enabled unconditionally.
      '';
    };

    ramdiskTmp = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Use tmpfs for /tmp instead of VirtioFS (faster, limited by RAM).";
    };

    verbose = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Enable verbose debug logging.";
    };

    # Thresholds are seconds without build output (what nix's
    # max-silent-time measures); 0 turns that step off.
    hangDiagnostics = {
      enable = lib.mkEnableOption ''
        diagnostics for builds that stop producing output: the guest
        snapshots the build's processes and threads (state, wchan, syscall,
        kernel stacks, CPU), PSI, CPU steal and the kernel log
        into `dir`, and can probe and fail the build with a verdict in its
        log.  Requires a guest built from this source (`usePrebuilt = false`
        until a release includes it)'';

      dir = lib.mkOption {
        type = lib.types.str;
        default = "/var/log/nix-linux-builder/hangs";
        description = ''
          Snapshots go to `<dir>/<UTC time>-<derivation name>-<pid>/`.
          Builds that never go silent leave nothing behind.
        '';
      };

      snapshotAfter = lib.mkOption {
        type = lib.types.ints.unsigned;
        default = 1200;
        description = "First snapshot after this many seconds of silence.";
      };

      interval = lib.mkOption {
        type = lib.types.ints.unsigned;
        default = 600;
        description = "Further snapshots every this many seconds while silent.";
      };

      stopContAfter = lib.mkOption {
        type = lib.types.ints.unsigned;
        default = 0;
        description = ''
          Send SIGSTOP then SIGCONT to every build process once, after this
          many seconds of silence, and record whether output or CPU comes
          back (as it does for a lost futex wakeup).
        '';
      };

      failAfter = lib.mkOption {
        type = lib.types.ints.unsigned;
        default = 0;
        description = ''
          Print a summary and verdict to the build log, then fail the build
          (exit 124), after this many seconds of silence.  Must be below
          nix's (or Hydra's) max-silent-time: any output resets nix's silence
          timer, so a summary printed earlier would only postpone the kill.
        '';
      };

      fdSampleInterval = lib.mkOption {
        type = lib.types.ints.unsigned;
        default = 30;
        description = ''
          Every this many seconds, whether the build is silent or not, record
          the guest's open files (/proc/sys/fs/file-nr) and those of the VM's
          Virtualization.framework process on the host.  If the build
          fails, the samples go to `fd-samples.txt` and
          `host-fd-samples.txt` in its directory under `dir` and the peaks
          to its log.  For ENFILE ("Too many open files in system").
          0 turns it off.
        '';
      };

      retentionDays = lib.mkOption {
        type = lib.types.ints.positive;
        default = 14;
        description = "Delete snapshot directories older than this, daily.";
      };
    };
  };

  config = lib.mkIf cfg.enable {
    # No activation script: the store binary already carries the
    # com.apple.security.virtualization entitlement (signed in postFixup), so
    # Virtualization.framework starts VMs straight from the store path.

    # Install nix-linux-shell wrapper for interactive debugging.
    environment.systemPackages = [ nixLinuxShell ];

    # The builder runs as the nix build user, so the snapshot directory must
    # be writable by group nixbld; sticky, like /tmp, so builds cannot remove
    # each other's.
    system.activationScripts.postActivation.text = lib.mkIf hang.enable ''
      mkdir -p ${lib.escapeShellArg hang.dir}
      chown root:nixbld ${lib.escapeShellArg hang.dir}
      chmod 1775 ${lib.escapeShellArg hang.dir}
    '';

    launchd.daemons.nix-linux-builder-hang-prune = lib.mkIf hang.enable {
      script = ''
        find ${lib.escapeShellArg hang.dir} -mindepth 1 -maxdepth 1 -type d \
          -mtime +${toString hang.retentionDays} -exec rm -rf {} +
      '';
      serviceConfig.StartCalendarInterval = [ { Hour = 4; Minute = 17; } ];
    };

    nix.settings = {
      extra-experimental-features = [ "external-builders" ];
      external-builders = builtins.toJSON [
        {
          systems = cfg.systems;
          program = builderBin;
          args = lib.flatten builderArgs;
        }
      ];
    };
  };
}
