/*
  joystick.h - Analog joystick jog on the thermistor ADC inputs.

  X is P0.24 (AD0.1), Y is P0.25 (AD0.2). Supply the pots from 3.3 V.
  The stick must be released at power-up; that reading becomes the center.
*/

#ifndef joystick_h
#define joystick_h

void joystick_init();
void joystick_poll();

#endif
