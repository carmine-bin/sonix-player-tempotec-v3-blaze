#ifndef PROFILER_H
#define PROFILER_H

#include <stdbool.h>

// A sampling profiler for the device, where there is no perf. Dormant unless
// the card carries .local/profile: its first line is how many seconds to
// sample (default 20). Then, about a hundred times a second of CPU time, the
// program counter and thread of whatever is running are noted, and at the end
// written with the process's maps and thread names to .local/profile.out, and
// the trigger file removed. sonix_player_debug resolves the addresses.
// A second number on the line delays the start by that many seconds, so the
// file can be put on the card before the player boots.
// With no delay and the file already there when the player starts, sampling
// begins at once, which is how the start itself is profiled.
void profiler_start(const char *sd_root);

// Called on the profiler's own thread when sampling starts and when the report
// is written. NULL by default.
void profiler_set_notify(void (*cb)(bool started));

#endif /* PROFILER_H */
