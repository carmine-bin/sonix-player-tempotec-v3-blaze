#ifndef BOOTPROF_H
#define BOOTPROF_H

#include <stdbool.h>

// Where the start goes, block by block and page by page.
//
// main() and gui_init() leave a mark after each thing they do; a mark closes
// the stretch since the one before it and keeps three numbers for it: the time
// on the clock, the time this thread spent on the CPU, and how much the heap
// grew. The two times apart say what kind of slow a page is -- its own work,
// or waiting on the card, the DAC's bus or another thread on the only core.
//
// Marks are always taken: two readings of each clock, some hundred and fifty
// times in a start. Nothing is written anywhere unless the card holds
// <card>/.local/bootprof.txt, and then the report is appended to that same
// file once the first pass of the main loop is done, and printed to the log.
// So measuring a device is: make that file, empty; start the player a few
// times; read the file. Deleting it ends it. The simulator takes
// SONIX_BOOTPROF=<file> in its place.
//
// With the file there, the few synchronous writes the interface thread still
// makes (BOOTPROF_CALL below) are timed too, one line each, for as long as the
// player runs.

// First thing after the log is up: the zero every "at" in the report is
// counted from.
void bootprof_start(void);

// Closes the stretch since the last mark and names it. The name must outlive
// the report: a literal.
void bootprof_mark(const char *name);

// The group the marks that follow are reported under ("main", "gui").
void bootprof_group(const char *group);

// The card is mounted: decides whether this run is being measured.
void bootprof_arm(const char *sd_root);

// The first pass of the main loop has run: closes the last stretch and writes
// the report, if this run is being measured. Once.
extern bool bootprof_reported;
void bootprof_first_pass(void);

// One call, timed where it is made, when this run is being measured.
unsigned long long bootprof_call_begin(void);
void bootprof_call_end(const char *what, unsigned long long begun);
#define BOOTPROF_CALL(what, call)                                                                                    \
	do {                                                                                                             \
		unsigned long long bootprof_t_ = bootprof_call_begin();                                                      \
		call;                                                                                                        \
		bootprof_call_end(what, bootprof_t_);                                                                        \
	} while (0)

#endif /* BOOTPROF_H */
