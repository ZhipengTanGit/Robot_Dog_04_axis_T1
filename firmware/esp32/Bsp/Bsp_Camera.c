#include "Bsp_Camera.h"
#include "CameraCheckLogic.h"
#include "Bsp_BoardBus.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_camera.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if !CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW
#error Camera and XL9555 must both use the new IDF I2C driver
#endif

#define CAMERA_XCLK_HZ 24000000
#define CAMERA_FRAME_MAX_AGE_US 1500000LL
#define CAMERA_RESET_BIT 0x01U
#define CAMERA_PWDN_BIT 0x02U
#define CAMERA_CONTROL_MASK (CAMERA_RESET_BIT | CAMERA_PWDN_BIT)
#define CAMERA_PLL_PRE_DIV 4U
#define CAMERA_PLL_MULTIPLIER 112U
#define CAMERA_PLL_FALLBACK_MULTIPLIER 96U
#define CAMERA_FRAME_WIDTH 640U
#define CAMERA_FRAME_HEIGHT 480U
#define CAMERA_HTS 2500U
#define CAMERA_VTS 1120U
#define CAMERA_TEST_JPEG_QUALITY 35U
#define CAMERA_JPEG_BUFFER_BYTES (512U * 1024U)
/* White-paper calibration for this OV5640 module under the current indoor
 * lighting, 2026-10-26. Runtime service controls can select automatic AWB. */
#define CAMERA_COLOR_DEFAULT_MODE 3U
#define CAMERA_COLOR_DEFAULT_R 1355U
#define CAMERA_COLOR_DEFAULT_G 1024U
#define CAMERA_COLOR_DEFAULT_B 1120U
/* 8-bit clock mode: PLL_CLK=VCO/2, SCLK=PLL_CLK/4.
 * DVP uses its independent root /4 and manual /8 divider. */
#define CAMERA_SCLK_DIVISOR 8U
#define CAMERA_PCLK_DIVISOR 64U
#define CAMERA_PLL_VCO_HZ (CAMERA_XCLK_HZ / CAMERA_PLL_PRE_DIV * CAMERA_PLL_MULTIPLIER)
_Static_assert(CAMERA_PLL_VCO_HZ <= 800000000U, "OV5640 VCO exceeds local datasheet limit");
_Static_assert(CAMERA_PLL_VCO_HZ / CAMERA_PCLK_DIVISOR * 2U < 32000000U, "OV5640 PCLK exceeds S3 RX margin");
_Static_assert(CAMERA_PLL_VCO_HZ / CAMERA_SCLK_DIVISOR == CAMERA_HTS * CAMERA_VTS * 30U,
    "Board clock and frame timing must target 30 fps");

static portMUX_TYPE camera_lock = portMUX_INITIALIZER_UNLOCKED;
static Camera_Status camera_status = {
    .state = "OFF", .xl_error = ESP_ERR_INVALID_STATE,
    .sensor_probe_error = ESP_ERR_INVALID_STATE,
    .id_restart_error = ESP_ERR_INVALID_STATE, .id_stop_error = ESP_ERR_INVALID_STATE,
};
static Camera_CheckStatus check_status = {.state = "IDLE", .stage = "IDLE"};
static bool startup_called;
static SemaphoreHandle_t frame_lock;
/* Only the frame_lock owner may access driver state or leased_frame. */
static bool driver_initialized;
static int64_t clock_ready_us; /* Protected by frame_lock, including rechecks. */
static camera_fb_t *leased_frame;
static uint32_t runtime_capture_failures; /* Protected by frame_lock. */
static bool previous_ready;
static esp_err_t previous_error;
static char previous_state[20];
static char diagnostics[CAMERA_DIAGNOSTICS_CAPACITY];
static size_t diagnostics_used;
static bool diagnostics_capturing;
static const char *const diagnostics_tags[] = {"camera", "ov5640", "sccb-ng", "cam_hal", "s3 ll_cam", "i2c.master"};

void Camera_GetDiagnostics(char *out, size_t capacity)
{
    if (!out || !capacity) return;
    portENTER_CRITICAL(&camera_lock);
    size_t count = diagnostics_used < capacity - 1 ? diagnostics_used : capacity - 1;
    memcpy(out, diagnostics, count); out[count] = 0;
    portEXIT_CRITICAL(&camera_lock);
}
static void append_board_diagnostics(const char *format, ...)
{
    char line[256];
    va_list arguments; va_start(arguments, format);
    int required = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (required <= 0) return;
    size_t length = (size_t)required < sizeof(line) ? (size_t)required : sizeof(line) - 1;
    portENTER_CRITICAL(&camera_lock);
    /* Keep explicit clock/fallback evidence even if SDK errors filled RAM. */
    if (diagnostics_used + length >= sizeof(diagnostics)) diagnostics_used = sizeof(diagnostics) - 1 - length;
    memcpy(diagnostics + diagnostics_used, line, length);
    diagnostics_used += length; diagnostics[diagnostics_used] = 0;
    portEXIT_CRITICAL(&camera_lock);
}
static bool diagnostic_line_selected(const char *line)
{
    /* This project's IDF Log V1 passes the complete formatted line to vprintf.
     * Match the actual tag after its timestamp, including optional ANSI color. */
    const char *level = strstr(line, "E (");
    const char *tag = level ? strstr(level, ") ") : NULL;
    if (!tag) return false;
    tag += 2;
    for (unsigned i = 0; i < sizeof(diagnostics_tags) / sizeof(diagnostics_tags[0]); ++i) {
        size_t length = strlen(diagnostics_tags[i]);
        if (!strncmp(tag, diagnostics_tags[i], length) && tag[length] == ':') return true;
    }
    return false;
}
static int diagnostic_vprintf(const char *format, va_list arguments)
{
    /* Reentrant: formatting uses task-local storage, then only the bounded
     * copy takes camera_lock. Never call ESP_LOG or the old UART callback. */
    char line[384];
    if (!format) return 0;
    int required = vsnprintf(line, sizeof(line), format, arguments);
    if (required <= 0 || !diagnostic_line_selected(line)) return required;
    size_t length = (size_t)required < sizeof(line) ? (size_t)required : sizeof(line) - 1;
    portENTER_CRITICAL(&camera_lock);
    if (diagnostics_capturing) {
        size_t available = sizeof(diagnostics) - 1 - diagnostics_used;
        if (length > available) length = available;
        memcpy(diagnostics + diagnostics_used, line, length);
        diagnostics_used += length; diagnostics[diagnostics_used] = 0;
    }
    portEXIT_CRITICAL(&camera_lock);
    return required;
}

static void append_capture_diagnostics(void)
{
    esp_camera_capture_stats_t stats;
    esp_camera_get_capture_stats(&stats);
    char line[224];
    int required = snprintf(line, sizeof(line),
        "\nCAM_STATS vsync=%lu eof=%lu q_ovf=%lu soi_err=%lu eoi_err=%lu fb_ovf=%lu timeout=%lu\n",
        (unsigned long)stats.vsync_events, (unsigned long)stats.dma_eof_events,
        (unsigned long)stats.event_queue_overflows, (unsigned long)stats.jpeg_soi_errors,
        (unsigned long)stats.jpeg_eoi_errors, (unsigned long)stats.frame_buffer_overflows,
        (unsigned long)stats.frame_timeouts);
    if (required <= 0) return;
    size_t length = (size_t)required < sizeof(line) ? (size_t)required : sizeof(line) - 1;
    portENTER_CRITICAL(&camera_lock);
    size_t available = sizeof(diagnostics) - 1 - diagnostics_used;
    if (length > available) length = available;
    memcpy(diagnostics + diagnostics_used, line, length);
    diagnostics_used += length; diagnostics[diagnostics_used] = 0;
    portEXIT_CRITICAL(&camera_lock);
}

static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static SemaphoreHandle_t frame_mutex(void)
{
    portENTER_CRITICAL(&camera_lock);
    SemaphoreHandle_t result = frame_lock;
    portEXIT_CRITICAL(&camera_lock);
    if (result) return result;
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (!candidate) return NULL;
    portENTER_CRITICAL(&camera_lock);
    if (!frame_lock) { frame_lock = candidate; candidate = NULL; }
    result = frame_lock;
    portEXIT_CRITICAL(&camera_lock);
    if (candidate) vSemaphoreDelete(candidate);
    return result;
}
void Camera_GetStatus(Camera_Status *out)
{
    if (!out) return;
    portENTER_CRITICAL(&camera_lock); *out = camera_status; portEXIT_CRITICAL(&camera_lock);
}
void Camera_GetCheckStatus(Camera_CheckStatus *out)
{
    if (!out) return;
    portENTER_CRITICAL(&camera_lock); *out = check_status; portEXIT_CRITICAL(&camera_lock);
}
static void check_stage(const char *stage)
{
    portENTER_CRITICAL(&camera_lock);
    snprintf(check_status.stage, sizeof(check_status.stage), "%s", stage);
    snprintf(camera_status.state, sizeof(camera_status.state), "%s", stage);
    portEXIT_CRITICAL(&camera_lock);
}
static void check_finish(const char *stage, esp_err_t error)
{
    portENTER_CRITICAL(&camera_lock);
    CameraCheck_Finish(&check_status, stage, error, millis());
    camera_status.ready = check_status.usable;
    camera_status.error = check_status.error;
    snprintf(camera_status.state, sizeof(camera_status.state), "%s", check_status.usable ? "READY" : stage);
    portEXIT_CRITICAL(&camera_lock);
}
static esp_err_t release_camera_reset(void)
{
    /* V0.24: XL9555 P00=active-low RESET, P01=active-high PWDN.
     * Masked read/modify/write/readback preserves LCD/touch/amplifier controls. */
    esp_err_t error = BoardBus_XlUpdate(2, CAMERA_CONTROL_MASK, CAMERA_PWDN_BIT);
    if (error != ESP_OK) return error;
    error = BoardBus_XlUpdate(6, CAMERA_CONTROL_MASK, 0);
    if (error != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(10));
    error = BoardBus_XlUpdate(2, CAMERA_CONTROL_MASK, 0);
    if (error != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(10));
    error = BoardBus_XlUpdate(2, CAMERA_CONTROL_MASK, CAMERA_RESET_BIT);
    if (error == ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    return error;
}
static esp_err_t snapshot_expander(void)
{
    const uint8_t registers[] = {2, 6, 0};
    uint8_t values[3] = {0}, valid = 0;
    esp_err_t error = ESP_OK;
    for (unsigned i = 0; i < 3; ++i) {
        error = BoardBus_XlRead(registers[i], &values[i]);
        if (error != ESP_OK) break;
        valid |= (uint8_t)(1U << i);
    }
    if (error == ESP_OK && ((values[0] & CAMERA_CONTROL_MASK) != CAMERA_RESET_BIT ||
        (values[1] & CAMERA_CONTROL_MASK) != 0 || (values[2] & CAMERA_CONTROL_MASK) != CAMERA_RESET_BIT))
        error = ESP_ERR_INVALID_RESPONSE;
    portENTER_CRITICAL(&camera_lock);
    camera_status.xl_output0 = check_status.xl_output0 = values[0];
    camera_status.xl_config0 = check_status.xl_config0 = values[1];
    camera_status.xl_input0 = check_status.xl_input0 = values[2];
    camera_status.xl_valid = check_status.xl_valid = valid;
    camera_status.xl_error = check_status.expander_error = error;
    check_status.expander_ok = error == ESP_OK;
    portEXIT_CRITICAL(&camera_lock);
    return error;
}
/* Caller holds the BoardBus exclusive lock across both register-pointer and
 * receive operations, including the STOP/START diagnostic variant. */
static esp_err_t read_sensor_id_locked(i2c_master_dev_handle_t device, bool stop_start, uint16_t *pid)
{
    uint8_t values[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t address[] = {0x30, (uint8_t)(0x0a + i)};
        esp_err_t error;
        if (stop_start) {
            error = i2c_master_transmit(device, address, sizeof(address), 80);
            if (error == ESP_OK) error = i2c_master_receive(device, &values[i], 1, 80);
        } else error = i2c_master_transmit_receive(device, address, sizeof(address), &values[i], 1, 80);
        if (error != ESP_OK) return error;
    }
    *pid = ((uint16_t)values[0] << 8) | values[1];
    return ESP_OK;
}
static esp_err_t diagnose_sensor_id(void)
{
    uint16_t restart_id = 0, stop_id = 0;
    esp_err_t restart_error = BoardBus_BeginExclusive(), stop_error = restart_error;
    esp_err_t remove_error = ESP_OK;
    if (restart_error == ESP_OK) {
        i2c_master_dev_handle_t device = NULL;
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = OV5640_SCCB_ADDR, .scl_speed_hz = 100000,
        };
        restart_error = i2c_master_bus_add_device(BoardBus_Handle(), &config, &device);
        stop_error = restart_error;
        if (restart_error == ESP_OK) {
            restart_error = read_sensor_id_locked(device, false, &restart_id);
            stop_error = read_sensor_id_locked(device, true, &stop_id);
            remove_error = i2c_master_bus_rm_device(device);
        }
        BoardBus_EndExclusive();
    }
    bool verified = (restart_error == ESP_OK && restart_id == OV5640_PID) ||
                    (stop_error == ESP_OK && stop_id == OV5640_PID);
    esp_err_t error = verified ? ESP_OK : restart_error != ESP_OK ? restart_error : ESP_ERR_NOT_SUPPORTED;
    if (!verified && stop_error == ESP_OK) error = ESP_ERR_NOT_SUPPORTED;
    if (remove_error != ESP_OK) error = remove_error;
    uint16_t pid = restart_error == ESP_OK ? restart_id : stop_error == ESP_OK ? stop_id : 0;
    if (verified) pid = OV5640_PID;
    portENTER_CRITICAL(&camera_lock);
    camera_status.id_restart_error = restart_error; camera_status.id_stop_error = stop_error;
    camera_status.id_restart = restart_id; camera_status.id_stop = stop_id;
    camera_status.sensor_pid = check_status.sensor_pid = pid;
    camera_status.sda_level = (uint8_t)gpio_get_level(GPIO_NUM_8);
    camera_status.scl_level = (uint8_t)gpio_get_level(GPIO_NUM_19);
    camera_status.diag_complete = true;
    check_status.id_error = error; check_status.id_ok = error == ESP_OK;
    portEXIT_CRITICAL(&camera_lock);
    return error;
}
static esp_err_t cleanup_driver(void)
{
    if (!driver_initialized) return ESP_OK;
    esp_err_t error = BoardBus_BeginExclusive();
    if (error != ESP_OK) return error;
    error = esp_camera_deinit();
    driver_initialized = false;
    BoardBus_EndExclusive();
    return error;
}
static esp_err_t clock_read(i2c_master_dev_handle_t device, uint16_t reg, uint8_t *value)
{
    const uint8_t address[] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    esp_err_t error = i2c_master_transmit_receive(device, address, sizeof(address), value, 1, 80);
    esp_rom_delay_us(25);
    return error;
}
static esp_err_t clock_write(i2c_master_dev_handle_t device, uint16_t reg, uint8_t value)
{
    const uint8_t bytes[] = {(uint8_t)(reg >> 8), (uint8_t)reg, value};
    esp_err_t error = i2c_master_transmit(device, bytes, sizeof(bytes), 80);
    esp_rom_delay_us(25);
    return error;
}
/* Caller holds frame_lock then BoardBus. Use explicit I2C status for readback:
 * the SDK's uint8_t SCCB_Read16 returns zero even when its transfer failed. */
static esp_err_t configure_board_clock(sensor_t *sensor, bool fast)
{
    if (!sensor || sensor->id.PID != OV5640_PID || sensor->xclk_freq_hz != CAMERA_XCLK_HZ ||
        sensor->pixformat != PIXFORMAT_JPEG || sensor->status.framesize != FRAMESIZE_VGA)
        return ESP_ERR_NOT_SUPPORTED;
    uint8_t multiplier = fast ? CAMERA_PLL_MULTIPLIER : CAMERA_PLL_FALLBACK_MULTIPLIER;
    uint32_t vco_hz = CAMERA_XCLK_HZ / CAMERA_PLL_PRE_DIV * multiplier;
    uint32_t sclk_hz = vco_hz / CAMERA_SCLK_DIVISOR;
    uint16_t band_50 = (uint16_t)(sclk_hz / CAMERA_HTS / 100U);
    uint16_t band_60 = (uint16_t)(sclk_hz / CAMERA_HTS / 120U);
    uint16_t exposure_max = CAMERA_VTS - 4U;
    i2c_master_dev_handle_t device = NULL;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = OV5640_SCCB_ADDR, .scl_speed_hz = 50000,
    };
    esp_err_t error = i2c_master_bus_add_device(BoardBus_Handle(), &config, &device);
    uint16_t failed_reg = 0x3008;
    uint8_t original = 0, value = 0, isp_control = 0;
    if (error == ESP_OK) error = clock_read(device, 0x3008, &original);
    if (error == ESP_OK && (original & 0xc0U)) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) { failed_reg = 0x5001; error = clock_read(device, failed_reg, &isp_control); }
    /* Standby prevents a transient high PCLK while set_pll writes sys_div
     * before the multiplier/pre-divider. Restore the exact original byte. */
    if (error == ESP_OK) error = clock_write(device, 0x3008, original | 0x40U);
    if (error == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2));
        failed_reg = 0xffff; /* SDK call: its captured log identifies the register. */
        if (sensor->set_pll(sensor, 0, multiplier, 1, 0, CAMERA_PLL_PRE_DIV, 2, 1, 8) != 0)
            error = ESP_FAIL;
    }
    /* Keep the SDK VGA sensor window, binning and ISP scaling. Only apply
     * the board's verified PLL/totals and 8-bit divider; do not overwrite
     * VGA geometry with the previous native FHD crop. */
    const uint16_t mode_registers[][2] = {
        {0x3034,0x18},
        {0x3808,CAMERA_FRAME_WIDTH >> 8},{0x3809,CAMERA_FRAME_WIDTH & 0xff},
        {0x380a,CAMERA_FRAME_HEIGHT >> 8},{0x380b,CAMERA_FRAME_HEIGHT & 0xff},
        {0x380c,CAMERA_HTS >> 8},{0x380d,CAMERA_HTS & 0xff},
        {0x380e,CAMERA_VTS >> 8},{0x380f,CAMERA_VTS & 0xff},
        {0x5001,isp_control},
        {0x3a02,exposure_max >> 8},{0x3a03,exposure_max & 0xff},
        {0x3a14,exposure_max >> 8},{0x3a15,exposure_max & 0xff},
        {0x3a08,band_50 >> 8},{0x3a09,band_50 & 0xff},
        {0x3a0a,band_60 >> 8},{0x3a0b,band_60 & 0xff},
        {0x3a0e,exposure_max / band_50},{0x3a0d,exposure_max / band_60},
    };
    for (size_t i = 0; error == ESP_OK && i < sizeof(mode_registers)/sizeof(mode_registers[0]); ++i) {
        failed_reg = mode_registers[i][0];
        error = clock_write(device, failed_reg, (uint8_t)mode_registers[i][1]);
    }
    const uint16_t expected[][2] = {
        {0x3039,0x00},{0x3034,0x18},{0x3035,0x11},{0x3036,multiplier},
        {0x3037,0x04},{0x3108,0x26},{0x3824,0x08},{0x460c,0x22},{0x3103,0x13},
    };
    for (size_t i = 0; error == ESP_OK && i < sizeof(expected)/sizeof(expected[0]); ++i) {
        failed_reg = expected[i][0];
        error = clock_read(device, failed_reg, &value);
        if (error == ESP_OK && value != expected[i][1]) error = ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t i = 0; error == ESP_OK && i < sizeof(mode_registers)/sizeof(mode_registers[0]); ++i) {
        failed_reg = mode_registers[i][0];
        error = clock_read(device, failed_reg, &value);
        if (error == ESP_OK && value != mode_registers[i][1]) error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        failed_reg = 0x3008;
        error = clock_write(device, 0x3008, original);
        if (error == ESP_OK) error = clock_read(device, 0x3008, &value);
        if (error == ESP_OK && value != original) error = ESP_ERR_INVALID_RESPONSE;
    }
    if (device) {
        esp_err_t remove_error = i2c_master_bus_rm_device(device);
        if (error == ESP_OK) error = remove_error;
    }
    if (error == ESP_OK) {
        clock_ready_us = esp_timer_get_time();
        append_board_diagnostics("\nCAM_CLOCK mode=VGA_TEST mul=%u pre=4 sys=1 bits=8 VCO=%lu SCLK=%lu PCLK=%lu HTS=%u VTS=%u nominal_mfps=%lu target_fps=30 quality=%u\n",
            multiplier, (unsigned long)vco_hz, (unsigned long)sclk_hz,
            (unsigned long)(vco_hz / CAMERA_PCLK_DIVISOR), CAMERA_HTS, CAMERA_VTS,
            (unsigned long)((uint64_t)sclk_hz * 1000U / CAMERA_HTS / CAMERA_VTS), CAMERA_TEST_JPEG_QUALITY);
    } else append_board_diagnostics("\nCAM_CLOCK_ERROR mul=%u reg=%04x error=%d\n", multiplier, failed_reg, error);
    return error;
}
/* Color access holds frame_lock then BoardBus, as for configure_board_clock.
 * Mode 0 restores the full SDK AWB table; mode 1 uses Digilent's simple AWB
 * reference. Preserve CMX/SDE and the verified FHD scaling state. Reference:
 * cfg_simple_awb_ in
 * github.com/Digilent/Zybo-Z7-20-pcam-5c, sdk/appsrc/pcam_vdma_hdmi/ov5640/OV5640.h. */
static uint8_t color_mode; /* Protected by frame_lock. */
static const uint8_t sdk_awb_values[] = {
    /* Original sensor_default_regs, 0x5180 through 0x519e inclusive. */
    0xff,0xf2,0x00,0x14,0x25,0x24,0x09,0x09,0x09,0x75,0x54,0xe0,0xb2,0x42,0x3d,0x56,
    0x46,0xf8,0x04,0x70,0xf0,0xf0,0x03,0x01,0x04,0x12,0x04,0x00,0x06,0x82,0x38,
};
static esp_err_t color_readback(i2c_master_dev_handle_t device, uint8_t mode)
{
    uint8_t values[9] = {0};
    const uint16_t registers[] = {0x3400,0x3401,0x3402,0x3403,0x3404,0x3405,0x3406,0x5001,0x5183};
    for (size_t i = 0; i < sizeof(registers)/sizeof(registers[0]); ++i) {
        esp_err_t error = clock_read(device, registers[i], &values[i]);
        if (error != ESP_OK) {
            append_board_diagnostics("\nCAM_COLOR_READ_ERROR reg=%04x error=%d\n", registers[i], error);
            return error;
        }
    }
    append_board_diagnostics("\nCAM_COLOR_RGB mode=%u r=%u g=%u b=%u 5001=%02x 5183=%02x 3406=%02x\n",
        (unsigned)mode, (unsigned)(((values[0] & 0x0fU) << 8) | values[1]),
        (unsigned)(((values[2] & 0x0fU) << 8) | values[3]),
        (unsigned)(((values[4] & 0x0fU) << 8) | values[5]),
        (unsigned)values[7], (unsigned)values[8], (unsigned)values[6]);
    return ESP_OK;
}
static esp_err_t configure_board_color(sensor_t *sensor, uint8_t mode, uint16_t r, uint16_t g, uint16_t b)
{
    if (!sensor || sensor->id.PID != OV5640_PID || sensor->pixformat != PIXFORMAT_JPEG)
        return ESP_ERR_NOT_SUPPORTED;
    if (mode > 3 || (mode == 3 && (r < 256 || r > 4095 || g < 256 || g > 4095 || b < 256 || b > 4095)))
        return ESP_ERR_INVALID_ARG;
    i2c_master_dev_handle_t device = NULL;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = OV5640_SCCB_ADDR, .scl_speed_hz = 50000,
    };
    esp_err_t error = i2c_master_bus_add_device(BoardBus_Handle(), &config, &device);
    uint16_t failed_reg = 0x3008;
    uint8_t original = 0, isp_control = 0, value = 0;
    bool restore_needed = false, writes_started = false;
    if (error == ESP_OK) error = clock_read(device, failed_reg, &original);
    if (error == ESP_OK && (original & 0xc0U)) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) { failed_reg = 0x5001; error = clock_read(device, failed_reg, &isp_control); }
    if (error == ESP_OK) {
        failed_reg = 0x3008;
        restore_needed = true;
        error = clock_write(device, failed_reg, original | 0x40U);
        if (error == ESP_OK) error = clock_read(device, failed_reg, &value);
        if (error == ESP_OK && value != (original | 0x40U)) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) vTaskDelay(pdMS_TO_TICKS(2));
    }
    const uint16_t simple_awb[][2] = {
        {0x518d,0x00},{0x518f,0x20},{0x518e,0x00},{0x5190,0x20},
        {0x518b,0x00},{0x518c,0x00},{0x5187,0x10},{0x5188,0x10},
        {0x5189,0x40},{0x518a,0x40},{0x5186,0x10},{0x5181,0x58},
        {0x5184,0x25},{0x5182,0x11},{0x5183,0x80},{0x5191,0xff},{0x5192,0x00},
    };
    uint16_t color_registers[40][2];
    uint8_t previous[40];
    size_t count = 0;
    if (mode <= 1) {
        for (size_t i = 0; i < sizeof(sdk_awb_values); ++i) {
            color_registers[count][0] = 0x5180U + i;
            color_registers[count++][1] = sdk_awb_values[i];
        }
        if (mode == 1) {
            for (size_t i = 0; i < sizeof(simple_awb)/sizeof(simple_awb[0]); ++i)
                color_registers[simple_awb[i][0] - 0x5180U][1] = simple_awb[i][1];
        }
    } else {
        if (mode == 2) { r = 0x520; g = 0x410; b = 0x660; }
        const uint16_t gains[] = {r, g, b};
        for (size_t i = 0; i < 3; ++i) {
            color_registers[count][0] = 0x3400U + 2U * i;
            color_registers[count++][1] = gains[i] >> 8;
            color_registers[count][0] = 0x3401U + 2U * i;
            color_registers[count++][1] = gains[i] & 0xffU;
        }
    }
    color_registers[count][0] = 0x3406; color_registers[count++][1] = mode >= 2 ? 1 : 0;
    color_registers[count][0] = 0x5001; color_registers[count++][1] = isp_control | 0x01U;
    /* Save every affected register before the first write so a failed runtime
     * update can restore the previously working profile while still stopped. */
    for (size_t i = 0; error == ESP_OK && i < count; ++i) {
        failed_reg = color_registers[i][0];
        error = clock_read(device, failed_reg, &previous[i]);
    }
    for (size_t i = 0; error == ESP_OK && i < count; ++i) {
        failed_reg = color_registers[i][0];
        writes_started = true;
        error = clock_write(device, failed_reg, (uint8_t)color_registers[i][1]);
    }
    for (size_t i = 0; error == ESP_OK && i < count; ++i) {
        failed_reg = color_registers[i][0];
        error = clock_read(device, failed_reg, &value);
        if (error == ESP_OK && value != color_registers[i][1]) error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error != ESP_OK && writes_started) {
        esp_err_t rollback_error = ESP_OK;
        for (size_t i = 0; i < count; ++i) {
            esp_err_t item_error = clock_write(device, color_registers[i][0], previous[i]);
            if (item_error == ESP_OK) item_error = clock_read(device, color_registers[i][0], &value);
            if (item_error == ESP_OK && value != previous[i]) item_error = ESP_ERR_INVALID_RESPONSE;
            if (rollback_error == ESP_OK) rollback_error = item_error;
        }
        append_board_diagnostics("\nCAM_COLOR_ROLLBACK error=%d\n", rollback_error);
    }
    /* Restore streaming even after a failed transfer; the caller will reject
     * initialization if any color write/readback failed. */
    if (restore_needed) {
        esp_err_t restore_error = clock_write(device, 0x3008, original);
        if (restore_error == ESP_OK) restore_error = clock_read(device, 0x3008, &value);
        if (restore_error == ESP_OK && value != original) restore_error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK && restore_error != ESP_OK) { failed_reg = 0x3008; error = restore_error; }
    }
    if (error == ESP_OK) {
        sensor->status.awb = 1;
        sensor->status.wb_mode = mode == 2 ? 3 : 0;
        sensor->status.awb_gain = mode >= 2; /* Matches the 0x3406 manual-gain bit. */
        if (mode <= 1) sensor->status.dcw = mode == 0;
        color_mode = mode;
        clock_ready_us = esp_timer_get_time();
        append_board_diagnostics("\nCAM_COLOR mode=%u verified=%u\n", (unsigned)mode, (unsigned)count);
        error = color_readback(device, mode);
    }
    if (device) {
        esp_err_t remove_error = i2c_master_bus_rm_device(device);
        if (error == ESP_OK) error = remove_error;
    }
    if (error != ESP_OK) append_board_diagnostics("\nCAM_COLOR_ERROR reg=%04x error=%d\n", failed_reg, error);
    return error;
}
static esp_err_t color_operation(bool write, uint8_t mode, uint16_t r, uint16_t g, uint16_t b)
{
    SemaphoreHandle_t mutex = frame_mutex();
    if (!mutex) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(200)) != pdTRUE) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&camera_lock);
    bool ready = camera_status.ready && !check_status.running;
    portEXIT_CRITICAL(&camera_lock);
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (!ready || !driver_initialized) goto finished;
    error = BoardBus_BeginExclusive();
    if (error != ESP_OK) goto finished;
    if (write) error = configure_board_color(esp_camera_sensor_get(), mode, r, g, b);
    else {
        i2c_master_dev_handle_t device = NULL;
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = OV5640_SCCB_ADDR, .scl_speed_hz = 50000,
        };
        error = i2c_master_bus_add_device(BoardBus_Handle(), &config, &device);
        if (error == ESP_OK) error = color_readback(device, color_mode);
        if (device) {
            esp_err_t remove_error = i2c_master_bus_rm_device(device);
            if (error == ESP_OK) error = remove_error;
        }
    }
    BoardBus_EndExclusive();
finished:
    xSemaphoreGive(mutex);
    return error;
}
esp_err_t Camera_ReadColor(void) { return color_operation(false, 0, 0, 0, 0); }
esp_err_t Camera_SetColor(uint8_t mode, uint16_t r, uint16_t g, uint16_t b)
{
    if (mode > 3 || (mode == 3 && (r < 256 || r > 4095 || g < 256 || g > 4095 || b < 256 || b > 4095)))
        return ESP_ERR_INVALID_ARG;
    return color_operation(true, mode, r, g, b);
}
static esp_err_t initialize_driver(bool fast, bool *clock_attempted)
{
    *clock_attempted = false; clock_ready_us = 0;
    const camera_config_t config = {
        .pin_pwdn = -1, .pin_reset = -1,
        /* X2 supplies the physical 24 MHz clock. This is a configuration value,
         * not an electrical clock measurement; never drive another GPIO. */
        .pin_xclk = -1, .pin_sccb_sda = -1, .pin_sccb_scl = -1,
        .sccb_i2c_port = I2C_NUM_0,
        .pin_d0 = 41, .pin_d1 = 2, .pin_d2 = 1, .pin_d3 = 42,
        .pin_d4 = 40, .pin_d5 = 38, .pin_d6 = 45, .pin_d7 = 48,
        .pin_vsync = 21, .pin_href = 47, .pin_pclk = 39,
        .xclk_freq_hz = CAMERA_XCLK_HZ, .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG, .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = CAMERA_TEST_JPEG_QUALITY, .fb_count = 2, .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST, .jpeg_buffer_size = CAMERA_JPEG_BUFFER_BYTES,
    };
    esp_err_t error = BoardBus_BeginExclusive();
    if (error != ESP_OK) return error;
    /* Never call init on a live driver: cam_init overwrites its singleton.
     * SDK 2.1.8 clears its own state on probe/config failure. */
    esp_log_level_t saved_levels[sizeof(diagnostics_tags) / sizeof(diagnostics_tags[0])];
    for (unsigned i = 0; i < sizeof(diagnostics_tags) / sizeof(diagnostics_tags[0]); ++i)
        saved_levels[i] = esp_log_level_get(diagnostics_tags[i]);
    portENTER_CRITICAL(&camera_lock); diagnostics_capturing = true; portEXIT_CRITICAL(&camera_lock);
    vprintf_like_t saved_vprintf = esp_log_set_vprintf(diagnostic_vprintf);
    for (unsigned i = 0; i < sizeof(diagnostics_tags) / sizeof(diagnostics_tags[0]); ++i)
        esp_log_level_set(diagnostics_tags[i], ESP_LOG_ERROR);
    error = esp_camera_init(&config);
    if (error == ESP_OK) {
        driver_initialized = true;
        sensor_t *sensor = esp_camera_sensor_get();
        if (!sensor || sensor->id.PID != OV5640_PID) error = ESP_ERR_NOT_SUPPORTED;
        else {
            *clock_attempted = true;
            error = configure_board_clock(sensor, fast);
            if (error == ESP_OK) error = configure_board_color(sensor, CAMERA_COLOR_DEFAULT_MODE,
                CAMERA_COLOR_DEFAULT_R, CAMERA_COLOR_DEFAULT_G, CAMERA_COLOR_DEFAULT_B);
        }
    }
    /* Restore tag filtering before restoring UART output; no temporary enabled
     * tag may emit through the normal callback after this capture window. */
    for (unsigned i = 0; i < sizeof(diagnostics_tags) / sizeof(diagnostics_tags[0]); ++i)
        esp_log_level_set(diagnostics_tags[i], saved_levels[i]);
    (void)esp_log_set_vprintf(saved_vprintf);
    portENTER_CRITICAL(&camera_lock); diagnostics_capturing = false; portEXIT_CRITICAL(&camera_lock);
    BoardBus_EndExclusive();
    return error;
}
static int64_t frame_time_us(const camera_fb_t *frame)
{
    return (int64_t)frame->timestamp.tv_sec * 1000000LL + frame->timestamp.tv_usec;
}
static esp_err_t validate_frame(const camera_fb_t *frame)
{
    if (!frame) return ESP_ERR_TIMEOUT;
    if (frame->format != PIXFORMAT_JPEG || frame->width > UINT16_MAX || frame->height > UINT16_MAX ||
        !CameraCheck_JpegValid(frame->buf, frame->len, (uint16_t)frame->width, (uint16_t)frame->height))
        return ESP_ERR_INVALID_RESPONSE;
    int64_t age = esp_timer_get_time() - frame_time_us(frame);
    return age < 0 || age > CAMERA_FRAME_MAX_AGE_US ? ESP_ERR_TIMEOUT : ESP_OK;
}
static void initialize_camera(void *argument)
{
    (void)argument;
    /* Existing leases retain the mutex until the caller returns the frame.
     * On timeout preserve the live driver; do not reset or free leased memory. */
    if (xSemaphoreTake(frame_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        portENTER_CRITICAL(&camera_lock);
        CameraCheck_Finish(&check_status, "WAIT_FRAME", ESP_ERR_TIMEOUT, millis());
        camera_status.ready = previous_ready; camera_status.error = previous_error;
        memcpy(camera_status.state, previous_state, sizeof(previous_state));
        portEXIT_CRITICAL(&camera_lock);
        vTaskDelete(NULL); return;
    }
    runtime_capture_failures = 0;
    esp_err_t error;
    const char *failure_stage = "CLEANUP";
    check_stage("CLEANUP");
    error = cleanup_driver();
    if (error != ESP_OK) goto finished;
    portENTER_CRITICAL(&camera_lock);
    camera_status.diag_complete = false; camera_status.sensor_pid = 0;
    camera_status.width = camera_status.height = 0;
    camera_status.sensor_probe_error = camera_status.id_restart_error = camera_status.id_stop_error = ESP_ERR_INVALID_STATE;
    camera_status.id_restart = camera_status.id_stop = 0;
    portEXIT_CRITICAL(&camera_lock);
    failure_stage = "I2C_ERROR";
    error = BoardBus_Init();
    if (error != ESP_OK) goto finished;
    check_stage("EXPANDER"); failure_stage = "EXPANDER";
    error = release_camera_reset();
    if (error != ESP_OK) {
        portENTER_CRITICAL(&camera_lock);
        camera_status.xl_error = check_status.expander_error = error;
        portEXIT_CRITICAL(&camera_lock);
        goto finished;
    }
    error = snapshot_expander();
    if (error != ESP_OK) goto finished;
    check_stage("SCCB_PROBE"); failure_stage = "SCCB_PROBE";
    error = BoardBus_Probe(OV5640_SCCB_ADDR);
    portENTER_CRITICAL(&camera_lock);
    camera_status.sensor_probe_error = check_status.sccb_error = error;
    check_status.sccb_ack = error == ESP_OK;
    camera_status.diag_complete = error != ESP_OK;
    camera_status.sda_level = (uint8_t)gpio_get_level(GPIO_NUM_8);
    camera_status.scl_level = (uint8_t)gpio_get_level(GPIO_NUM_19);
    portEXIT_CRITICAL(&camera_lock);
    if (error != ESP_OK) goto finished;
    check_stage("SENSOR_ID"); failure_stage = "SENSOR_ID";
    error = diagnose_sensor_id();
    if (error != ESP_OK) goto finished;
    bool fast = true, clock_attempted = false;
initialize_attempt:
    check_stage("SENSOR_INIT"); failure_stage = "SENSOR_INIT";
    if (!esp_psram_is_initialized()) error = ESP_ERR_NO_MEM;
    else {
        portENTER_CRITICAL(&camera_lock);
        camera_status.psram_bytes = (uint32_t)esp_psram_get_size();
        portEXIT_CRITICAL(&camera_lock);
        error = initialize_driver(fast, &clock_attempted);
    }
    portENTER_CRITICAL(&camera_lock);
    check_status.init_error = error; check_status.init_ok = error == ESP_OK;
    portEXIT_CRITICAL(&camera_lock);
    if (error != ESP_OK) goto clock_fallback;
    check_stage("FRAME"); failure_stage = "FRAME";
    /* SDK bounds fb_get to 4 seconds. No I2C lock held while waiting for pixels. */
    camera_fb_t *frame = NULL;
    unsigned quality = CAMERA_TEST_JPEG_QUALITY;
    for (;;) {
        /* Drop transition buffers after both clock and quality changes. */
        for (unsigned warmup = 0; warmup < 2; ++warmup) {
            frame = esp_camera_fb_get();
            if (!frame) { error = ESP_ERR_TIMEOUT; break; }
            esp_camera_fb_return(frame); frame = NULL;
        }
        if (error != ESP_OK) break;
        frame = esp_camera_fb_get();
        if (frame && frame->len > CAMERA_CHECK_MAX_JPEG_BYTES && quality < 63U) {
            unsigned next_quality = quality + 9U;
            if (next_quality > 63U) next_quality = 63U;
            append_board_diagnostics("\nCAM_JPEG_RETRY bytes=%lu limit=%u quality=%u next_quality=%u\n",
                (unsigned long)frame->len, CAMERA_CHECK_MAX_JPEG_BYTES, quality, next_quality);
            esp_camera_fb_return(frame); frame = NULL;
            error = BoardBus_BeginExclusive();
            if (error == ESP_OK) {
                sensor_t *sensor = esp_camera_sensor_get();
                error = sensor && sensor->set_quality(sensor, (int)next_quality) == 0 ? ESP_OK : ESP_FAIL;
                BoardBus_EndExclusive();
            }
            if (error != ESP_OK) break;
            quality = next_quality;
            continue;
        }
        error = validate_frame(frame);
        if (error == ESP_OK && frame_time_us(frame) < clock_ready_us) error = ESP_ERR_INVALID_RESPONSE;
        if (frame) append_board_diagnostics("\nCAM_JPEG bytes=%lu limit=%u quality=%u error=%d\n",
            (unsigned long)frame->len, CAMERA_CHECK_MAX_JPEG_BYTES, quality, error);
        break;
    }
    portENTER_CRITICAL(&camera_lock);
    check_status.frame_error = error; check_status.frame_ok = error == ESP_OK;
    if (frame) {
        check_status.frame_bytes = (uint32_t)frame->len;
        check_status.width = (uint16_t)frame->width; check_status.height = (uint16_t)frame->height;
        check_status.frame_timestamp_ms = (uint32_t)(frame_time_us(frame) / 1000);
    }
    if (error == ESP_OK) {
        ++camera_status.frames;
        camera_status.width = check_status.width; camera_status.height = check_status.height;
        camera_status.last_frame_ms = check_status.frame_timestamp_ms;
    } else ++camera_status.capture_failures;
    portEXIT_CRITICAL(&camera_lock);
    if (frame) esp_camera_fb_return(frame);
    /* Preserve the first-frame evidence before a failed capture frees driver
     * state. This bounded snapshot does not enable runtime camera logging. */
    append_capture_diagnostics();
clock_fallback:
    if (error != ESP_OK && fast && clock_attempted) {
        append_board_diagnostics("\nCAM_CLOCK_FALLBACK target=30fps failed=%d; reinitialize VGA nominal=25.714fps\n", error);
        esp_err_t cleanup_error = cleanup_driver();
        if (cleanup_error != ESP_OK) { error = cleanup_error; failure_stage = "CLEANUP"; goto finished; }
        fast = false; clock_attempted = false;
        portENTER_CRITICAL(&camera_lock);
        check_status.init_ok = check_status.frame_ok = false;
        check_status.frame_bytes = check_status.frame_timestamp_ms = 0;
        check_status.frame_error = ESP_ERR_INVALID_STATE;
        portEXIT_CRITICAL(&camera_lock);
        goto initialize_attempt;
    }
finished:
    if (error != ESP_OK && driver_initialized && strcmp(failure_stage, "CLEANUP")) {
        esp_err_t cleanup_error = cleanup_driver();
        if (cleanup_error != ESP_OK) { error = cleanup_error; failure_stage = "CLEANUP"; }
    }
    check_finish(error == ESP_OK ? "DONE" : failure_stage, error);
    xSemaphoreGive(frame_lock);
    vTaskDelete(NULL);
}
esp_err_t Camera_RequestCheck(const char *id)
{
    if (!CameraCheck_IdValid(id)) return ESP_ERR_INVALID_ARG;
    if (!frame_mutex()) return ESP_ERR_NO_MEM;
    bool start;
    portENTER_CRITICAL(&camera_lock);
    esp_err_t error = CameraCheck_Begin(&check_status, id, millis(), &start);
    if (error == ESP_OK && start) {
        diagnostics_used = 0; diagnostics[0] = 0;
        startup_called = true;
        previous_ready = camera_status.ready; previous_error = camera_status.error;
        memcpy(previous_state, camera_status.state, sizeof(previous_state));
        camera_status.ready = false;
    }
    portEXIT_CRITICAL(&camera_lock);
    if (error != ESP_OK || !start) return error;
    if (xTaskCreate(initialize_camera, "camera_check", 6144, NULL, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&camera_lock);
        CameraCheck_Finish(&check_status, "TASK_ERROR", ESP_ERR_NO_MEM, millis());
        camera_status.ready = previous_ready; camera_status.error = previous_error;
        memcpy(camera_status.state, previous_state, sizeof(previous_state));
        portEXIT_CRITICAL(&camera_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t Camera_Start(void)
{
    portENTER_CRITICAL(&camera_lock); bool started = startup_called; portEXIT_CRITICAL(&camera_lock);
    return started ? ESP_OK : Camera_RequestCheck("0000000000000000");
}
esp_err_t Camera_AcquireFrame(Camera_Frame *out, uint32_t timeout_ms)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    SemaphoreHandle_t mutex = frame_mutex();
    if (!mutex) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    /* Recheck only AFTER taking the lifecycle/frame mutex: a check may have
     * reset/deinitialized the camera while this caller waited for a frame. */
    portENTER_CRITICAL(&camera_lock);
    bool ready = camera_status.ready && !check_status.running;
    portEXIT_CRITICAL(&camera_lock);
    if (!ready || !driver_initialized) { xSemaphoreGive(mutex); return ESP_ERR_INVALID_STATE; }
    camera_fb_t *frame = esp_camera_fb_get();
    esp_err_t error = validate_frame(frame);
    if (error != ESP_OK) {
        /* Snapshot while this task still owns the buffer; returning it permits
         * DMA to update both the frame timestamp and data immediately. */
        bool has_frame = frame != NULL;
        size_t failed_bytes = frame ? frame->len : 0;
        int64_t age_ms = frame ? (esp_timer_get_time() - frame_time_us(frame)) / 1000 : -1;
        if (frame) esp_camera_fb_return(frame);
        uint32_t failures = ++runtime_capture_failures;
        portENTER_CRITICAL(&camera_lock);
        ++camera_status.capture_failures; camera_status.error = error;
        snprintf(camera_status.state, sizeof(camera_status.state), "CAPTURE_ERROR");
        portEXIT_CRITICAL(&camera_lock);
        if (failures <= 3U || failures % 16U == 0U) {
            /* Keep the latest failure and its counters together in bounded
             * RAM; retain the initialization prefix when the buffer fills.
             * No ESP_LOG call or UART output is enabled by this diagnostic. */
            portENTER_CRITICAL(&camera_lock);
            const size_t reserve = 384U;
            if (diagnostics_used > sizeof(diagnostics) - 1U - reserve)
                diagnostics_used = sizeof(diagnostics) - 1U - reserve;
            diagnostics[diagnostics_used] = 0;
            portEXIT_CRITICAL(&camera_lock);
            append_board_diagnostics("\nCAM_ACQUIRE_FAIL count=%lu has_frame=%u bytes=%lu age_ms=%lld error=%d\n",
                (unsigned long)failures, has_frame ? 1U : 0U, (unsigned long)failed_bytes, (long long)age_ms, error);
            append_capture_diagnostics();
        }
        xSemaphoreGive(mutex); return error;
    }
    leased_frame = frame;
    out->data = frame->buf; out->length = frame->len;
    out->width = frame->width; out->height = frame->height; out->opaque = frame;
    out->timestamp_ms = (uint32_t)(frame_time_us(frame) / 1000);
    portENTER_CRITICAL(&camera_lock);
    out->sequence = ++camera_status.frames; camera_status.last_frame_ms = out->timestamp_ms;
    camera_status.width = (uint16_t)frame->width; camera_status.height = (uint16_t)frame->height;
    camera_status.error = ESP_OK;
    if (!check_status.running) snprintf(camera_status.state, sizeof(camera_status.state), "READY");
    portEXIT_CRITICAL(&camera_lock);
    return ESP_OK;
}
void Camera_ReleaseFrame(Camera_Frame *frame, bool sent_ok)
{
    if (!frame || !frame->opaque || frame->opaque != leased_frame) return;
    /* Contract: release on the same task that acquired the frame mutex. */
    esp_camera_fb_return(leased_frame); leased_frame = NULL;
    memset(frame, 0, sizeof(*frame));
    if (!sent_ok) Camera_RecordSendFailure();
    xSemaphoreGive(frame_lock);
}
void Camera_RecordSendFailure(void)
{
    portENTER_CRITICAL(&camera_lock); ++camera_status.send_failures; portEXIT_CRITICAL(&camera_lock);
}
