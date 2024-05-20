#pragma once

#include <stdint.h>

void dacamp_init(void);

void dacamp_start(void);

void dacamp_stop(void);

void dacamp_flush(void);

//samples is an array of LR 16 bit or 24 (stored as 32) bit sample pairs
//sampleSize is 4 for PCM16 or 8 for PCM24
//L = sample&0xFFFF, R = sample >> 16 
//L = sample&0xFFFFFFFF, R = sample >> 32 
//returns how many samples were written to the internal buffer
int dacamp_pcm_put(uint32_t* samples, int sampleCount, int sampleSize);