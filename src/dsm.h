//stupidest ever delta sigma modulator with 32x LINEAR oversampling

#pragma once

#include <stdint.h>

#define _DSM_INT_MAX        0x7FFF << 15;
#define _DSM_INT16_TO_INT32(a) ((int32_t)(a) << 15)

typedef struct dsm 
{
    int16_t prevSample;
    int32_t buf[32];
    int32_t error;
} dsm_t;

static void dsm_init(dsm_t *ptr)
{
    ptr->prevSample = 0;
    ptr->error = 0;
}

static inline void dsm_reset(dsm_t *ptr)
{
    dsm_init(ptr);
}

static void _dsm_interpolate(int32_t *buf, int16_t firstSample, int16_t secondSample)
{
    //linear interpolation :clown-emoji:
    int32_t sample = _DSM_INT16_TO_INT32(firstSample);
    int32_t step = (_DSM_INT16_TO_INT32(secondSample) - sample) / 32;

    buf[0] = sample;

    for (int i = 1; i < 32; ++i)
        buf[i] = (sample += step);
}

static uint32_t dsm_process_sample(dsm_t *ptr, int16_t pcm)
{
    uint32_t ret = 0;

    _dsm_interpolate(ptr->buf, ptr->prevSample, pcm);

    ptr->prevSample = pcm;

    for (int i = 0; i < 32; ++i)
    {
        ptr->error += ptr->buf[i];

        ret >>= 1;

        if (ptr->error > 0)
        {
            ret |= 0x80000000;
            ptr->error -= _DSM_INT_MAX;
        }
        else
            ptr->error += _DSM_INT_MAX;
    }

    return ret;
}
