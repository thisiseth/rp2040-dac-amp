#include "dacamp.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"

#include "hbridge.pio.h"
#include "ringbuf.h"

#include "dsm.h"

#define PIO         pio0
#define SM_LEFT     0
#define SM_RIGHT    1
                                  //6789 67 & 89 GPIO should be paralleled for more drive current
#define HBRIDGE_LEFT_START_PIN  6 //++--

#define PIO_TX_FIFO_DEPTH       8
#define PIO_RING_BUFFER_DEPTH   32 //allow buffering of up to N processed pio samples, should be at least PIO_TX_FIFO_DEPTH in size

#define PCM_RING_BUFFER_DEPTH   8192

static volatile bool isEnabledRequested;

static uint32_t pcmRingInternalBuffer[PCM_RING_BUFFER_DEPTH];
static ringbuf_t pcmRing;

static dsm_t dsmLeft;
static dsm_t dsmRight;

auto_init_mutex(pcmMutex);

static void core1_worker(void);
static bool process_sample(uint32_t *outSampleL, uint32_t *outSampleR, bool allowRepeatPrevious);

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
    hbridge_program_init(PIO, SM_LEFT, offset, HBRIDGE_LEFT_START_PIN);

    bool isEnabledActual = false;
    bool refillBuffers = false;

    uint32_t pioRingLInternalBuf[PIO_RING_BUFFER_DEPTH], pioRingRInternalBuf[PIO_RING_BUFFER_DEPTH];
    ringbuf_t pioRingL, pioRingR;

    ringbuf_init(&pioRingL, pioRingLInternalBuf, PIO_RING_BUFFER_DEPTH, sizeof(uint32_t));
    ringbuf_init(&pioRingR, pioRingRInternalBuf, PIO_RING_BUFFER_DEPTH, sizeof(uint32_t));

    uint32_t pioSampleL, pioSampleR;

    while (1) {
        bool isEnabled = isEnabledRequested;

        if (isEnabled != isEnabledActual)
        {
            if (isEnabled) 
            {
                dsm_reset(&dsmLeft);
                dsm_reset(&dsmRight);
                ringbuf_clear(&pioRingL);
                ringbuf_clear(&pioRingR);
                refillBuffers = true;

                hbridge_program_start(PIO, SM_LEFT);
            }
            else 
                hbridge_program_stop(PIO, SM_LEFT);

            isEnabledActual = isEnabled;
        }

        if (!isEnabledActual)
            continue;

        if (refillBuffers && ringbuf_is_full(&pioRingL))
            refillBuffers = false;

        if (!refillBuffers)
            while (!pio_sm_is_tx_fifo_full(PIO, SM_LEFT) && ringbuf_get_one(&pioRingL, &pioSampleL))
                pio_sm_put(PIO, SM_LEFT, pioSampleL);

        if (ringbuf_is_full(&pioRingL))
            continue;

        if (!process_sample(&pioSampleL, &pioSampleR, refillBuffers))
            continue;

        ringbuf_put_one(&pioRingL, &pioSampleL);
    }
}

static bool process_sample(uint32_t *outSampleL, uint32_t *outSampleR, bool allowRepeatPrevious)
{
    uint32_t pcmStereoSample;

    mutex_enter_blocking(&pcmMutex);

    bool success = ringbuf_get_one(&pcmRing, &pcmStereoSample);

    mutex_exit(&pcmMutex);

    if (!success)
        return false;

    //test: take L
    //*outSample = 0b10101010101010101010101010101010;
    *outSampleL = dsm_process_sample(&dsmLeft, (int16_t)(pcmStereoSample & 0xFFFF));

// #include <math.h>

//     static int16_t saw = 0;
//     static bool dir = 0;

//     if (saw >= 20000 | saw <= -20000)
//         dir = !dir;

//     saw += dir ? 1000 : -1000;

//     *outSampleL = dsm_process_sample(&dsmLeft, (int16_t)(saw));

    return true;
}