/*
 * main.h
 *
 */

#ifndef MAIN_MAIN_H_
#define MAIN_MAIN_H_

void app_main();
void read_file_to_shared_buffer();

bool setup_audio_output(i2s_chan_handle_t *tx_handle, uint32_t sample_frequency, i2s_data_bit_width_t bits_sample, i2s_slot_mode_t slot_mode);
bool disable_audio_output(i2s_chan_handle_t *tx_handle);
bool reconfigure_audio_output(i2s_chan_handle_t *tx_handle, uint32_t sample_frequency, i2s_data_bit_width_t bits_sample, i2s_slot_mode_t slot_mode);
bool gpio_setup();

#endif /* MAIN_MAIN_H_ */
