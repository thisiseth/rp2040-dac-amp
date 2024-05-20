//stupidest ever delta sigma modulator with 32x LINEAR oversampling

#pragma once

#include <memory.h>
#include <stdint.h>

#define _DSM_INT_MAX                (0x7FFF << 8)
#define _DSM_INT_MAX_SHORT_PULSE    ((_DSM_INT_MAX * 7) / 8) //minus dead time
#define _DSM_INT16_TO_INT32(a)      ((((int32_t)(a))*3) << 6) //limit modulator input to 75%

typedef struct dsm
{
    int16_t prevSample;
    int32_t buf[32];
    int32_t integrator[4];
    bool prevOutput;
} dsm_t;

static void dsm_init(dsm_t* ptr)
{
    ptr->prevSample = 0;
    ptr->prevOutput = false;
    memset(ptr->integrator, 0, sizeof(int32_t) * 4);
}

static inline void dsm_reset(dsm_t* ptr)
{
    dsm_init(ptr);
}

static void _dsm_interpolate(int32_t* buf, int16_t firstSample, int16_t secondSample)
{
    //linear interpolation :clown-emoji:
    int32_t sample = _DSM_INT16_TO_INT32(firstSample);
    int32_t step = (_DSM_INT16_TO_INT32(secondSample) - sample) / 32;

    buf[0] = sample;

    for (int i = 1; i < 32; ++i)
        buf[i] = (sample += step);
}

#define _DSM_QUANTIZE(a)                ((a) ? _DSM_INT_MAX : (-_DSM_INT_MAX))
#define _DSM_QUANTIZE_SHORT_PULSE(a)    ((a) ? _DSM_INT_MAX_SHORT_PULSE : (-_DSM_INT_MAX_SHORT_PULSE))

//a = [1, 1/4, 1/16, 1/128];
//b = [1, 0, 0, 0, 1];
//c = [1, 1, 1, 1];
//g = [1/1024, 1/128];

#define _DSM_A1(a) ((a))
#define _DSM_A2(a) ((a) >> 2)
#define _DSM_A3(a) ((a) >> 4)
#define _DSM_A4(a) ((a) >> 8)

#define _DSM_B1(a) ((a))
#define _DSM_B2(a) (0)
#define _DSM_B3(a) (0)
#define _DSM_B4(a) (0)
#define _DSM_B5(a) ((a))

#define _DSM_C1(a) ((a))
#define _DSM_C2(a) ((a))
#define _DSM_C3(a) ((a))
#define _DSM_C4(a) ((a))

#define _DSM_G1(a) ((a) >> 10)
#define _DSM_G2(a) ((a) >> 7)

static inline bool _dsm_calculate(dsm_t* ptr, int32_t input)
{
    int32_t quantizerInput = 
        _DSM_A1(ptr->integrator[0]) +
        _DSM_A2(ptr->integrator[1]) +
        _DSM_A3(ptr->integrator[2]) +
        _DSM_A4(ptr->integrator[3]) +
        _DSM_B5(input);

    //if output changes we lose some precious energy during H-bridge dead-time, otherwise we get full-length pulse.. i think...
    bool outputHigh = quantizerInput > 0;

    int32_t output = outputHigh == ptr->prevOutput
        ? _DSM_QUANTIZE(outputHigh)
        : _DSM_QUANTIZE_SHORT_PULSE(outputHigh);

    ptr->integrator[0] += _DSM_B1(input) - _DSM_C1(output) - _DSM_G1(ptr->integrator[1]);
    ptr->integrator[1] += _DSM_B2(input) + _DSM_C2(ptr->integrator[0]);
    ptr->integrator[2] += _DSM_B3(input) + _DSM_C3(ptr->integrator[1]) - _DSM_G2(ptr->integrator[2]);
    ptr->integrator[3] += _DSM_B4(input) + _DSM_C4(ptr->integrator[2]);

    return ptr->prevOutput = outputHigh;
}

static uint32_t dsm_process_sample(dsm_t* ptr, int16_t pcm)
{
    uint32_t ret = 0;

    _dsm_interpolate(ptr->buf, ptr->prevSample, pcm);

    ptr->prevSample = pcm;

    for (int i = 0; i < 32; ++i)
    {
        ret >>= 1;

        if (_dsm_calculate(ptr, ptr->buf[i]))
            ret |= 0x80000000;
    }

    return ret;
}
