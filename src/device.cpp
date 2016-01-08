#include <libbladeRF.h>
#include "device.h"
#include "options.h"
#include "util.h"
#include <stdlib.h>

struct device_data_struct device_data;

bool open_device(void)
{
    int status;

    // Initialize everything in device_data to zero
    memset(&device_data, 0, sizeof(struct device_data_struct));

    LOG("Opening and initializing device...\n");
    status = bladerf_open(&device_data.dev, opts.devstr);
    if( status != 0 ) {
        ERROR("Failed to open device: %s\n", bladerf_strerror(status));
        goto out;
    }

    status = bladerf_set_frequency(device_data.dev, BLADERF_MODULE_RX, opts.freqs[1]);
    if( status != 0 ) {
        ERROR("Failed to set RX frequency: %s\n", bladerf_strerror(status));
        goto out;
    } else {
        char str[9];
        double2str_suffix(str, opts.freqs[1], freq_suffixes, NUM_FREQ_SUFFIXES);
        INFO("  RX frequency: %sHz\n", str);
    }

    status = bladerf_set_sample_rate(device_data.dev, BLADERF_MODULE_RX, opts.samplerate, NULL);
    if( status != 0 ) {
        ERROR("Failed to set RX sample rate: %s\n", bladerf_strerror(status));
        goto out;
    } else {
        char str[9];
        double2str_suffix(str, opts.samplerate, freq_suffixes, NUM_FREQ_SUFFIXES);
        INFO("  RX samplerate: %ssps\n", str);
    }

    status = bladerf_set_bandwidth(device_data.dev, BLADERF_MODULE_RX, opts.samplerate, NULL);
    if( status != 0 ) {
        ERROR("Failed to set RX bandwidth: %s\n", bladerf_strerror(status));
        goto out;
    } else {
        char str[9];
        double2str_suffix(str, opts.samplerate, freq_suffixes, NUM_FREQ_SUFFIXES);
        INFO("  RX bandwidth: %sHz\n", str);
    }

    status = bladerf_set_lna_gain(device_data.dev, opts.lna);
    if( status != 0 ) {
        ERROR("Failed to set RX LNA gain: %s\n", bladerf_strerror(status));
        goto out;
    } else {
        bool ok;
        int db = bladerf_lna_gain_to_db(opts.lna, &ok);
        INFO("  RX LNA Gain: %ddB\n", db);
    }

    status = bladerf_set_rxvga1(device_data.dev, opts.rxvga1);
    if( status != 0 ) {
        ERROR("Failed to set RX VGA1 gain: %s\n", bladerf_strerror(status));
        goto out;
    } else {
        INFO("  RX VGA1 gain: %ddB\n", opts.rxvga1);
    }

    status = bladerf_set_rxvga2(device_data.dev, opts.rxvga2);
    if( status != 0 ) {
        ERROR("Failed to set RX VGA2 gain: %s\n", bladerf_strerror(status));
        goto out;
    } else {
        INFO("  RX VGA2 gain: %ddB\n", opts.rxvga2);
    }

    status = bladerf_sync_config(device_data.dev, BLADERF_MODULE_RX,
                                 BLADERF_FORMAT_SC16_Q11_META, opts.num_buffers,
                                 opts.buffer_size, opts.num_transfers, opts.timeout_ms);
    if( status != 0 ) {
        ERROR("Failed to sync config: %s\n", bladerf_strerror(status));
        goto out;
    }

    status = bladerf_enable_module(device_data.dev, BLADERF_MODULE_RX, true);
    if( status != 0 ) {
        ERROR("Failed to enable RX module: %s\n", bladerf_strerror(status));
        goto out;
    }

    // Get the current timestamp, and then increment it by 10ms
    status = bladerf_get_timestamp(device_data.dev, BLADERF_MODULE_RX,
                                   &device_data.last_buffer_timestamp);
    if( status != 0 ) {
        ERROR("Failed to get timestamp: %s\n", bladerf_strerror(status));
        goto out;
    }
    device_data.last_buffer_timestamp += (10*opts.samplerate)/1000;

    // Initialize queued data mutex
    pthread_mutex_init(&device_data.queued_mutex, NULL);
out:
    if (status != 0) {
        bladerf_close(device_data.dev);
        return false;
    }
    return true;
}

void close_device(void)
{
    LOG("\nClosing device...");

    /* Disable RX module, shutting down our underlying RX stream */
    int status = bladerf_enable_module(device_data.dev, BLADERF_MODULE_RX, false);
    if (status != 0) {
        ERROR("Failed to disable RX module: %s\n", bladerf_strerror(status));
    }

    // Deinitialize and free resources
    bladerf_close(device_data.dev);
    free(device_data.qtunes);
    LOG(".Done!\n");
}


void schedule_tuning(unsigned short idx, uint64_t timestamp)
{
    int status = bladerf_schedule_retune(device_data.dev, BLADERF_MODULE_RX,
                                         timestamp, 0, &device_data.qtunes[idx]);
    if( status != 0 ) {
        char str[9];
        double2str_suffix(str, opts.freqs[idx], freq_suffixes, NUM_FREQ_SUFFIXES);
        ERROR("bladerf_schedule_retune(dev, rx, %llu, %s, NULL) failed: %s\n",
              timestamp, str, bladerf_strerror(status));
    }
}

// Arbitrary maximum amount of data we will capture at once, limited to 100MB
#define MAX_CAPTURE_BUFF_SIZE (100*1024*1024)
/*
 Receives at least one (and possibly multiple) buffers from the bladeRF, all
 depending on how many integrations are needed combined with how large each fft
 buffer is.
*/
int16_t* receive_buffers(unsigned short freq_idx, unsigned int integration_idx,
                         unsigned int *ret_buffs)
{
    // Our metadata struct to instruct libbladerf on when it should receive data
    int status;
    struct bladerf_metadata meta;
    memset(&meta, 0, sizeof(meta));

    // How many buffers will we capture in one go here?  There is a maximum size
    // we will capture at once, arbitrarily chosen to be 100MB worth of data
    uint32_t buff_size = sizeof(int16_t)*2*opts.fft_len;
    uint32_t max_buffs = MAX(MAX_CAPTURE_BUFF_SIZE/buff_size, 1);
    uint32_t num_buffs = MIN(opts.num_integrations - integration_idx, max_buffs);
    *ret_buffs = num_buffs;

    // Calculate timestamp at which point this data will be ready
    meta.timestamp = device_data.last_buffer_timestamp + num_buffs*opts.fft_len;

    // If this is our last buffer to capture for this frequency, schedule a
    // tuning for when we are done receiving data for the current buffer
    // so that the buffer after the one we are about to receive will be tuned
    // to the next frequency by the time it's ready to be received.
    if( num_buffs == opts.num_integrations - integration_idx ) {
        schedule_tuning(freq_idx, meta.timestamp + 1);
    }

    // Allocate space for our incoming data
    uint32_t data_len = sizeof(int16_t)*2*num_buffs*opts.fft_len;
    int16_t * data = (int16_t *) malloc(data_len);

    uint64_t ts = meta.timestamp;

    // Actually receive the data
    status = bladerf_sync_rx(device_data.dev, data, num_buffs*opts.fft_len,
                             &meta, opts.timeout_ms);

    // Increment last_buffer_timestamp
    device_data.last_buffer_timestamp = meta.timestamp;

    if( status != 0 ) {
        // If our timestamp is off somehow, reset to the bladeRF's clock
        if( status == BLADERF_ERR_TIME_PAST ) {
            bladerf_get_timestamp(device_data.dev, BLADERF_MODULE_RX,
                                  &device_data.last_buffer_timestamp);

            // Also give ourselves some breathing room (1ms) to catch up
            device_data.last_buffer_timestamp += opts.samplerate/1000;
        }

        // Squelch BLADERF_ERR_TIME_PAST errors if we aren't in a verbose mood
        if( status != BLADERF_ERR_TIME_PAST || opts.verbosity >= 1 ) {
            ERROR("bladerf_sync_rx(dev, buffer, %d, meta, %d) failed: %s\n",
                  num_buffs*opts.fft_len, opts.timeout_ms, bladerf_strerror(status));
        }
        free(data);
        return NULL;
    }
    return data;
}

bool calibrate_quicktune(void)
{
    // Allocate qtunes, if it does not already exist
    int status;
    if( device_data.qtunes == NULL ) {
        unsigned int qtune_size = sizeof(bladerf_quick_tune)*opts.num_freqs;
        device_data.qtunes = (bladerf_quick_tune *) malloc(qtune_size);
    }

    LOG("Calibrating quick tune parameters...\n");
    INFO("Frequency table:\n");
    for( int idx = 0; idx < opts.num_freqs; idx++) {
        unsigned int f = opts.freqs[idx];

        // Print out frequencies if we are verbose enough
        char str[9];
        double2str_suffix(str, f, freq_suffixes, NUM_FREQ_SUFFIXES);
        INFO("  [%d] Frequency %sHz\n", idx, str);

        // Set the frequency
        status = bladerf_set_frequency(device_data.dev, BLADERF_MODULE_RX, f);
        if (status != 0) {
            ERROR("Couldn't tune to %sHz: %s\n", str, bladerf_strerror(status));
            return false;
        }

        // Get the quicktune magic
        status = bladerf_get_quick_tune(device_data.dev, BLADERF_MODULE_RX,
                                        &device_data.qtunes[idx]);
        if (status != 0) {
            ERROR("Couldn't get quick tune data for %sHz: %s\n",
                  str, bladerf_strerror(status));
            return false;
        }
    }

    // This whole method is pretty much guaranteed to wreck our timing, so let's
    // go ahead and reset our timestamps now.
    bladerf_get_timestamp(device_data.dev, BLADERF_MODULE_RX,
                          &device_data.last_buffer_timestamp);
    return true;
}
