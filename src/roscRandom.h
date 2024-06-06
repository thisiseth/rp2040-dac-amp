#pragma once

#include "pico/stdlib.h"
#include "pico/rand.h"
#include "hardware/dma.h"
#include "hardware/structs/rosc.h"

#define _ROSC_RANDOM_DMA_CHANNEL_CRC 9
#define _ROSC_RANDOM_DMA_CHANNEL_STORE 10
#define _ROSC_RANDOM_DMA_CHANNEL_SIGNAL 11

static volatile int32_t roscRandomOne = 1;
static int roscRandomDiscard;

static volatile int32_t roscRandomOut = 0;

static volatile int roscRandomDataReady = 0;

static bool _rosc_setup(void)
{
    if (!(rosc_hw->status & ROSC_STATUS_ENABLED_BITS))
        return false;

    rosc_hw->ctrl = (ROSC_CTRL_ENABLE_VALUE_ENABLE << ROSC_CTRL_ENABLE_LSB) |
                    (ROSC_CTRL_FREQ_RANGE_VALUE_HIGH << ROSC_CTRL_FREQ_RANGE_LSB);

    rosc_hw->freqb = (ROSC_FREQB_PASSWD_VALUE_PASS << ROSC_FREQB_PASSWD_LSB) |
                     ROSC_FREQB_DS4_BITS |
                     ROSC_FREQB_DS5_BITS |
                     ROSC_FREQB_DS6_BITS |
                     ROSC_FREQB_DS7_BITS;

    rosc_hw->div = ROSC_DIV_VALUE_PASS + 1;

    while (!(rosc_hw->status & ROSC_STATUS_STABLE_BITS))
        tight_loop_contents();

    return true;
}

static bool rosc_random_init(void)
{
    if (!_rosc_setup() ||
        dma_channel_is_claimed(_ROSC_RANDOM_DMA_CHANNEL_CRC) ||
        dma_channel_is_claimed(_ROSC_RANDOM_DMA_CHANNEL_STORE) ||
        dma_channel_is_claimed(_ROSC_RANDOM_DMA_CHANNEL_SIGNAL) ||
        (dma_hw->sniff_ctrl & DMA_SNIFF_CTRL_EN_BITS))
        return false;

    dma_channel_claim(_ROSC_RANDOM_DMA_CHANNEL_CRC);
    dma_channel_claim(_ROSC_RANDOM_DMA_CHANNEL_STORE);
    dma_channel_claim(_ROSC_RANDOM_DMA_CHANNEL_SIGNAL);

    // CRC32 seed
    dma_hw->sniff_data = get_rand_32();

    dma_sniffer_enable(_ROSC_RANDOM_DMA_CHANNEL_CRC, DMA_SNIFF_CTRL_CALC_VALUE_CRC32, true);

    // CRC channel
    dma_channel_config dmaCrcConfig = dma_channel_get_default_config(_ROSC_RANDOM_DMA_CHANNEL_CRC);

    channel_config_set_transfer_data_size(&dmaCrcConfig, DMA_SIZE_32);
    channel_config_set_read_increment(&dmaCrcConfig, false);
    channel_config_set_write_increment(&dmaCrcConfig, false);
    channel_config_set_irq_quiet(&dmaCrcConfig, true);
    channel_config_set_enable(&dmaCrcConfig, true);
    channel_config_set_sniff_enable(&dmaCrcConfig, true);
    channel_config_set_chain_to(&dmaCrcConfig, _ROSC_RANDOM_DMA_CHANNEL_STORE);
    channel_config_set_dreq(&dmaCrcConfig, DREQ_FORCE); // permanent unpaced transfer

    dma_channel_configure(_ROSC_RANDOM_DMA_CHANNEL_CRC,
                          &dmaCrcConfig,
                          &roscRandomDiscard,  // discard
                          &rosc_hw->randombit, // read randombit from ROSC
                          1,                   // one word per transfer
                          false);              // do not start

    // store channel
    dma_channel_config dmaStoreConfig = dma_channel_get_default_config(_ROSC_RANDOM_DMA_CHANNEL_STORE);

    channel_config_set_transfer_data_size(&dmaStoreConfig, DMA_SIZE_32);
    channel_config_set_read_increment(&dmaStoreConfig, false);
    channel_config_set_write_increment(&dmaStoreConfig, false);
    channel_config_set_irq_quiet(&dmaStoreConfig, true);
    channel_config_set_enable(&dmaStoreConfig, true);
    channel_config_set_chain_to(&dmaStoreConfig, _ROSC_RANDOM_DMA_CHANNEL_SIGNAL);
    channel_config_set_dreq(&dmaStoreConfig, DREQ_FORCE); // permanent unpaced transfer

    dma_channel_configure(_ROSC_RANDOM_DMA_CHANNEL_STORE,
                          &dmaStoreConfig,
                          &roscRandomOut,      // write to output
                          &dma_hw->sniff_data, // read from CRC32 sniffer
                          1,                   // one word per transfer
                          false);              // do not start

    // signal channel
    dma_channel_config dmaSignalConfig = dma_channel_get_default_config(_ROSC_RANDOM_DMA_CHANNEL_SIGNAL);

    channel_config_set_transfer_data_size(&dmaSignalConfig, DMA_SIZE_32);
    channel_config_set_read_increment(&dmaSignalConfig, false);
    channel_config_set_write_increment(&dmaSignalConfig, false);
    channel_config_set_irq_quiet(&dmaSignalConfig, true);
    channel_config_set_enable(&dmaSignalConfig, true);
    channel_config_set_chain_to(&dmaSignalConfig, _ROSC_RANDOM_DMA_CHANNEL_CRC);
    channel_config_set_dreq(&dmaSignalConfig, DREQ_FORCE); // permanent unpaced transfer

    dma_channel_configure(_ROSC_RANDOM_DMA_CHANNEL_SIGNAL, 
                          &dmaSignalConfig,
                          &roscRandomDataReady, // signal data ready
                          &roscRandomOne,       // read one
                          1,                    // one word per transfer
                          false);               // do not start

    dma_channel_start(_ROSC_RANDOM_DMA_CHANNEL_CRC); // now start

    return true;
}

static int32_t rosc_random_get()
{
    while (!roscRandomDataReady)
        tight_loop_contents();

    roscRandomDataReady = 0;

    return roscRandomOut;
}