/*
 * nix-linux-builder — Host open-file sampling
 * Copyright 2025 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group
 * Apache License 2.0
 */

#ifndef NLB_FD_MONITOR_H
#define NLB_FD_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct nlb_fd_monitor nlb_fd_monitor;

/*
 * Start sampling, every interval_secs, how many files this VM's
 * Virtualization.framework process holds open and the host's kern.num_files.
 * Call before the VM starts.  Returns NULL if the sampler could not start
 * (the build runs without it).
 */
nlb_fd_monitor *nlb_fd_monitor_start(uint32_t interval_secs);

/*
 * Stop sampling.  If the build failed, write the samples to
 * <diag_build_dir>/host-fd-samples.txt (when diag_build_dir is set) and
 * print the peaks to stderr.  Frees m; NULL is a no-op.
 */
void nlb_fd_monitor_finish(nlb_fd_monitor *m, const char *diag_build_dir, bool failed);

#endif /* NLB_FD_MONITOR_H */
