# RP2040 DAC-Amp

You can do PDM audio output using the PIO, 
but what if you can make a whole USB DAC & 5* watt per channel D-Class amplifier out of your Raspberry Pi Pico and some transistors?..

## Basic concept

In theory RP2040 is capable of acting as a USB audio device (tinyUSB), a PCM-to-PDM converter (with fixed point arithmetic) with 48KHz*32=1.536MHz sampling and a decent H-bridge driver (using PIO) at the same time, 
requiring only 6 MOSFETs per channel with some resistors and a low-pass output filter to make a complete... uhm... music device (no speakers included)

## Limitations

* With 12 MHz main oscillator it is better to avoid 44.1KHz and stick to 48Khz
* Each GPIO is rated for 12mA, however you can parallel 2 or 4 together
* RP2040 has no FPU and even integer division is not very fast, however it is hardware-accelerated
* You still need low gate charge 3.3V or even 1.8V gate voltage transistors

## Current state

![plot](./doc/breadboard.jpg)

* Built on a breadboard with H-bridges soldered on pieces of a stripboard
* With the help from Richard Schreier's delta-sigma toolbox (available in [matlab](https://www.mathworks.com/matlabcentral/fileexchange/19-delta-sigma-toolbox) and [python](https://python-deltasigma.readthedocs.io/))
  DSM was rewritten as a proper 4th order CIFF topology modulator
* Using only int32 add, multiply and shift with power-of-two coefficients (proper division is unacceptably slow),
  both the theoretical (delta-sigma toolbox) and real (building as dll) DSM SNR is at least 75 db in the passband
* However in real hardware at low volume it sounds terrible - rattling, screeching, bubbling - reminds of the sound of incoming GSM call near cheap speakers
* *- due to the nature of higher-order DSMs, to avoid the overload the input has to be limited to ~70% (value is experimental)
  so it is more like +-3.5V, and like with any cheap speakers the advertised power is a bit overstated... for a full amplitude "0%" THD sine wave the estimation is 1.5 watts per channel with 4 ohm load
* Now in stereo!
* Supports 16 and 24 bitdepths at 48 and 96 kHz, 24/96 is the preferred mode to offload some of scaling and oversampling to your host device
* Works with the type-c equipped iPhone 15 Pro LOL
  
## How to 

### Build (firmware)

The project is intended to be built using standard Pi Pico C/C++ SDK cmake scripts. Please refer to the [Pico SDK doc](https://github.com/raspberrypi/pico-sdk/) for more

*(linux/wsl2, for other platforms refer to the SDK doc)*

1. Install CMake (at least version 3.13), and GCC cross compiler
   ```
   sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
   ```
2. Get a copy of Pi Pico SDK with submodules (you need tinyUSB)
   ```
   git clone --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
   ```
4. Set `PICO_SDK_PATH` to the SDK location in your environment, or pass it (`-DPICO_SDK_PATH=`) to cmake later
5. Clone this repo, go to /src/ (where CMakeLists.txt resides) and create a CMake build directory there
   ```
   /src$       mkdir build
   /src$       cd build
   /src/build$ cmake .. <add -DPICO_SDK_PATH= here if you do not use env variable>
   ```
6. Build the project
   ```
   /src/build$ make rp2040_dac_amp
   ```
7. All build artifacts will be placed to /src/build/, including
   ```
   /src/build/rp2040_dac_amp.uf2 <- firmware
   /src/build/rp2040_dac_amp.dis <- disassembly if you are interested
   ```

### Build (hardware)

By default left channel H-bridge is connected to GPIO 6-13, right channel H-bridge is connected to GPIO 14-21. 
Each signal is 2 pins wide to get more current to drive the transistors

The wiring is (please also check the code comments):
```
                 left channel                      right channel
GPIO      6  7    8  9   10  11  12  13     14  15  16  17  18  19  20  21
          |__|    |__|    |__|    |__|       |__|    |__|    |__|    |__|
           |       |       |       |          |       |       |       |
H-bridge   L-      H+      L+      H-         L-      H+      L+      H-
```

Where each H-bridge is (pulldown resistors omitted)
```
            "plus side"                           "minus side"
 +5V from usb __________________________________________
                 |       |                     |       |
                100R     |                     |     100R                "high side"
                 |----P-mosfet             P-mosfet----|   
    H(igh)+ --N-mosfet   |                     |    N-mosfet-- H(igh)-
           +0V __|       |____Load+   Load-____|       |__ +0V
                         |                     |
    L(ow)+ -----------N-mosfet             N-mosfet------------ L(ow)-   "low side"
                         |                     |
                   +0V -------------------------
```

Using an H-bridge we can get +5V, -5V, 0V and "not connected" at load terminals:
```
           L- H+ L+ H-       Load
H-bridge   0  0  0  0        not connected - high impedance
           1  1  0  0        +5V
           0  0  1  1        -5V
           1  0  1  0         0V
           0  1  0  1         0V
          _______________________
           
           *  1  1  *         short circuit, blown transistors
           1  *  *  1         or psu goes into protection mode:
                              if both transitors of left or right side are open
                              you just short +5V to 0V
```

And the last step is connect the speakers to the load terminals of the H-bridge through a low-pass LC filter, 
use whatever values are laying around, aim at ~30kHz cutoff 
```
  H-bridge                Speaker
   Load+ ----L=5.6uH----- Speaker+
                       |
                    c=2.2uF (ceramic/film)
                       |
   Load- ---------------- Speaker-
```
  
## Conclusions

It was interesting to understand how modern digital-to-analog sound conversion is made, but although general concepts are mostly the same, 
even the cheapest modern DAC is much, much more advanced than a software no-FPU-MCU solution ever could be, 
starting from the oversampling which should be at least 64x, certainly more than 1-bit quantization (up to 6 bits according to wiki), enermous gate currents if you want some amplification too etc.

But to my surprise the sound is absolutely tolerable, especially if you crank the volume up above the screeching noise floor, 
and it is cute and smells of rosin when heated!

## What can and can not be improved

* I don't believe it is possible to do 64x oversampling instead of 32x for two channels due to both computational performance and transistor switching time.
  Not without a proper gate driver IC, but going this path it's easier to buy a real amplifier
* Multibit quantizer - 1.5 bits with +1, 0, -1 states - should be possible and in theory should greatly improve low volume performance
* Non-linear interpolation may improve the sound a bit
* Non-breadboard variant can definitely be an improvement
* Also i'm not sure my inductor assembly does its job and whether it affects the sound
* No watchdogs or output protection implemented yet: sometimes tinyUSB just stops sending new data so the output stucks at some DC level

