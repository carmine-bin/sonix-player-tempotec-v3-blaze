#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// Power / display management: mirrors the stock firmware's idle behaviour.
//
//   - Short-pressing the power button toggles the screen off/on. Music keeps
//     playing while the screen is off.
//   - After screen_off_timeout_ms with no user input the screen turns off on
//     its own (even while music is playing).
//   - Suspend-to-RAM. The stock standby executes `mem` 60 s after the screen
//     goes dark, but only after a real, awaited STOP of the audio pipeline
//     (command 0x203, wait for state 2, then `echo mem`). That sequence is
//     replicated here: suspend only when stopped, with no ALSA object alive,
//     Wi-Fi parked and sync() done. Disabled with [power] standby_mem = 0.
//
// The state machine runs on the LVGL/main thread via an lv_timer. Input
// threads (physical buttons) feed it through the thread-safe power_notify_*
// functions; touch input is tracked automatically via LVGL's inactivity clock.
//
// Wi-Fi parking in standby has no switch: a minute after the screen goes dark
// with nothing wanting the radio it powers itself down, and wifi_in_use() in
// power.c keeps it up while something is using it -- Qobuz, Tidal, podcasts,
// radio, AirPlay, DLNA, file transfer. The wait is [power] radio_park_seconds.
//
// Every knob here is driven at runtime from the settings pages, so the config is
// copied into the module and exposed through setters and getters.

typedef struct {
	// sysfs / device paths. A NULL path disables the corresponding action,
	// which is how the host (SDL) build stays safe -- e.g. it must never write
	// "mem" to the developer's own /sys/power/state.
	const char *brightness_path;	 // e.g. /sys/class/backlight/backlight_pwm0/brightness
	const char *max_brightness_path; // e.g. /sys/class/backlight/backlight_pwm0/max_brightness
	const char *blank_path;			 // e.g. /sys/class/graphics/fb0/blank (screen on/off: 0=on, 1=off)
	const char *power_state_path;	 // e.g. /sys/power/state ("mem" = suspend to RAM); NULL = never

	// Desired on-screen backlight level in raw backlight units. If <= 0 at
	// init, the current sysfs value is adopted as the on-level instead.
	long brightness;

	bool screen_off_enabled;
	uint32_t screen_off_timeout_ms;

} power_config_t;

// Initialize the power manager and start its state-machine timer. Copies cfg
// (does not retain the pointer) and remembers disp for inactivity tracking and
// display blanking. Call once, after lv_init() and gui_init().
void power_init(const power_config_t *cfg, lv_display_t *disp);

// Implemented by main.c, because only the display owner knows which
// framebuffer path is in use (the page-flipping display or LVGL's plain fbdev
// driver -- calling the fbdev driver's functions on the page-flipping display
// dereferences driver data it does not have). The blank/unblank cycle tears
// down the panel's scan-out and may reset the video mode, so waking needs a
// display-specific kick: `begin` is called right after the unblank, before
// the wake repaint (it restores the video mode and re-arms scan-out), `end`
// right after the repaint. Both are no-ops on the host build.
void display_wake_begin(lv_display_t *disp);
void display_wake_end(lv_display_t *disp);
// After a frame drawn with the panel blanked: both framebuffer pages get it,
// so the unblank cannot scan out the previous one (see main.c).
void display_mirror_page(void);
// The fast-wake window's black page: shown over the interface's, or taken
// away again; LVGL's pages are left as they were (see main.c).
void display_show_black(bool on);
// A wake's black-page exit: left to the full repaint's own pan where it can be.
void display_black_wake(void);
// Returns once the last pan has reached the panel.
void display_wait_flip(void);

// Screen rotation: turns the picture (and the touch panel with it) through
// 180 degrees. Also implemented by main.c, for the same reason -- only the
// display owner knows how the framebuffer is being fed. LVGL 9.1 has no
// rotation of its own, so this switches rendering to RAM pages that are
// copied into the framebuffer upside down. Unsupported (and a no-op) on the
// plain fbdev fallback and on the host build.
bool display_rotation_supported(void);
bool display_get_rotated(void);
void display_set_rotated(bool rotated);

// A copy of what is on screen right now, in RGB565, for screenshots. Lives in
// main.c for the same reason as the three above: only the framebuffer owner
// knows which of the two pages is the visible one.
//
// Must be called from the GUI thread: the pages are swapped inside the flush,
// and a copy taken from another thread at that moment would read half of one
// frame and half of the other.
//
// NULL on the plain fbdev fallback with no second page, or when the display
// cannot be read back.
//
// The buffer belongs to the module, not the caller, and must not be freed:
// there is one, kept aside and reused for every shot, valid until the next
// call. That is deliberate -- it is 700 kB contiguous, and reallocating it per
// shot is how it returns NULL exactly when memory is tight (Qobuz downloading,
// a decoder open, a cover just decoded). Shots are taken one at a time, so
// reuse is safe.
uint16_t *display_capture_frame(int *out_w, int *out_h);

// True where the interface is drawn at the layout's size and shrunk 2/3 onto
// the panel (the V3 Blaze): one panel pixel is then a pixel and a half of
// what LVGL draws.
bool display_is_scaled(void);

// --- Screen control (must be called on the LVGL/main thread) ---
void power_screen_on(void);
void power_screen_off(void);
void power_toggle_screen(void);
bool power_screen_is_on(void);

// For the main loop, after every pass of lv_timer_handler(). LVGL starts its
// animation timer again whenever an animation is created or ends, so the pause
// put on it at the blank lasts only until the first animation started in the
// dark -- a title that starts scrolling at a track change -- and from there it
// runs every 16 ms until the screen comes back. With the panel off this puts it
// back to sleep; power_screen_on() catches every animation up on the way back.
void power_keep_animations_paused(void);

// For the main loop too, after power_keep_animations_paused(). With the panel
// dark the interface's timers -- the battery and jack polls, the progress
// tick, the bridges, Last.fm, the low-battery check -- each woke the player on
// a phase of its own: a second apart in period, but milliseconds apart in
// phase, so six of them made six wakeups a second where one would do. This
// puts every running timer of a second or longer back on the power tick's
// second (POWER_TICK_STANDBY_MS), moving it earlier by under a second and never
// later, so they all fall due in the pass the tick wakes for. It runs after
// every pass because each callback stamps its own start time, a few
// milliseconds after the one before it, and a timer readied or resumed off the
// beat would otherwise stay off it. Nothing with the screen on.
void power_align_standby_timers(void);

// --- Activity notifications (thread-safe; call from any thread) ---
// Reset the idle timers because the user did something (e.g. a physical
// button press). Touch input is already accounted for automatically.
void power_notify_activity(void);

// Double-tap-to-wake via the touch controller's gesture mode (gesture_sw).
// Where power_double_tap_wake_supported() is false it stays off whatever is
// asked.
void power_set_double_tap_wake(bool enabled);

// False on the R1: cst8xx_touch.ko puts the controller in deep sleep with its
// interrupt disabled when the panel blanks, and has no gesture mode. By the
// model table, or by the gesture node when the model is unknown.
bool power_double_tap_wake_supported(void);

// Whether that option is on. The input threads ask before treating a pair of
// taps on a dark screen as a wake: the touch controller keeps reporting them
// either way, so the decision has to be made here rather than trusted to the
// hardware's gesture mode.
bool power_double_tap_wake_enabled(void);

// Register an lv_timer that only exists to paint. It is paused when the panel
// goes dark and resumed -- and run once immediately -- when it comes back.
// With the screen off nothing is presented, so a painting timer is pure drain.
// Register once, at creation; power.c never unregisters, so the timer must live
// for the life of the program.
void power_pause_in_standby(lv_timer_t *timer);

// The same, for a timer that cannot simply stop. The battery poll is the case
// this exists for: it also drives the status LED, so stopping it in standby
// would leave the LED lit past its twenty seconds and never turning red on the
// charger. Slowed to `standby_period_ms` while the panel is dark, back to its
// own period -- and run once immediately -- at the wake.
void power_slow_in_standby(lv_timer_t *timer, uint32_t standby_period_ms);

// A hook run on the UI thread at both ends of a screen-off: once with the
// panel freshly dark (so whatever it puts up is what the framebuffer holds,
// and the next wake opens on it rather than on the page) and once as the panel
// comes back. The GUI uses it for the screensaver; power.c has no idea what it
// does. It returns true if it put something on screen, which is what tells
// power.c whether the extra repaint is worth doing. NULL clears it.
void power_set_wake_hook(bool (*cb)(void));
// Queue a power-button short-press. Consumed on the next state-machine tick,
// which toggles the screen (or is absorbed as the wake event after suspend).
void power_notify_power_button(void);

// True from just before the device suspends to RAM until a moment after it
// wakes: the headphone remote's keys are to be ignored then (see power.c).
// Safe to call from any thread.
bool power_headset_keys_settling(void);

// --- Runtime configuration (driven by the settings pages) ---
void power_set_brightness(long value); // raw backlight units, clamped to [0, max]
long power_get_brightness(void);	   // current on-level
long power_get_max_brightness(void);   // -1 if unknown

// While held, the idle timer never blanks the screen. Used by anything the
// user is expected to look at and think about before touching again -- the
// date picker being the obvious one.
void power_hold_screen_on(bool hold);

// The same, held by what is on the screen rather than by a job: the lyrics in
// the player, for as long as they are on show. A flag of its own, so that one
// of the two letting go does not release the other.
void power_hold_screen_for_view(bool hold);

// Stops charging once the battery reaches `percent` (80..100; 100 means no
// limit). Whether this can be enforced depends on the charger driver exposing
// a control node -- power_charge_limit_supported() says whether it found one.
void power_set_charge_limit(int percent);
int power_get_charge_limit(void); // 100 when no limit is set
bool power_charge_limit_supported(void);

// True while the charger is being held off -- the battery is at the limit, or
// DAC mode has forbidden charging, or on the R1 the charge has ended at the
// lowered voltage (axpcharge.h). The cable is still in and the player still
// runs off it; nothing is going into the battery. What the status bar and the
// LED follow, so neither claims a charge that is not happening.
bool power_charging_held(void);

// Puts the charger back on and forgets the limit is holding it off. For the
// paths that end the player: the charger driver keeps the bit across a
// shutdown, so a device switched off at its charge limit would come back to a
// cable that does nothing until the next boot resets the chip.
void power_charging_release(void);

// Switches the device off the whole way: the playing position and the clock
// saved, the streaming caches emptied, the card's databases closed and the
// card unmounted, then `poweroff` through init. Does not return. The power
// menu and the automatic shutdown both end here.
void power_shutdown(void);

// The analogue output brought down quietly before the power goes: the track
// stopped, the DAC muted and the route parked on the empty socket, the same
// sequence as before mem. Without it the amplifier loses its supply still
// driving the headphones, which is the pop a forced reboot makes on the stock
// firmware too. Called by power_shutdown() and by the reboot paths.
void power_quiet_audio_for_shutdown(void);

// Forbids or allows charging outright, on top of the percentage limit. Used by
// DAC mode, where the point of not charging is to keep the charger's noise off
// the cable. Stopping the charger takes three writes in a set order -- see
// charger_run() -- and this is the only place that knows them.
void power_set_charging_allowed(bool allowed);

// Powers the device off after `minutes` with no input and nothing playing.
void power_set_auto_off(bool enabled, uint32_t minutes);

// The scheduled shutdown ("Apagado programado", the stock firmware's sleep
// shutdown): the player switches off `minutes` from now, whether music is
// playing or not. Counted on CLOCK_BOOTTIME and carried across a suspend by
// the RTC alarm, so a device asleep when the time comes still goes off. Not
// kept across a restart: a timer is for tonight.
//
// When the time comes the hook runs (the interface's countdown with a way to
// call it off, see scheduledoff.h), with the screen dark too: it lights it.
// With no hook it shuts down at once.
void power_scheduled_off_start(uint32_t minutes);
void power_scheduled_off_cancel(void);
bool power_scheduled_off_armed(void);
uint32_t power_scheduled_off_left_ms(void); // 0 when not armed
void power_set_scheduled_off_hook(void (*cb)(void));

// Closes DAC mode, the Bluetooth receiver or Wi-Fi Transfer after `minutes`
// with no sound or transfer through it and no input; 0 never does.
void power_set_idle_link(uint32_t minutes);

void power_set_screen_off_enabled(bool enabled);
void power_set_screen_off_timeout(uint32_t ms);

// Suspend-to-RAM after a wait with the screen dark (60 s unless set below),
// driven by the switch in the Power settings. No effect on the simulator:
// /sys/power/state does not exist there.
void power_set_standby_enabled(bool enabled);
// How long the screen stays dark before standby, in seconds (Settings > Power,
// [power] standby_mem_seconds). Never less than 15.
void power_set_standby_after(uint32_t seconds);

// Powers the Bluetooth radio and its daemons down, in order, through
// bluetooth_power_down_stack(). The stock init script starts the whole stack at
// every boot -- rfkill on, patchram, bluetoothd, bluealsa -- and with the
// Bluetooth switch off that is pure drain.
// Called once at startup, and only when the config says Bluetooth is off.
void power_bluetooth_off(void);

// Whether this module has switched a radio off for idleness (it switches it
// back on itself): a radio off for that reason is not the user's choice.
bool power_radios_parked(void);

// Snapshot the live configuration (reflects any runtime changes).
void power_get_config(power_config_t *out);

#endif // POWER_H
