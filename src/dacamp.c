#include "dacamp.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hbridge.pio.h"
#include "ringbuf.h"

#define PIO         pio0
#define SM_LEFT     0
#define SM_RIGHT    1

#define PIO_TX_FIFO_DEPTH       8
#define PIO_RING_BUFFER_DEPTH   64 //allow buffering of up to 64 pio samples, should be at least PIO_TX_FIFO_DEPTH in size

#define PCM_RING_BUFFER_DEPTH   8192

static volatile bool isEnabledRequested;

static uint32_t pcmRingInternalBuffer[PCM_RING_BUFFER_DEPTH];
static ringbuf_t pcmRing;

auto_init_mutex(pcmMutex);

static void core1_worker(void);
static bool process_sample(uint32_t *outSample);

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

int dacamp_pcm_put(const uint32_t* samples, int sampleCount)
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
    hbridge_program_init(PIO, SM_LEFT, offset, 3);

    bool isEnabledActual;
    bool waitFull;

    uint32_t pioRingInternalBuf[PIO_RING_BUFFER_DEPTH];
    ringbuf_t pioRing;
    ringbuf_init(&pioRing, pioRingInternalBuf, PIO_RING_BUFFER_DEPTH, sizeof(uint32_t));

    uint32_t pioSample;

    while (1) {
        bool isEnabled = isEnabledRequested;

        if (isEnabled != isEnabledActual)
        {
            if (isEnabled) 
            {
                ringbuf_clear(&pioRing);
                hbridge_program_start(PIO, SM_LEFT);
            }
            else 
                hbridge_program_stop(PIO, SM_LEFT);

            isEnabledActual = isEnabled;
        }

        if (!isEnabledActual)
            continue;

        //if fifo is near-empty wait until all 8 slots are ready then fill
        if (pio_sm_get_tx_fifo_level(PIO, SM_LEFT) <= 2) 
            waitFull = true;

        if (!waitFull || ringbuf_filled_slots(&pioRing) >= PIO_TX_FIFO_DEPTH)
        {
            waitFull = false;

            while (!pio_sm_is_tx_fifo_full(PIO, SM_LEFT) && ringbuf_get_one(&pioRing, &pioSample))
                pio_sm_put(PIO, SM_LEFT, pioSample);
        }

        if (!process_sample(&pioSample))
            continue;

        if (!waitFull && !pio_sm_is_tx_fifo_full(PIO, SM_LEFT))
            pio_sm_put(PIO, SM_LEFT, pioSample);
        else
            ringbuf_put_one(&pioRing, &pioSample);
    }
}

static bool process_sample(uint32_t *outSample)
{
    uint32_t pcmStereoSample;

    mutex_enter_blocking(&pcmMutex);

    bool success = ringbuf_get_one(&pcmRing, &pcmStereoSample);

    mutex_exit(&pcmMutex);

    if (!success)
        return false;

    //test: take L, else is zeroed
    *outSample = 0b10101010101010101010101010101010;//pcmStereoSample & 0xFFFF;

    return true;
    //
}