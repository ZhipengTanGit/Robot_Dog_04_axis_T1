#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
int64_t esp_timer_get_time(void);
void vTaskDelay(uint32_t ticks);
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *argument, unsigned priority, void *handle);

typedef void *i2c_master_dev_handle_t;
#define BOARD_XL_SPEAKER_SD 0x10U
esp_err_t BoardBus_AddDeviceAtSpeed(uint8_t address, uint32_t speed, i2c_master_dev_handle_t *device);
esp_err_t BoardBus_RemoveDevice(i2c_master_dev_handle_t device);
esp_err_t BoardBus_Probe(uint8_t address);
esp_err_t BoardBus_Read(i2c_master_dev_handle_t device, const uint8_t *reg, size_t reg_length,
                        uint8_t *data, size_t length);
esp_err_t BoardBus_Write(i2c_master_dev_handle_t device, const uint8_t *data, size_t length);
esp_err_t BoardBus_XlConfigureOutput(uint8_t mask, uint8_t value);
esp_err_t BoardBus_XlOutput(uint8_t mask, uint8_t value);

typedef struct FakeI2sChannel { unsigned state, kind; } *i2s_chan_handle_t;
typedef struct { int dma_desc_num, dma_frame_num; bool auto_clear; } i2s_chan_config_t;
typedef struct {
    int clk_cfg, slot_cfg;
    struct { int mclk, bclk, ws, dout, din; } gpio_cfg;
} i2s_std_config_t;
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 0
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_STEREO 2
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) ((i2s_chan_config_t){0})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) (rate)
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, slots) (bits)
esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *config);
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_disable(i2s_chan_handle_t handle);
esp_err_t i2s_del_channel(i2s_chan_handle_t handle);
esp_err_t i2s_channel_read(i2s_chan_handle_t handle, void *data, size_t length, size_t *read, unsigned timeout);
esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *data, size_t length, size_t *written, unsigned timeout);

typedef struct { int speed_mode, timer_num, duty_resolution, freq_hz, clk_cfg; } ledc_timer_config_t;
typedef struct { int gpio_num, speed_mode, channel, intr_type, timer_sel, duty, hpoint; } ledc_channel_config_t;
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_3 3
#define LEDC_TIMER_1_BIT 1
#define LEDC_AUTO_CLK 0
#define LEDC_CHANNEL_7 7
#define LEDC_INTR_DISABLE 0
esp_err_t ledc_timer_config(const ledc_timer_config_t *config);
esp_err_t ledc_channel_config(const ledc_channel_config_t *config);
uint32_t ledc_get_freq(int mode, int timer);
esp_err_t ledc_stop(int mode, int channel, int idle);
esp_err_t ledc_timer_pause(int mode, int timer);
