#include "dacamp.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "pico/platform.h"
#include "hardware/watchdog.h"

#include "ringbuf.h"
#include "dsm.h"
#include "volumeLut.h"
#include "roscRandom.h"

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

#define PCM_RING_BUFFER_DEPTH 2048

#define PCM_TO_DSM_PCM_BUFFER_LENGTH 256

static volatile bool isEnabledRequested = false, isFlushRequested = false;
static volatile uint32_t requestedSampleRate;

static uint64_t pcmRingInternalBuffer[PCM_RING_BUFFER_DEPTH];
static ringbuf_t pcmRing;

static dsm_t dsmLeft, dsmRight;

static uint64_t lastPcm;

static spin_lock_t *pcmSpinlock;

static uint64_t pcmToDsmPcmBuffer[PCM_TO_DSM_PCM_BUFFER_LENGTH];

#define _DACAMP_PCM16_LEFT(pcm)         ((int16_t)(pcm))
#define _DACAMP_PCM16_RIGHT(pcm)        ((int16_t)((pcm) >> 16))
 
#define _DACAMP_PCM24_LEFT(pcm)         (((int32_t)(pcm)) >> 8)
#define _DACAMP_PCM24_RIGHT(pcm)        ((int32_t)((pcm) >> 32) >> 8)

#define _DACAMP_DSM_PCM_LEFT(pcm)       ((int32_t)(pcm))
#define _DACAMP_DSM_PCM_RIGHT(pcm)      ((int32_t)((pcm) >> 32))
#define _DACAMP_DSM_PCM(left, right)    (((uint64_t)(left & 0xFFFFFFFF)) | (((uint64_t)((right)) << 32)))

static void core1_worker(void);
static bool process_sample(uint64_t *outSampleL, uint64_t *outSampleR, bool doNotRepeatPrevious, bool sampleRate96k);
static void dacamp_panic(void);
static void dacamp_init_cringe_debug(void);

void dacamp_init(void)
{
    //set SMPS to PWM mode
    gpio_init(23);
    gpio_set_dir(23, GPIO_OUT);
    gpio_put(23, 1);

    //set clock to 192mhz
    set_sys_clock_pll(1536000000, 4, 2);

    ringbuf_init(&pcmRing, &pcmRingInternalBuffer, PCM_RING_BUFFER_DEPTH, sizeof(uint64_t));

    pcmSpinlock = spin_lock_init(spin_lock_claim_unused(true));

    dacamp_init_cringe_debug();
    
    multicore_launch_core1(core1_worker);
}

void dacamp_start(uint32_t sampleRate)
{
    requestedSampleRate = sampleRate;
    isEnabledRequested = true;
}

void dacamp_change_sample_rate(uint32_t sampleRate)
{
    requestedSampleRate = sampleRate;
    dacamp_flush();
}

void dacamp_stop(void)
{
    isEnabledRequested = false;
    isFlushRequested = true;

    uint32_t irq = spin_lock_blocking(pcmSpinlock);

    ringbuf_clear(&pcmRing);

    spin_unlock(pcmSpinlock, irq);
}

void dacamp_flush(void)
{
    uint32_t irq = spin_lock_blocking(pcmSpinlock);

    ringbuf_clear(&pcmRing);

    spin_unlock(pcmSpinlock, irq);

    isFlushRequested = true;
}

int dacamp_pcm_put(const uint32_t* samples, int sampleCount, int sampleSize, const int16_t *volume, const int8_t *mute)
{
    if (!isEnabledRequested)
        return sampleCount; //discard

    int ret = 0;

    //4-byte uint32_t samples are 2 channels of 16bit pcm
    //8-byte uint64_t samples are 2 channels of 24bit pcm

    uint64_t *samples64 = (uint64_t*)samples;

    int32_t sampleLeft, sampleRight;

    int32_t volumeLeft = (int32_t)volume[0] + (int32_t)volume[1], 
            volumeRight = (int32_t)volume[0] + (int32_t)volume[2];

    int32_t volumeIndexLeft = (-volumeLeft) >> DACAMP_VOLUME_STEP_BITS,
            volumeIndexRight = (-volumeRight) >> DACAMP_VOLUME_STEP_BITS;

    int32_t muteLeft = mute[0] || mute[1] || volumeLeft <= DACAMP_MIN_VOLUME_UAC2, 
            muteRight = mute[0] || mute[2] || volumeRight <= DACAMP_MIN_VOLUME_UAC2;

    while (sampleCount > 0)
    {
        int samplesToWrite = sampleCount > PCM_TO_DSM_PCM_BUFFER_LENGTH
            ? PCM_TO_DSM_PCM_BUFFER_LENGTH 
            : sampleCount;
        
        for (int i = 0; i < samplesToWrite; ++i)
        {
            if (sampleSize == 4)
            {
                uint32_t sample = *(samples++);
                sampleLeft = DSM_INT16_TO_INT32(_DACAMP_PCM16_LEFT(sample));
                sampleRight = DSM_INT16_TO_INT32(_DACAMP_PCM16_RIGHT(sample));
            }
            else 
            {
                uint64_t sample = *(samples64++);
                sampleLeft = DSM_INT24_TO_INT32(_DACAMP_PCM24_LEFT(sample));
                sampleRight = DSM_INT24_TO_INT32(_DACAMP_PCM24_RIGHT(sample));
            }

            //volume and mute
            if (!muteLeft)
            {
                sampleLeft *= volumeLutNumerator[volumeIndexLeft];
                sampleLeft /= volumeLutDenominator[volumeIndexLeft];
            }
            else
                sampleLeft = 0;

            if (!muteRight)
            {
                sampleRight *= volumeLutNumerator[volumeIndexRight];
                sampleRight /= volumeLutDenominator[volumeIndexRight];
            }
            else
                sampleRight = 0;

            pcmToDsmPcmBuffer[i] = _DACAMP_DSM_PCM(sampleLeft, sampleRight);
        }

        spin_lock_unsafe_blocking(pcmSpinlock);

        int samplesWritten = ringbuf_put(&pcmRing, pcmToDsmPcmBuffer, samplesToWrite);

        spin_unlock_unsafe(pcmSpinlock);

        ret += samplesWritten;

        if (samplesWritten != samplesToWrite)
            break;

        sampleCount -= samplesWritten;
    }

    return ret;
}

static void core1_worker(void) 
{
    uint offset = pio_add_program(PIO, &hbridge_program);

    if (!hbridge_program_init(PIO, SM_LEFT, SM_RIGHT, offset, HBRIDGE_LEFT_START_PIN, HBRIDGE_RIGHT_START_PIN) ||
        !rosc_random_init())
        dacamp_panic();

    bool isEnabled, sampleRate96k;
    bool isEnabledActual = false;
    bool refillBuffers = false;

    uint64_t pioRingInternalBuf[2*PIO_RING_BUFFER_DEPTH];
    ringbuf_t pioRing;

    ringbuf_init(&pioRing, pioRingInternalBuf, PIO_RING_BUFFER_DEPTH, 2*sizeof(uint64_t));

    uint64_t pioSample[2];

    watchdog_enable(500, 1); // 500ms without samples 

    while (1) {
        isEnabled = isEnabledRequested;

        if (isEnabled != isEnabledActual)
        {
            if (isEnabled) 
            {
                dsm_reset(&dsmLeft);
                dsm_reset(&dsmRight);
                ringbuf_clear(&pioRing);
                refillBuffers = true;
                lastPcm = 0;

                sampleRate96k = requestedSampleRate == 96000;

                hbridge_program_start(PIO, offset, SM_LEFT, SM_RIGHT);
            }
            else 
                hbridge_program_stop(PIO, SM_LEFT, SM_RIGHT);

            isEnabledActual = isEnabled;
            isFlushRequested = false;
        }
        else if (isFlushRequested) 
        {
            if (isEnabledActual)
            {
                hbridge_program_stop(PIO, SM_LEFT, SM_RIGHT);

                dsm_reset(&dsmLeft);
                dsm_reset(&dsmRight);
                ringbuf_clear(&pioRing);
                refillBuffers = true;
                lastPcm = 0;

                sampleRate96k = requestedSampleRate == 96000;

                hbridge_program_start(PIO, offset, SM_LEFT, SM_RIGHT);
            }

            isFlushRequested = false;
        }

        if (!isEnabledActual)
        {
            watchdog_update();
            continue;
        }

        if (refillBuffers && ringbuf_is_full(&pioRing))
            refillBuffers = false;

        if (!refillBuffers)
            while (pio_sm_get_tx_fifo_level(PIO, SM_LEFT) <= (PIO_TX_FIFO_DEPTH - 2) && ringbuf_get_one(&pioRing, pioSample))
            {   
                // assuming we already fill right first and left second, 
                //and they consume bits at the same rate, left will always be 'fuller'
#ifdef HBRIDGE_STEREO
                pio_sm_put(PIO, SM_RIGHT, (uint32_t)(pioSample[1] >> 32));
                pio_sm_put(PIO, SM_RIGHT, (uint32_t)pioSample[1]);
#endif
                pio_sm_put(PIO, SM_LEFT, (uint32_t)(pioSample[0] >> 32));
                pio_sm_put(PIO, SM_LEFT, (uint32_t)pioSample[0]);
            }

        if (ringbuf_is_full(&pioRing))
            continue;

        if (!process_sample(&pioSample[0], &pioSample[1], !ringbuf_is_empty(&pioRing) || refillBuffers, sampleRate96k))
            continue;

        ringbuf_put_one(&pioRing, pioSample);
    }
}

static inline bool process_sample(uint64_t *outSampleL, uint64_t *outSampleR, bool doNotRepeatPrevious, bool sampleRate96k)
{
    if (sampleRate96k)
    {
        uint64_t firstPcm;

        uint32_t irq = spin_lock_blocking(pcmSpinlock);

        bool success = ringbuf_filled_slots(&pcmRing) >= 2;

        if (success)
        {
            ringbuf_get_one(&pcmRing, &firstPcm);
            ringbuf_get_one(&pcmRing, &lastPcm);
        }

        spin_unlock(pcmSpinlock, irq);

        if (success)
            watchdog_update();

        if (!success && doNotRepeatPrevious)
            return false;

        if (success)
        {
            *outSampleL = dsm_process_sample_x16(&dsmLeft, _DACAMP_DSM_PCM_LEFT(firstPcm), _DACAMP_DSM_PCM_LEFT(lastPcm), (uint32_t)rosc_random_get());
#ifdef HBRIDGE_STEREO
            *outSampleR = dsm_process_sample_x16(&dsmRight, _DACAMP_DSM_PCM_RIGHT(firstPcm), _DACAMP_DSM_PCM_RIGHT(lastPcm), (uint32_t)rosc_random_get());
#endif
        }
        else 
        {
            *outSampleL = dsm_process_sample_x32(&dsmLeft, _DACAMP_DSM_PCM_LEFT(lastPcm), (uint32_t)rosc_random_get());
#ifdef HBRIDGE_STEREO
            *outSampleR = dsm_process_sample_x32(&dsmRight, _DACAMP_DSM_PCM_RIGHT(lastPcm), (uint32_t)rosc_random_get());
#endif
        }

        return true;        
    }
    else 
    {
        uint32_t irq = spin_lock_blocking(pcmSpinlock);

        bool success = ringbuf_get_one(&pcmRing, &lastPcm);

        spin_unlock(pcmSpinlock, irq);

        if (success)
            watchdog_update();

        if (!success && doNotRepeatPrevious)
            return false;

        *outSampleL = dsm_process_sample_x32(&dsmLeft, _DACAMP_DSM_PCM_LEFT(lastPcm), (uint32_t)rosc_random_get());
#ifdef HBRIDGE_STEREO
        *outSampleR = dsm_process_sample_x32(&dsmRight, _DACAMP_DSM_PCM_RIGHT(lastPcm), (uint32_t)rosc_random_get());
#endif

        return true;
    }
}

static void dacamp_panic(void)
{
    //enable led
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 1);

    panic("dacamp panic");
}

#define CRINGE_DEBUG_LED1 4
#define CRINGE_DEBUG_LED2 5

static void dacamp_init_cringe_debug(void)
{
    gpio_init(CRINGE_DEBUG_LED1);
    gpio_init(CRINGE_DEBUG_LED2);
    gpio_set_dir(CRINGE_DEBUG_LED1, GPIO_OUT);
    gpio_set_dir(CRINGE_DEBUG_LED2, GPIO_OUT);
    gpio_set_drive_strength(CRINGE_DEBUG_LED1, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(CRINGE_DEBUG_LED1, GPIO_DRIVE_STRENGTH_2MA);
}

void dacamp_debug_stuff_task(void)
{
    uint32_t irq = spin_lock_blocking(pcmSpinlock);

    uint32_t level = ringbuf_filled_slots(&pcmRing);

    spin_unlock(pcmSpinlock, irq);

    gpio_put(CRINGE_DEBUG_LED1, level > 30);
    gpio_put(CRINGE_DEBUG_LED2, level == 0);
}