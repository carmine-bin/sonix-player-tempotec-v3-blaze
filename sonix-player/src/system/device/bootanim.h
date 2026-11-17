#ifndef BOOTANIM_H
#define BOOTANIM_H

#include <stdint.h>

// TempoTec's boot animation, played over the logo.jpeg the kernel stage left on
// screen. The frames are TempoTec's and are not in the repository: the firmware
// packer copies them out of the stock rootfs into
// /usr/resource/sonix/boot_animation/0.png .. N.png before it deletes the stock
// interface. Frames that are not there are not an error, nothing is drawn.
//
// Drawn straight into the first framebuffer page at the panel's own pixels
// (the frames are 240x60, laid exactly over the mark in logo.jpeg), once and
// before the interface exists, so it is the one thing in startup that waits
// for something other than work. A single pass of 125 frames, about five
// seconds -- or less: the interface is ready well before that, and the
// animation is what the screen shows until it is, not something to sit through
// afterwards. bootanim_stop() ends it at the frame it is on.
//
// fb is the mapped framebuffer, RGB565, panel_w x panel_h with a tight stride.
// Returns the number of frames drawn.
int bootanim_play(uint16_t *fb, int panel_w, int panel_h);

// From another thread: the frame being drawn is the last. bootanim_play()
// returns within one frame's time, 40 ms.
void bootanim_stop(void);

#endif // BOOTANIM_H
