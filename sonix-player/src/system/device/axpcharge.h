#ifndef AXPCHARGE_H
#define AXPCHARGE_H

#include <stdbool.h>
#include <stdint.h>

// Charging on a player whose AXP2101 PMIC is also its charger: the R1 and the
// Blaze, which have no MP2731. The percentage limit in power.c needs the MP2731's node, so on
// this player there are two switches instead, both written to the PMIC over
// I2C (bus 0, address 0x34):
//
//   * the charge limit: target voltage 4.20 V instead of the stock 4.40 V
//     (REG64h). The charge tapers off and ends on its own at the lower
//     ceiling, around 80%; the charger itself is never switched off.
//   * the current limit: charge current at most 500 mA (REG62h). Never raises
//     a current that is already lower.
//
// Switching either off writes back what was there before. Both are asserted
// again every few seconds from the power tick.

// True on a player charged by the PMIC alone: by the model table when
// system-info.json names one, otherwise by the MP2731's node being absent.
// Decides which controls the Power page shows.
bool axpcharge_applies(void);

// Applies both switches at once. Does nothing where axpcharge_applies() is
// false.
void axpcharge_set(bool limit_voltage, bool limit_current);

// Re-asserts the switches, at most every few seconds. Called from the power
// tick.
void axpcharge_tick(void);

// True on the player whose currents this file also sets by the kind of cable,
// the way its stock player does: the Blaze (sysinfo.h, cable_charge_policy).
// The driver alone keeps the AXP2101's input at 500 mA whatever is plugged in.
// There, at every look at the cable (once a second, from the tick):
//
//   * a wall charger (VBUS up, a gadget bound, and no host on the line for a
//     few seconds): input 2000 mA, charge 1000 mA, stock's numbers;
//   * a USB host (the controller has been enumerated since the plug): 500 and
//     500, stock's numbers too;
//   * no cable, or one not told apart yet: the driver's own defaults, 500 mA
//     in and its 780 mA setting -- what every release before this charged with.
//
// A cable taken for a wall charger that then cannot hold VBUS up at 2000 mA,
// or whose port cuts it, goes back to 500 mA in until it is pulled.
//
// The 500 mA switch caps the charge current of all three. The 80 % cap's stop
// and the stop for DAC mode are a charge current of 0 in the same decision,
// while a cable is in; the cable goes on carrying the player. Nothing is ever asked above stock's
// numbers.
bool axpcharge_cable_policy(void);

// DAC mode with charging switched off: on the Blaze the charger is stopped
// while a cable is in, and the charge indicator is hidden only while that stop
// is really written. Acted on at the next tick. Nothing anywhere else.
void axpcharge_block_charging(bool block);

// True while the voltage cap is on and the PMIC reports the charge as done, or
// while the charge is stopped until the battery is down to the cap: nothing
// more goes into the battery. As of the last tick.
bool axpcharge_holding(void);

// One register of the PMIC, read or written over the same bus. Both players
// have the AXP2101, whichever of them charges. False when it does not answer,
// and always on the host.
bool axpcharge_reg_read(uint8_t reg, uint8_t *value);
bool axpcharge_reg_write(uint8_t reg, uint8_t value);

#endif /* AXPCHARGE_H */
