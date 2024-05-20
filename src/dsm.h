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
#define _DSM_INT_MAX_SHORT_PULSE    ((_DSM_INT_MAX * 7) / 8) //minus dead time (?)

#define _DSM_QUANTIZE(a)                ((a) ? _DSM_INT_MAX : (-_DSM_INT_MAX))
#define _DSM_QUANTIZE_SHORT_PULSE(a)    ((a) ? _DSM_INT_MAX_SHORT_PULSE : (-_DSM_INT_MAX_SHORT_PULSE))

typedef struct dsm
{
    int32_t prevSample;
    int32_t integrator[4];
    uint32_t prevOutputHigh;
    
#ifdef DSM_INTEGRATOR_METRICS //only for local PC simulation
    int32_t integratorMax[4];
    int32_t integratorMin[4];
#endif
} dsm_t;

static void dsm_init(dsm_t* ptr)
{
    ptr->prevSample = 0;
    ptr->prevOutputHigh = 0;
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
    uint32_t outputHigh = 
        (_DSM_A1(ptr->integrator[0]) +
         _DSM_A2(ptr->integrator[1]) +
         _DSM_A3(ptr->integrator[2]) +
         _DSM_A4(ptr->integrator[3]) +
         _DSM_B5(input)) > 0;

    int32_t output = outputHigh == ptr->prevOutputHigh
        ? _DSM_QUANTIZE(outputHigh)
        : _DSM_QUANTIZE_SHORT_PULSE(outputHigh);

    ptr->prevOutputHigh = outputHigh;

    ptr->integrator[0] += _DSM_B1(input) - _DSM_C1(output) - _DSM_G1(ptr->integrator[1]);
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

    return outputHigh;
}

static uint32_t dsm_process_sample(dsm_t* ptr, int32_t dsmPcm)
{
    uint32_t ret = 0;

    //linear interpolation with 1 sample delay
    int32_t sample = ptr->prevSample;
    int32_t step = (dsmPcm - sample) >> 5; // / 32

    ptr->prevSample = dsmPcm;        
    
    ret |= _dsm_calculate(ptr, sample);
    sample += step;

    for (int i = 0; i < 31; ++i)
    {
        ret <<= 1;

        ret |= _dsm_calculate(ptr, sample);
        sample += step;
    }

    return ret;
}
