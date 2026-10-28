#!/bin/sh
# nix-linux-builder — tests for guest/nlb-watchdog, run natively on Linux.
# Thresholds are in seconds, so the whole run takes about a minute.
#   tests/guest/watchdog-test.sh /path/to/nlb-watchdog
set -u
wd=${1:?usage: watchdog-test.sh /path/to/nlb-watchdog}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

check() { # name, condition...
    name=$1; shift
    if "$@"; then echo "ok   $name"; else echo "FAIL $name"; fail=1; fi
}

# Output is relayed, the exit code is passed through, and the build sees a tty.
out=$("$wd" -s 0 -f 0 -- sh -c 'echo hello; [ -t 1 ] && echo tty; exit 3')
rc=$?
check "relays output" [ "$(echo "$out" | head -1)" = hello ]
check "builder has a terminal" [ "$(echo "$out" | sed -n 2p)" = tty ]
check "passes exit code" [ "$rc" = 3 ]
check "no carriage returns" [ "$(printf %s "$out" | tr -cd '\r' | wc -c)" = 0 ]

# SIGKILL: a build sandbox may run us with SIGTERM ignored.
"$wd" -s 0 -f 0 -- sh -c 'kill -KILL $$'
check "128+signal when killed" [ "$?" = 137 ]

# A busy loop with no output: snapshots, then a BUSY verdict and exit 124.
out=$("$wd" -s 2 -i 3 -f 9 -d "$tmp/busy" -- sh -c 'while :; do :; done')
rc=$?
check "busy: exit 124" [ "$rc" = 124 ]
check "busy: verdict" sh -c 'echo "$1" | grep -q "verdict: BUSY"' _ "$out"
check "busy: snapshots written" [ -s "$tmp/busy/snapshot-001.txt" ]

# Asleep with no output: IDLE verdict.
out=$("$wd" -s 2 -i 3 -f 9 -d "$tmp/idle" -- sleep 1000)
check "idle: exit 124" [ "$?" = 124 ]
check "idle: verdict" sh -c 'echo "$1" | grep -q "verdict: IDLE"' _ "$out"

# Something that only wakes on SIGCONT: the probe makes output resume, and
# that is recorded.  (A lost futex wakeup is the case this stands in for.)
out=$("$wd" -s 2 -i 3 -c 5 -f 0 -d "$tmp/cont" -- sh -c '
    trap "echo woke; exit 0" CONT
    while :; do sleep 1; done')
rc=$?
check "stopcont: woke and exited 0" [ "$rc" = 0 ]
check "stopcont: woke" sh -c 'echo "$1" | grep -q woke' _ "$out"
check "stopcont: resume noted" sh -c 'grep -q "after SIGSTOP/SIGCONT" "$1"/resumed-after-*.txt' _ "$tmp/cont"

# Output resets the silence timer: a build that prints every 2s never trips a 3s threshold.
out=$("$wd" -s 3 -f 5 -d "$tmp/chatty" -- sh -c 'for i in 1 2 3 4 5; do echo $i; sleep 2; done')
check "chatty: not failed" [ "$?" = 0 ]
check "chatty: no snapshots" [ ! -e "$tmp/chatty" ]

# Open-file sampling: a failed build publishes its samples and puts the peak
# in the log; a successful one leaves nothing behind.
out=$("$wd" -n 1 -d "$tmp/fds" -- sh -c 'sleep 2; exit 1')
check "fds: passes exit code" [ "$?" = 1 ]
check "fds: peak in log" sh -c 'echo "$1" | grep -q "guest open files: peak"' _ "$out"
check "fds: samples published" [ "$(grep -vc '^#' "$tmp/fds/fd-samples.txt" 2>/dev/null)" -ge 2 ]
out=$("$wd" -n 1 -d "$tmp/fds-ok" -- sh -c 'sleep 2')
check "fds: nothing on success" [ ! -e "$tmp/fds-ok" ]
check "fds: no log line on success" [ -z "$out" ]

# Dropping caches every second leaves the build alone: same output, same exit
# code, nothing added to the log.  (Unprivileged here, so the drop itself
# fails quietly; in the VM the watchdog runs as root.)
out=$("$wd" -r 1 -- sh -c 'sleep 3; echo done; exit 4')
rc=$?
check "drop caches: passes exit code" [ "$rc" = 4 ]
check "drop caches: log untouched" [ "$out" = done ]

exit $fail
