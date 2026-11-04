#include "Bsp_BoardBus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "soc/i2c_periph.h"
#include <string.h>

#define IO_TIMEOUT_MS 80
#define BOARD_SDA_GPIO GPIO_NUM_8
#define BOARD_SCL_GPIO GPIO_NUM_19
#define LINE_SAMPLE_COUNT 5U
#define LINE_SAMPLE_INTERVAL_US 25U
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t bus_lock;
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t xl;
static BoardBus_Status status = {
    .last_error = ESP_ERR_INVALID_STATE,
    .line_sample_age_ms = UINT32_MAX, .line_sample_error = ESP_ERR_INVALID_STATE,
    .recovery.state = "NOT_ATTEMPTED",
};
static int64_t line_sample_at_us;

static SemaphoreHandle_t mutex_get(void)
{
    portENTER_CRITICAL(&state_lock);
    SemaphoreHandle_t result = bus_lock;
    portEXIT_CRITICAL(&state_lock);
    if (result) return result;
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (!candidate) return NULL;
    portENTER_CRITICAL(&state_lock);
    if (!bus_lock) { bus_lock = candidate; candidate = NULL; }
    result = bus_lock;
    portEXIT_CRITICAL(&state_lock);
    if (candidate) vSemaphoreDelete(candidate);
    return result;
}

static esp_err_t take(void)
{
    SemaphoreHandle_t lock = mutex_get();
    if (!lock) return ESP_ERR_NO_MEM;
    /* Sensor initialization may hold the shared bus while its vendor SCCB
     * backend performs register programming. Runtime transactions are 80 ms. */
    return xSemaphoreTake(lock, pdMS_TO_TICKS(5000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
static esp_err_t record(esp_err_t error)
{
    /* CST9217 requires a minimum 20 us bus-free interval. */
    esp_rom_delay_us(25);
    portENTER_CRITICAL(&state_lock);
    ++status.transactions;
    status.last_error = error;
    if (error != ESP_OK) ++status.errors;
    portEXIT_CRITICAL(&state_lock);
    return error;
}
esp_err_t BoardBus_BeginExclusive(void) { return take(); }
void BoardBus_EndExclusive(void)
{
    esp_rom_delay_us(25);
    xSemaphoreGive(bus_lock);
}

esp_err_t BoardBus_Init(void)
{
    esp_err_t error = take();
    if (error != ESP_OK) return error;
    if (!bus) {
        const i2c_master_bus_config_t config = {
            .i2c_port = BOARD_I2C_PORT, .sda_io_num = BOARD_SDA_GPIO, .scl_io_num = BOARD_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        error = i2c_new_master_bus(&config, &bus);
    }
    if (error == ESP_OK && !xl) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x20,
            .scl_speed_hz = 100000,
        };
        error = i2c_master_bus_add_device(bus, &config, &xl);
    }
    xSemaphoreGive(bus_lock);
    return error;
}
i2c_master_bus_handle_t BoardBus_Handle(void) { return bus; }
esp_err_t BoardBus_AddDevice(uint8_t address, i2c_master_dev_handle_t *device)
{
    return BoardBus_AddDeviceAtSpeed(address, 100000, device);
}
esp_err_t BoardBus_AddDeviceAtSpeed(uint8_t address, uint32_t scl_speed_hz,
                                  i2c_master_dev_handle_t *device)
{
    if (!device || address > 0x7f || scl_speed_hz == 0 || scl_speed_hz > 400000)
        return ESP_ERR_INVALID_ARG;
    esp_err_t error = BoardBus_Init();
    if (error != ESP_OK) return error;
    if ((error = take()) != ESP_OK) return error;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = address,
        .scl_speed_hz = scl_speed_hz,
    };
    error = i2c_master_bus_add_device(bus, &config, device);
    xSemaphoreGive(bus_lock);
    return error;
}
esp_err_t BoardBus_RemoveDevice(i2c_master_dev_handle_t device)
{
    if (!device) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take();
    if (error != ESP_OK) return error;
    error = i2c_master_bus_rm_device(device);
    xSemaphoreGive(bus_lock);
    return error;
}
esp_err_t BoardBus_Probe(uint8_t address)
{
    if (!bus) return ESP_ERR_INVALID_STATE;
    esp_err_t error = take();
    if (error != ESP_OK) return error;
    error = record(i2c_master_probe(bus, address, IO_TIMEOUT_MS));
    xSemaphoreGive(bus_lock);
    return error;
}
esp_err_t BoardBus_Write(i2c_master_dev_handle_t device, const uint8_t *data, size_t length)
{
    if (!device || !data || !length) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take();
    if (error != ESP_OK) return error;
    error = record(i2c_master_transmit(device, data, length, IO_TIMEOUT_MS));
    xSemaphoreGive(bus_lock);
    return error;
}
esp_err_t BoardBus_Read(i2c_master_dev_handle_t device, const uint8_t *reg, size_t reg_len, uint8_t *data, size_t length)
{
    if (!device || !reg || !reg_len || !data || !length) return ESP_ERR_INVALID_ARG;
    esp_err_t error = take();
    if (error != ESP_OK) return error;
    error = record(i2c_master_transmit_receive(device, reg, reg_len, data, length, IO_TIMEOUT_MS));
    xSemaphoreGive(bus_lock);
    return error;
}
esp_err_t BoardBus_XlRead(uint8_t reg, uint8_t *value)
{
    return BoardBus_Read(xl, &reg, 1, value, 1);
}
esp_err_t BoardBus_XlUpdate(uint8_t reg, uint8_t mask, uint8_t bits)
{
    if (!xl || (reg != 2 && reg != 6)) return ESP_ERR_INVALID_STATE;
    esp_err_t error = take();
    if (error != ESP_OK) return error;
    uint8_t before = 0, check = 0;
    error = record(i2c_master_transmit_receive(xl, &reg, 1, &before, 1, IO_TIMEOUT_MS));
    uint8_t next = (before & (uint8_t)~mask) | (bits & mask);
    if (error == ESP_OK && next != before) {
        uint8_t data[] = { reg, next };
        error = record(i2c_master_transmit(xl, data, sizeof(data), IO_TIMEOUT_MS));
    }
    if (error == ESP_OK) {
        error = record(i2c_master_transmit_receive(xl, &reg, 1, &check, 1, IO_TIMEOUT_MS));
        if (error == ESP_OK && check != next) error = record(ESP_ERR_INVALID_RESPONSE);
    }
    xSemaphoreGive(bus_lock);
    return error;
}
esp_err_t BoardBus_XlOutput(uint8_t mask, uint8_t bits) { return BoardBus_XlUpdate(2, mask, bits); }
esp_err_t BoardBus_XlConfigureOutput(uint8_t mask, uint8_t initial)
{
    /* Other modules only touch disjoint masks; preload before enabling drivers. */
    esp_err_t error = BoardBus_XlOutput(mask, initial);
    return error == ESP_OK ? BoardBus_XlUpdate(6, mask, 0) : error;
}
void BoardBus_GetStatus(BoardBus_Status *out)
{
    if (!out) return;
    int64_t sampled_at;
    portENTER_CRITICAL(&state_lock);
    *out = status; sampled_at = line_sample_at_us;
    portEXIT_CRITICAL(&state_lock);
    int64_t now = esp_timer_get_time();
    uint64_t age_ms = out->line_sample_valid && now >= sampled_at ?
        (uint64_t)(now - sampled_at) / 1000U : UINT64_MAX;
    out->line_sample_age_ms = age_ms < UINT32_MAX ? (uint32_t)age_ms : UINT32_MAX;
}
/* Caller holds bus_lock, excluding all board I2C/SCCB clients. */
static BoardBus_LineSample sample_lines_locked(void)
{
    BoardBus_LineSample sample = {.valid = true, .sda_stable = true, .scl_stable = true};
    esp_rom_delay_us(LINE_SAMPLE_INTERVAL_US);
    sample.sda_level = (uint8_t)gpio_get_level(BOARD_SDA_GPIO);
    sample.scl_level = (uint8_t)gpio_get_level(BOARD_SCL_GPIO);
    for (unsigned i = 1; i < LINE_SAMPLE_COUNT; ++i) {
        esp_rom_delay_us(LINE_SAMPLE_INTERVAL_US);
        uint8_t sda = (uint8_t)gpio_get_level(BOARD_SDA_GPIO);
        uint8_t scl = (uint8_t)gpio_get_level(BOARD_SCL_GPIO);
        if (sda != sample.sda_level) sample.sda_stable = false;
        if (scl != sample.scl_level) sample.scl_stable = false;
        sample.sda_level = sda;
        sample.scl_level = scl;
    }
    return sample;
}

static bool lines_high(const BoardBus_LineSample *sample)
{
    return sample->valid && sample->sda_stable && sample->scl_stable &&
        sample->sda_level == 1 && sample->scl_level == 1;
}

static esp_err_t release_i2c_pins(void)
{
    /* In IDF 5.5.3 set_direction(INPUT) disables output AND peripheral OE
     * before disabling OD. Do this before gpio_config, whose OD step comes
     * first; otherwise a live open-drain output could briefly be push-pull. */
    esp_err_t error = gpio_set_direction(BOARD_SDA_GPIO, GPIO_MODE_INPUT);
    esp_err_t next = gpio_set_direction(BOARD_SCL_GPIO, GPIO_MODE_INPUT);
    if (error == ESP_OK) error = next;
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << BOARD_SDA_GPIO) | (1ULL << BOARD_SCL_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    /* Both pins are fixed valid output-capable GPIOs. Do not change OD or
     * matrix settings after an unexpected direction error. Restore follows. */
    if (error == ESP_OK) error = gpio_config(&config);
    return error;
}

static esp_err_t restore_i2c_pins(void)
{
    /* Preload the released latch while output is disabled. gpio_config sets
     * open drain BEFORE enabling output; gpio_set_direction does the reverse.
     * Never drive a push-pull high against a client holding either line low. */
    esp_err_t error = gpio_set_level(BOARD_SDA_GPIO, 1);
    esp_err_t next = gpio_set_level(BOARD_SCL_GPIO, 1);
    if (error == ESP_OK) error = next;
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << BOARD_SDA_GPIO) | (1ULL << BOARD_SCL_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    next = gpio_config(&config);
    if (error == ESP_OK) error = next;
    if (next == ESP_OK) {
        /* Same public ROM matrix routing used by the IDF I2C master driver.
         * Keep the existing bus/device handles; all clients share these pins. */
        esp_rom_gpio_connect_out_signal(BOARD_SDA_GPIO, i2c_periph_signal[BOARD_I2C_PORT].sda_out_sig, false, false);
        esp_rom_gpio_connect_in_signal(BOARD_SDA_GPIO, i2c_periph_signal[BOARD_I2C_PORT].sda_in_sig, false);
        esp_rom_gpio_connect_out_signal(BOARD_SCL_GPIO, i2c_periph_signal[BOARD_I2C_PORT].scl_out_sig, false, false);
        esp_rom_gpio_connect_in_signal(BOARD_SCL_GPIO, i2c_periph_signal[BOARD_I2C_PORT].scl_in_sig, false);
    }
    return error;
}

esp_err_t BoardBus_Recover(void)
{
    BoardBus_Recovery result = {
        .state = "RUNNING", .attempted = true,
        .attempted_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .result = ESP_ERR_INVALID_STATE,
    };
    portENTER_CRITICAL(&state_lock);
    status.recovery = result;
    portEXIT_CRITICAL(&state_lock);

    result.lock_error = take();
    bool acquired = result.lock_error == ESP_OK;
    if (!acquired) {
        strcpy(result.state, "LOCK_ERROR");
        result.result = result.lock_error;
        goto done;
    }
    if (!bus || !xl) {
        strcpy(result.state, "NOT_READY");
        goto done;
    }

    result.before = sample_lines_locked();
    result.reset_attempted = true;
    result.reset_error = i2c_master_bus_reset(bus);
    esp_rom_delay_us(1000);
    result.after_reset = sample_lines_locked();
    if (!lines_high(&result.after_reset)) {
        result.release_attempted = true;
        result.release_error = release_i2c_pins();
        esp_rom_delay_us(1000);
        result.released = sample_lines_locked();
        result.released.valid = result.release_error == ESP_OK;
        /* Restore even if the release configuration failed partway through. */
        result.restore_attempted = true;
        result.restore_error = restore_i2c_pins();
        esp_rom_delay_us(1000);
        result.after_restore = sample_lines_locked();
        if (result.restore_error != ESP_OK) {
            strcpy(result.state, "RESTORE_ERROR");
            result.result = result.restore_error;
            goto done;
        }
        if (result.release_error != ESP_OK) {
            strcpy(result.state, "RELEASE_ERROR");
            result.result = result.release_error;
            goto done;
        }
        if (!lines_high(&result.after_restore)) {
            if (result.released.scl_stable && result.released.scl_level == 0 &&
                result.released.sda_stable && result.released.sda_level == 0)
                strcpy(result.state, "LINES_HELD_LOW");
            else if (result.released.scl_stable && result.released.scl_level == 0)
                strcpy(result.state, "RELEASED_SCL_LOW");
            else if (result.released.sda_stable && result.released.sda_level == 0)
                strcpy(result.state, "RELEASED_SDA_LOW");
            else strcpy(result.state, "LINES_LOW_AFTER_RESTORE");
            goto done;
        }
    }

    /* Never treat bus_reset(ESP_OK) as proof: probe, then read real registers.
     * Raw driver calls avoid recursively taking our shared mutex. */
    result.verify_attempted = true;
    result.probe_error = record(i2c_master_probe(bus, 0x20, IO_TIMEOUT_MS));
    if (result.probe_error == ESP_OK) {
        const uint8_t registers[] = {2, 6, 0};
        uint8_t *values[] = {&result.output0, &result.config0, &result.input0};
        for (unsigned i = 0; i < 3; ++i) {
            BoardBus_LineSample idle = sample_lines_locked();
            if (!lines_high(&idle)) break;
            result.read_attempted |= 1U << i;
            result.read_errors[i] = record(i2c_master_transmit_receive(
                xl, &registers[i], 1, values[i], 1, IO_TIMEOUT_MS));
            if (result.read_errors[i] == ESP_OK) result.valid |= 1U << i;
        }
    }
    result.after_verify = sample_lines_locked();
    result.verified = result.valid == 7 && lines_high(&result.after_verify);
    if (result.verified) {
        strcpy(result.state, "RECOVERED");
        result.result = ESP_OK;
    } else {
        strcpy(result.state, lines_high(&result.after_verify) ? "VERIFY_FAILED" : "LINES_LOW_AFTER_VERIFY");
        if (result.probe_error != ESP_OK) result.result = result.probe_error;
        else for (unsigned i = 0; i < 3; ++i) {
            if ((result.read_attempted & (1U << i)) && result.read_errors[i] != ESP_OK) {
                result.result = result.read_errors[i];
                break;
            }
        }
    }
done:
    result.completed = true;
    portENTER_CRITICAL(&state_lock);
    status.recovery = result;
    portEXIT_CRITICAL(&state_lock);
    if (acquired) xSemaphoreGive(bus_lock);
    return result.result;
}

static void sample_idle_lines(void)
{
    esp_err_t error = take();
    bool acquired = error == ESP_OK;
    BoardBus_LineSample sample = {0};
    int64_t sampled_at = 0;
    if (acquired && !bus) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        /* No GPIO reconfiguration or bus recovery here. All existing clients
         * share this lock, so no transaction can begin during these samples.
         * Allow the minimum bus-free interval before observing five levels
         * over 100 us; a low level can still mean a slave is holding the bus. */
        sample = sample_lines_locked();
        sampled_at = esp_timer_get_time();
    }
    portENTER_CRITICAL(&state_lock);
    status.line_sample_valid = error == ESP_OK;
    status.line_sample_error = error;
    status.sda_level = sample.sda_level; status.scl_level = sample.scl_level;
    status.sda_stable = sample.sda_stable; status.scl_stable = sample.scl_stable;
    line_sample_at_us = sampled_at;
    portEXIT_CRITICAL(&state_lock);
    if (acquired) xSemaphoreGive(bus_lock);
}
void BoardBus_RefreshStatus(void)
{
    uint8_t values[3] = {0}, valid = 0;
    const uint8_t registers[] = {2, 6, 0};
    for (unsigned i = 0; i < 3; ++i) {
        if (BoardBus_XlRead(registers[i], &values[i]) == ESP_OK) valid |= 1U << i;
    }
    portENTER_CRITICAL(&state_lock);
    status.output0 = values[0]; status.config0 = values[1]; status.input0 = values[2]; status.valid = valid;
    portEXIT_CRITICAL(&state_lock);
    sample_idle_lines();
}
