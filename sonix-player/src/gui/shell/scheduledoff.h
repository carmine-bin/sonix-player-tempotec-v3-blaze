#ifndef SCHEDULEDOFF_H
#define SCHEDULEDOFF_H

#include <stdbool.h>
#include <stddef.h>

#include "src/gui/shell/gui.h"

// "Apagado programado": the stock firmware's sleep shutdown. The player
// switches itself off a chosen time from now, playing or not -- falling asleep
// to an album, the device off when the album is long over.
//
// The clock is power.c's (power_scheduled_off_*), which carries it across a
// standby. This is the interface's side: the length, kept in [power]
// scheduled_off_minutes; the control centre's button and the power page's
// switch, which both arm it from now; and the countdown when the time comes --
// the screen lit if it was dark, ten seconds, "Apagar ahora" or "Cancelar".

void scheduledoff_init(gui_config_t *cfg);

// The length chosen, in minutes, and setting it. Setting it while the timer
// runs starts it again from now with the new length.
int scheduledoff_minutes(void);
void scheduledoff_set_minutes(int minutes);

// Armed from now, or called off; a toast says which.
void scheduledoff_set(bool on);
bool scheduledoff_armed(void);

// What is left, for the button: "27 min", and "45 s" in the last minute.
void scheduledoff_left_text(char *out, size_t size);

#endif /* SCHEDULEDOFF_H */
