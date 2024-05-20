//stupidest ever delta sigma modulator with 32x LINEAR oversampling

#pragma once

#include <stdint.h>

typedef struct dsm 
{
    int16_t prevSample;
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

static inline int16_t _dsm_interpolate(int16_t firstSample, int16_t secondSample, int step)
{
    //linear interpolation :clown-emoji:
    return firstSample + (secondSample - firstSample) * step / 32;
}

static uint32_t dsm_process_sample(dsm_t *ptr, int16_t pcm)
{
    uint32_t ret = 0;
    int16_t curSample;

    for (int i = 0; i < 32; ++i)
    {
        curSample = _dsm_interpolate(ptr->prevSample, pcm, i);

        ptr->error += curSample;

        ret >>= 1;

        if (ptr->error > 0)
        {
            ret |= 0x80000000;
            ptr->error -= 32767;
        }
        else
            ptr->error += 32767;
    }

    return ret;
}
