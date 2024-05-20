# RP2040 DAC-Amp

You can do PDM audio output using the PIO, 
but what if you can make a whole USB DAC & 5 watt per channel D-Class amplifier out of your Raspberry Pi Pico and some transistors?..

## Basic concept

In theory RP2040 is capable of acting as a USB audio device (tinyUSB), a PCM-to-PDM converter (with fixed point arithmetic) with 48KHz*32=1.536MHz sampling and a decent H-bridge driver (using PIO) at the same time, 
requiring only 6 MOSFETs per channel with some resistors and a low-pass output filter to make a complete... uhm... music device (no speakers included)

## Limitations

* With 12 MHz main oscillator it is better to avoid 44.1KHz and stick to 48Khz
* Each GPIO is rated for 12mA, however you can parallel 2 or 4 together
* You still need low gate charge 3.3V or even 1.8V gate voltage transistors

## Current state

I put a Pi Pico and a simple 6 MOSFET H-bridge (only one channel) on a breadboard, and it kind of works, 
but first order delta-sigma modulation is not enough, my linear 'oversampling' is pure garbage 
and on top of that breadboard is simply not suitable for frequencies and currents this high at the same time
