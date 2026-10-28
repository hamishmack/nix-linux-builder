# nix-linux-builder Usage

## Building

```bash
# Enter nix development shell and build
nix develop -c make build

# Build and codesign (required for Virtualization.framework)
nix develop -c make sign

# Build guest kernel and initrd via nix
nix build .#guest-kernel
nix build .#guest-initrd
```

## Command Line

```
nix-linux-builder [OPTIONS] <build.json>
nix-linux-builder --shell [OPTIONS] [<build.json>]

Options:
  --kernel <path>         Path to Linux ARM64 kernel Image (required)
  --initrd <path>         Path to initrd CPIO archive (required)
  --memory-size <bytes>   VM memory in bytes (default: 8589934592 = 8GiB)
  --cpu-count <n>         Number of vCPUs (default: host CPU count)
  --timeout <seconds>     Build timeout, 0 = none (default: 0)
  --network               Enable NAT networking in guest
  --ramdisk-tmp           Use tmpfs for /tmp instead of VirtioFS (faster, limited by RAM)
  --shell                 Interactive shell mode (build.json optional)
  --debug                 Drop to shell on build failure (requires --shell and build.json)
  -v, --verbose           Enable verbose logging
  -h, --help              Show help message

Hang diagnostics (seconds of build-log silence, 0 = off):
  --diag-dir <dir>        Keep hang snapshots in <dir>/<time>-<name>-<pid>/
  --hang-snapshot <s>     Snapshot the build's processes after <s> (default: 0)
  --hang-interval <s>     ...and every <s> after that (default: 600)
  --hang-stopcont <s>     SIGSTOP/SIGCONT the build once after <s> (default: 0)
  --hang-fail <s>         Print a diagnosis and fail the build after <s> (default: 0)
  --fd-sample <s>         Sample the guest's and the VM process's open files every
                          <s>, and report them if the build fails (default: 0)
  --drop-caches <s>       Drop the guest's dentry and inode caches every <s>, so
                          VirtioFS releases the host files (and vnodes) behind
                          them (default: 0)

Positional:
  <build.json>            Path to nix build specification JSON file
```

## Hang diagnostics

A build that hangs inside the VM usually ends with nix's
`timed out after N seconds` (max-silent-time) and nothing else: nix kills the
VM, and with it every clue. With the `--hang-*` options the guest watches the
build's output and, once it has been silent long enough, records why:

```bash
nix-linux-builder ... \
  --diag-dir /var/log/nix-linux-builder/hangs \
  --hang-snapshot 1200 --hang-interval 600 \
  --hang-stopcont 3600 --hang-fail 6600 build.json
```

- `--hang-snapshot` / `--hang-interval`: write `snapshot-NNN.txt` files to
  `<diag-dir>/<UTC time>-<derivation name>-<pid>/`. Each lists every build
  thread (state, wchan, syscall, CPU % since the previous snapshot, context
  switches), every build process (command line, I/O), kernel stacks of the
  sleeping and blocked threads, PSI, `/proc/stat` and the kernel log tail. If output resumes, a
  `resumed-after-NNN.txt` says so (a slow build, not a hang). Builds that
  never go silent leave no directory behind.
- `--hang-stopcont`: SIGSTOP then SIGCONT every build process once. Output
  coming back afterwards points at a lost wakeup in user space.
- `--hang-fail`: print a verdict and the build's threads to the build log, kill
  the build and exit 124. Keep it **below** nix's max-silent-time: any output
  resets nix's timer, so the verdict has to come with the failure. The verdict
  is one of:
  - `VIRTIOFS`: threads in D state waiting on FUSE (VirtioFS): the host side of
    a share stopped answering;
  - `KERNEL-BLOCKED`: threads in D state elsewhere;
  - `BUSY`: the build is using CPU but printing nothing (spin, livelock,
    infinite loop);
  - `IDLE`: everything asleep (deadlock, lost wakeup), with the SIGSTOP/SIGCONT
    outcome if the probe ran.

### Open files

`--fd-sample <s>` is for builds that fail with ENFILE ("Too many open files in
system"). Every `<s>` seconds, silent or not, the guest records
`/proc/sys/fs/file-nr`, and once more than half of `fs.file-max` is in use,
the processes holding the most fds and how many file structures no fd points
at (mmaps, files only the kernel holds). On the host, a thread records how many
files the VM's `com.apple.Virtualization.VirtualMachine` process holds open
(it serves the VirtioFS shares, with a host fd per file the guest has looked
up) and `kern.num_files`. If the build fails, both sets go to the build's diag
directory as `fd-samples.txt` and `host-fd-samples.txt`, and the build log
ends with the peaks:

```
nix-linux-builder: guest open files: peak 41210 of fs.file-max 1220080 at 52 min (107 samples in host:/var/log/.../fd-samples.txt)
nix-linux-builder: host open files: VM process (pid 62352) peak 91628 at 55 min; kern.num_files peak 276047 of kern.maxfiles 524288 at 49 min (110 samples in /var/log/.../host-fd-samples.txt)
```

Successful builds report nothing.

### Host vnodes

ENFILE can come from the host's vnode table rather than its file table.
Apple's VirtioFS keeps a host file open, and so a vnode, for every inode the
guest has cached, until the guest forgets the inode; a guest with plenty of
memory never does. A long build (GHC touches 100k+ inodes) then holds that
many vnodes for hours, and once `kern.free_vnodes` runs out (`kern.maxvnodes`
is about 250k on a 16 GiB Mac) opening any file on the host fails with
"Too many open files in system", however high `kern.maxfiles` is.

`--drop-caches <s>` (darwin module `dropCachesInterval`) makes the guest drop
its dentry and inode caches every `<s>` seconds (`echo 2 >
/proc/sys/vm/drop_caches`), so VirtioFS forgets the inodes and the host closes
their files. Files the build still has open are not affected; the build only
pays for looking paths up again.

With the darwin module:

```nix
services.nix-linux-builder.hangDiagnostics = {
  enable = true;
  stopContAfter = 3600;
  failAfter = 6600;   # Hydra's default max-silent-time is 7200
};
```

## Interactive Shell (nix-linux-shell)

```bash
# Plain shell with /nix/store mounted
nix-linux-shell

# Shell with a derivation's build environment
nix-linux-shell /nix/store/...-foo.drv

# Debug: run the build, drop to shell on failure
nix-linux-shell --debug /nix/store/...-foo.drv

# From a flake ref (resolved to .drv automatically)
nix-linux-shell nixpkgs#hello
```

### Direct invocation (without wrapper)

```bash
# Bare shell (no build.json needed)
sudo .build/nix-linux-builder --shell --kernel /path/to/Image --initrd /path/to/initrd --network

# Shell with build environment
sudo .build/nix-linux-builder --shell --kernel /path/to/Image --initrd /path/to/initrd --network build.json

# Debug mode: run build, drop to shell on failure
sudo .build/nix-linux-builder --shell --debug --kernel /path/to/Image --initrd /path/to/initrd --network build.json
```

## nix.conf Configuration

```nix
extra-experimental-features = external-builders
external-builders = [
  {
    "systems": ["aarch64-linux", "x86_64-linux"],
    "program": "/path/to/nix-linux-builder",
    "args": [
      "--kernel", "/path/to/guest-kernel/Image",
      "--initrd", "/path/to/guest-initrd/initrd"
    ]
  }
]
```

## Testing

```bash
# Build a simple Linux package (aarch64)
nix build nixpkgs#legacyPackages.aarch64-linux.hello

# Build a simple Linux package (x86_64, via Rosetta)
nix build nixpkgs#legacyPackages.x86_64-linux.hello
```
