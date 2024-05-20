#pragma once

#include <stdint.h>

void dacamp_init(void);

void dacamp_start(void);

void dacamp_stop(void);

//samples is an array of LR 16 bit sample pairs
//L = sample&0xFFFF, R = sample >> 16
//returns how many samples were written to the internal buffer
int dacamp_pcm_put(uint32_t* samples, int sampleCount);