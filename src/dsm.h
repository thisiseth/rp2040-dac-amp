//4th order DSM with CIFF topology and linear interpolation 32x oversampling

#pragma once

#include <memory.h>
#include <stdint.h>

// internally DSM uses 24 bit inputs - +-2^23 * 71% (to prevent overload), 
//if use more than 24 bits current DSM implementation with 32bit integrators starts to overflow
//71% is also a result of modelling and running this code locally with test signals 

#define DSM_INT16_TO_INT32(a)       ((((int32_t)(a)) * 45) << 2) //limit modulator input to 45/64= ~71%
#define DSM_INT24_TO_INT32(a)       ((((int32_t)(a)) * 45) >> 6)

#define _DSM_INT_MAX                (0x7FFF << 8)
#define _DSM_INT_MAX_SHORT_PULSE    ((_DSM_INT_MAX * 21) / 25) //minus dead time (?)
#define _DSM_ZERO_THRESHOLD         ((int32_t)0x00000000) //proper three-state quantizing needs more careful implementation to be useful

#if 1
    #define _DSM_DITHER_MSB             6
    #define _DSM_DITHER_GARBAGE_1(bits) (((int32_t)(bits)) >> (31 - _DSM_DITHER_MSB)) //use top half
    #define _DSM_DITHER_GARBAGE_2(bits) _DSM_DITHER_GARBAGE_1(bits << 16) //use bottom half
#else
    #define _DSM_DITHER_GARBAGE_1(bits) (0)
    #define _DSM_DITHER_GARBAGE_2(bits) (0)
#endif

typedef struct dsm
{
    int32_t prevSample;
    int32_t integrator[4];
    uint32_t prevOutput;

#ifdef DSM_INTEGRATOR_METRICS //only for local PC simulation
    int32_t integratorMax[4];
    int32_t integratorMin[4];
    int32_t quantizerMax;
    int32_t quantizerMin;
#endif
} dsm_t;

static void dsm_init(dsm_t* ptr)
{
    ptr->prevSample = 0;
    ptr->prevOutput = 0xFFFFFFFF;
    memset(ptr->integrator, 0, sizeof(int32_t) * 4);

#ifdef DSM_INTEGRATOR_METRICS
    memset(ptr->integratorMax, 0, sizeof(int32_t) * 4);
    memset(ptr->integratorMin, 0, sizeof(int32_t) * 4);
#endif
}

static inline void dsm_reset(dsm_t* ptr)
{
    dsm_init(ptr);
}

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

//watning: optimizations
static inline uint32_t _dsm_calculate(dsm_t* ptr, int32_t input)
{
    int32_t quantizerInput = _DSM_A1(ptr->integrator[0]) +
        _DSM_A2(ptr->integrator[1]) +
        _DSM_A3(ptr->integrator[2]) +
        _DSM_A4(ptr->integrator[3]) +
        _DSM_B5(input);

#ifdef DSM_INTEGRATOR_METRICS
    if (quantizerInput > ptr->quantizerMax)
        ptr->quantizerMax = quantizerInput;

    if (quantizerInput < ptr->quantizerMin)
        ptr->quantizerMin = quantizerInput;
#endif

    uint32_t dsmOutput;
    int32_t quantizerOutput;

    if (quantizerInput <= -_DSM_ZERO_THRESHOLD)
    {
        dsmOutput = 0b10;
        quantizerOutput = ptr->prevOutput == dsmOutput 
            ? -_DSM_INT_MAX 
            : -_DSM_INT_MAX_SHORT_PULSE;
    }
    else if (quantizerInput > _DSM_ZERO_THRESHOLD)
    {
        dsmOutput = 0b01;
        quantizerOutput = ptr->prevOutput == dsmOutput 
            ? _DSM_INT_MAX 
            : _DSM_INT_MAX_SHORT_PULSE;
    }
    else 
    {
        dsmOutput = 0b00;
        quantizerOutput = 0;
    }

    ptr->prevOutput = dsmOutput;

    ptr->integrator[0] += _DSM_B1(input) - _DSM_C1(quantizerOutput) - _DSM_G1(ptr->integrator[1]);
    ptr->integrator[1] += _DSM_B2(input) + _DSM_C2(ptr->integrator[0]);
    ptr->integrator[2] += _DSM_B3(input) + _DSM_C3(ptr->integrator[1]) - _DSM_G2(ptr->integrator[2]);
    ptr->integrator[3] += _DSM_B4(input) + _DSM_C4(ptr->integrator[2]);

#ifdef DSM_INTEGRATOR_METRICS
    for (int i = 0; i < 4; ++i)
    {
        if (ptr->integrator[i] > ptr->integratorMax[i])
            ptr->integratorMax[i] = ptr->integrator[i];

        if (ptr->integrator[i] < ptr->integratorMin[i])
            ptr->integratorMin[i] = ptr->integrator[i];
    }
#endif

    return dsmOutput;
}

static uint64_t dsm_process_sample_x32(dsm_t* ptr, int32_t dsmPcm, uint32_t randomBits)
{
    uint32_t retLow = 0, retHigh = 0;

    //linear interpolation with 1 sample delay
    int32_t sample = ptr->prevSample + _DSM_DITHER_GARBAGE_1(randomBits);
    int32_t step = (dsmPcm - sample) >> 5; // / 32

    ptr->prevSample = dsmPcm;

    retHigh |= _dsm_calculate(ptr, sample);
    sample += step;

#pragma GCC unroll 16
    for (int i = 0; i < 15; ++i)
    {
        retHigh <<= 2;

        retHigh |= _dsm_calculate(ptr, sample);
        sample += step;
    }

    sample += _DSM_DITHER_GARBAGE_2(randomBits) - _DSM_DITHER_GARBAGE_1(randomBits); //switch garbage

    retLow |= _dsm_calculate(ptr, sample);
    sample += step;

#pragma GCC unroll 16
    for (int i = 0; i < 15; ++i)
    {
        retLow <<= 2;

        retLow |= _dsm_calculate(ptr, sample);
        sample += step;
    }

    return ((uint64_t)retHigh) << 32 | retLow;
}

static uint64_t dsm_process_sample_x16(dsm_t* ptr, int32_t firstDsmPcm, int32_t secondDsmPcm, uint32_t randomBits)
{
    uint32_t retLow = 0, retHigh = 0;

    //linear interpolation with 1 sample delay
    int32_t sample = ptr->prevSample + _DSM_DITHER_GARBAGE_1(randomBits);
    int32_t step = (firstDsmPcm - sample) >> 4; // / 16

    ptr->prevSample = secondDsmPcm;

    retHigh |= _dsm_calculate(ptr, sample);
    sample += step;

#pragma GCC unroll 16
    for (int i = 0; i < 15; ++i)
    {
        retHigh <<= 2;

        retHigh |= _dsm_calculate(ptr, sample);
        sample += step;
    }

    sample = firstDsmPcm + _DSM_DITHER_GARBAGE_2(randomBits);
    step = (secondDsmPcm - sample) >> 4; // / 16

    retLow |= _dsm_calculate(ptr, sample);
    sample += step;

#pragma GCC unroll 16
    for (int i = 0; i < 15; ++i)
    {
        retLow <<= 2;

        retLow |= _dsm_calculate(ptr, sample);
        sample += step;
    }

    return ((uint64_t)retHigh) << 32 | retLow;
}