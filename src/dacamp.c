#include "dacamp.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "pico/platform.h"

#include "ringbuf.h"
#include "dsm.h"

//  undefine to process and init only one channel; 
// has to be before the inclusion of "hbridge.pio.h"
#define HBRIDGE_STEREO

#include "hbridge.pio.h"

#define PIO         pio0
#define SM_LEFT     0
#define SM_RIGHT    1

//  to drive the output i wired a simple H-bridge using
// Si2302 N-MOSFETs and Si2305 P-MOSFETs
// total of 2 N-MOSFETS and 1 P-MOSFET per side, 6 total per bridge
// 
// P-MOSFETs are drived through an N-MOSFET and a 100 ohm pullup to get fast enough switch-off, 
// though each pullup heats a lot
// N-MOSFETs have ~10k pulldowns, doesn't matter much, since GPIOs are push-pull
// no gate resistors since we are already severely limited by 12mA per GPIO 
//
//  my mosfet H-bridge wiring is:
// L-  H+ L+  H-, where L is low side and H is high side
// |___|  |___|   + and - are output pins
//   |      |     and MOSFET gates are cross-tied together 
//   B+     B-    for wiring simplicity
//
// so following bridge inputs result in:
// B+ B-
// 1  0: +5v
// 0  1: -5v
// 0  0: not voltage applied / dead time
// 1  1: short everything out and blow up the transistors, don't do that
//
//  the GPIO mapping is 
// note that this pins should be paralleled together to get as much drive current as possible
// so the PIO output is 0b1111_0000, 0b0000_1111 or 0b0000_0000
// Left channel bridge
// bridge  B+           B-
// GPIO    6,7,8,9      10,11,12,13
//
// Right channel bridge (reserved for, but not implemented yet)
// bridge  B+           B-
// GPIO    14,15,16,17  18,19,20,21

#define HBRIDGE_LEFT_START_PIN  6 // PIO takes first pin and assumes other pins are in succession
#define HBRIDGE_RIGHT_START_PIN 14

#define PIO_TX_FIFO_DEPTH 8
#define PIO_RING_BUFFER_DEPTH 32 //allow buffering of up to N processed pio samples, should be at least PIO_TX_FIFO_DEPTH in size

#define PCM_RING_BUFFER_DEPTH 8192

static volatile bool isEnabledRequested;

static uint32_t pcmRingInternalBuffer[PCM_RING_BUFFER_DEPTH] __aligned(4);
static ringbuf_t pcmRing;

static dsm_t dsmLeft, dsmRight;

static uint32_t lastPcm;

#define _DACAMP_PCM_LEFT(pcm) ((int16_t)(pcm))
#define _DACAMP_PCM_RIGHT(pcm) ((int16_t)((pcm) >> 16))

auto_init_mutex(pcmMutex);

static void core1_worker(void);
static bool process_sample(uint32_t *outSampleL, uint32_t *outSampleR, bool allowRepeatPrevious);
static void dacamp_panic();

void dacamp_init(void)
{
    //set SMPS to PWM mode
    gpio_init(23);
    gpio_set_dir(23, GPIO_OUT);
    gpio_put(23, 1);

    //set clock to 192mhz
    set_sys_clock_pll(1536000000, 4, 2);

    ringbuf_init(&pcmRing, &pcmRingInternalBuffer, PCM_RING_BUFFER_DEPTH, sizeof(uint32_t));

    multicore_launch_core1(core1_worker);
}

void dacamp_start(void)
{
    isEnabledRequested = true;
}

void dacamp_stop(void)
{
    isEnabledRequested = false;

    mutex_enter_blocking(&pcmMutex);

    ringbuf_clear(&pcmRing);

    mutex_exit(&pcmMutex);
}

int dacamp_pcm_put(uint32_t* samples, int sampleCount)
{
    if (!isEnabledRequested)
        return sampleCount; //discard

    mutex_enter_blocking(&pcmMutex);

    int ret = ringbuf_put(&pcmRing, samples, sampleCount);

    mutex_exit(&pcmMutex);

    return ret;
}

static void core1_worker(void) 
{
    uint offset = pio_add_program(PIO, &hbridge_program);

    if (!hbridge_program_init(PIO, SM_LEFT, SM_RIGHT, offset, HBRIDGE_LEFT_START_PIN, HBRIDGE_RIGHT_START_PIN))
        dacamp_panic();

    bool isEnabledActual = false;
    bool refillBuffers = false;

    uint64_t pioRingInternalBuf[PIO_RING_BUFFER_DEPTH] __aligned(4);
    ringbuf_t pioRing;

    ringbuf_init(&pioRing, pioRingInternalBuf, PIO_RING_BUFFER_DEPTH, sizeof(uint64_t));

    uint32_t pioSample[2];

    while (1) {
        bool isEnabled = isEnabledRequested;

        if (isEnabled != isEnabledActual)
        {
            if (isEnabled) 
            {
                dsm_reset(&dsmLeft);
                dsm_reset(&dsmRight);
                ringbuf_clear(&pioRing);
                refillBuffers = true;
                lastPcm = 0;

                hbridge_program_start(PIO, SM_LEFT, SM_RIGHT);
            }
            else 
                hbridge_program_stop(PIO, SM_LEFT, SM_RIGHT);

            isEnabledActual = isEnabled;
        }

        if (!isEnabledActual)
            continue;

        if (refillBuffers && ringbuf_is_full(&pioRing))
            refillBuffers = false;

        if (!refillBuffers)
            while (!pio_sm_is_tx_fifo_full(PIO, SM_LEFT) && ringbuf_get_one(&pioRing, pioSample))
            {   
                // assuming we already fill right first and left second, 
                //and they consume bits at the same rate, left will always be 'fuller'
#ifdef HBRIDGE_STEREO
                pio_sm_put(PIO, SM_RIGHT, pioSample[1]);
#endif
                pio_sm_put(PIO, SM_LEFT, pioSample[0]);
            }

        if (ringbuf_is_full(&pioRing))
            continue;

        if (!process_sample(&pioSample[0], &pioSample[1], !ringbuf_is_empty(&pioRing) || refillBuffers))
            continue;

        ringbuf_put_one(&pioRing, pioSample);
    }
}

static inline bool process_sample(uint32_t *outSampleL, uint32_t *outSampleR, bool doNotRepeatPrevious)
{
    mutex_enter_blocking(&pcmMutex);

    bool success = ringbuf_get_one(&pcmRing, &lastPcm);

    mutex_exit(&pcmMutex);

    if (!success && doNotRepeatPrevious)
        return false;

    *outSampleL = dsm_process_sample(&dsmLeft, _DACAMP_PCM_LEFT(lastPcm));
#ifdef HBRIDGE_STEREO
    *outSampleR = dsm_process_sample(&dsmRight, _DACAMP_PCM_RIGHT(lastPcm));
#endif

    return true;
}

static void dacamp_panic()
{
    //enable led
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 1);

    panic("dacamp panic");
}