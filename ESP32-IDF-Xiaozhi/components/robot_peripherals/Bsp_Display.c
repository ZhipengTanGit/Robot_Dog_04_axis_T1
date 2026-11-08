#include "Bsp_Display.h"
#include "Bsp_BoardBus.h"
#include "PeripheralMath.h"
#include "DisplaySyncLogic.h"
#include "DisplayRecoveryLogic.h"
#include "DisplayLayout.h"
#include "FaceLogic.h"
#include "FaceRender.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WIDTH DISPLAY_PANEL_WIDTH
#define HEIGHT DISPLAY_PANEL_HEIGHT
#define X_OFFSET DISPLAY_PANEL_X_OFFSET
#define STRIP_HEIGHT 24
/* Native clears use 410x16 DMA blocks. Rotated text uses narrow rectangles
 * with the same maximum pixel count, below the verified 16 KiB limit. */
#define DMA_STRIP_HEIGHT 16
#define PIXEL_BUFFER_BYTES (DISPLAY_TEXT_WIDTH * STRIP_HEIGHT * 2U)
#define LCD_MAX_TRANSFER_BYTES (16U * 1024U)
#define LCD_SPI_HZ 40000000
_Static_assert(WIDTH * DMA_STRIP_HEIGHT * 2U <= LCD_MAX_TRANSFER_BYTES,
    "display DMA strip exceeds the verified transfer limit");
_Static_assert(PIXEL_BUFFER_BYTES >= WIDTH * STRIP_HEIGHT * 2U,
    "display clear must fit in the rotated text buffer");
_Static_assert(DISPLAY_TEXT_X + DISPLAY_TEXT_WIDTH <= DISPLAY_UI_WIDTH,
    "landscape text exceeds the visible panel");
#define TE_GPIO 13
#define ROW_COUNT 8
#define TE_WAIT_MS 80
/* Normal operation: qualify a fresh Vblank edge before each dirty strip.
 * At 40 MHz the split 24-row transfer fits the shortest (y=54) window. */
#define DISPLAY_TE_SYNCHRONIZE 1
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static Display_Status status = {
    .state = "OFF", .init_stage = "NOT_STARTED", .touch_state = "OFF",
    .transport = "ESP_LCD_QSPI", .write_hz = LCD_SPI_HZ,
    .readback_enabled = false,
    .readback_reset = { .state = "NOT_ATTEMPTED" },
    .readback_initialized = { .state = "NOT_ATTEMPTED" },
};
static char battery[640], peripheral[640];
static uint32_t battery_at, peripheral_at;
static esp_lcd_panel_io_handle_t lcd;
/* Only the display task accesses the IO and this ownership flag. A failed
 * queue call can still leave DMA in flight, so only a successful drain may
 * release the pixel buffer for reuse or destruction. */
static bool lcd_color_pending;
static bool lcd_initializing;
static const char *lcd_init_phase = "INIT";
static i2c_master_dev_handle_t touch;
static uint8_t *pixels;
static bool started;
static bool spi_bus_owned, te_handler_installed;
static uint32_t last_init_attempt_ms;
static TaskHandle_t display_task_handle;
static int64_t te_last_at_us;
static uint8_t te_valid_periods;
static const unsigned row_y[ROW_COUNT] = {32,76,120,164,208,252,296,350};
typedef struct { char text[DISPLAY_TEXT_COLUMNS + 1U]; uint16_t color; bool valid; } DisplayRow;
static DisplayRow desired[ROW_COUNT], displayed[ROW_COUNT];
static bool dirty[ROW_COUNT], refresh_pending;
static unsigned next_dirty_row;
static FaceLogic face;
static uint8_t *face_pixels;
static bool shown_face;
static uint32_t last_face_frame;
static bool face_inflight;
static unsigned face_offset;
static uint32_t face_started_us;
static unsigned requested_page;
static int requested_mood = -1;
void Display_SetVoiceEmotion(unsigned mood)
{
    if (mood > FACE_COMFORT) return;
    portENTER_CRITICAL(&lock);
    requested_mood = (int)mood;
    requested_page = 1U;
    portEXIT_CRITICAL(&lock);
}
esp_err_t Display_RequestFace(bool visible)
{
    if(!started)return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&lock);requested_page=visible?1U:2U;portEXIT_CRITICAL(&lock);
    return ESP_OK;
}
static void stage(const char *name, esp_err_t error)
{
    portENTER_CRITICAL(&lock);
    snprintf(status.state, sizeof(status.state), "%s", name); status.error = error;
    portEXIT_CRITICAL(&lock);
}
static void init_stage(const char *name)
{
    portENTER_CRITICAL(&lock);
    strlcpy(status.init_stage, name, sizeof(status.init_stage));
    portEXIT_CRITICAL(&lock);
}
static void init_command_stage(uint8_t reg)
{
    if (!lcd_initializing) return;
    char name[32];
    snprintf(name, sizeof(name), "%s_CMD_%02X", lcd_init_phase, reg);
    init_stage(name);
}
static void touch_stage(const char *name, esp_err_t error)
{
    portENTER_CRITICAL(&lock);
    snprintf(status.touch_state, sizeof(status.touch_state), "%s", name); status.touch_error = error;
    portEXIT_CRITICAL(&lock);
}
void Display_GetStatus(Display_Status *out)
{
    if (!out) return;
    int64_t last;
    uint8_t valid_periods;
    portENTER_CRITICAL(&lock);
    *out = status; last = te_last_at_us; valid_periods = te_valid_periods;
    portEXIT_CRITICAL(&lock);
    int64_t now = esp_timer_get_time();
    uint64_t age = last > 0 && now >= last ? (uint64_t)(now - last) : UINT64_MAX;
    out->te_age_ms = age / 1000U < UINT32_MAX ? (uint32_t)(age / 1000U) : UINT32_MAX;
    out->te_signal_present = age <= DISPLAY_TE_MAX_AGE_US &&
        DisplaySync_SignalFresh(valid_periods, out->te_period_us, (uint32_t)age);
}
void Display_UpdateRobot(const char *battery_line, const char *peripheral_line)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&lock);
    if (battery_line) { strlcpy(battery, battery_line, sizeof(battery)); battery_at = now; }
    if (peripheral_line) { strlcpy(peripheral, peripheral_line, sizeof(peripheral)); peripheral_at = now; }
    portEXIT_CRITICAL(&lock);
}
static void IRAM_ATTR te_interrupt(void *unused)
{
    (void)unused;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&lock);
    if (te_last_at_us) {
        uint64_t delta = (uint64_t)(now - te_last_at_us);
        status.te_period_us = delta <= UINT32_MAX ? (uint32_t)delta : UINT32_MAX;
        if (delta >= DISPLAY_TE_MIN_PERIOD_US && delta <= DISPLAY_TE_MAX_PERIOD_US) {
            if (te_valid_periods < DISPLAY_TE_REQUIRED_PERIODS) ++te_valid_periods;
        } else te_valid_periods = 0;
    }
    te_last_at_us = now;
    ++status.te_edges;
    portEXIT_CRITICAL_ISR(&lock);
    BaseType_t wake = pdFALSE;
    if (display_task_handle) vTaskNotifyGiveFromISR(display_task_handle, &wake);
    if (wake == pdTRUE) portYIELD_FROM_ISR();
}
static esp_err_t lcd_wait_idle(void)
{
    if (!lcd_color_pending) return ESP_OK;
    if (!lcd) return ESP_ERR_INVALID_STATE;
    /* IDF 5.5.3 panel_io_spi_tx_param drains all queued color transactions
     * before considering the command. -1/NULL/0 sends no extra bus traffic. */
    esp_err_t error = esp_lcd_panel_io_tx_param(lcd, -1, NULL, 0);
    if (error == ESP_OK) lcd_color_pending = false;
    return error;
}
static esp_err_t command(uint8_t reg, const void *data, size_t length)
{
    init_command_stage(reg);
    if (!lcd || (length && !data)) return ESP_ERR_INVALID_ARG;
    /* OC5300 reference: commands and parameters use {02, 00, CMD, 00}
     * in single-line mode. Pixel payloads use the separate 0x32 QIO path. */
    const int packed = (int)((0x02U << 24) | ((uint32_t)reg << 8));
    return esp_lcd_panel_io_tx_param(lcd, packed, data, length);
}
static esp_err_t one(uint8_t reg, uint8_t value) { return command(reg, &value, 1); }
static esp_err_t lcd_add_writer(void)
{
    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = 12, .dc_gpio_num = -1, .spi_mode = 0,
        .pclk_hz = LCD_SPI_HZ, .trans_queue_depth = 10,
        .lcd_cmd_bits = 32, .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    return esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &lcd);
}
static esp_err_t draw_region(unsigned x, unsigned y, unsigned width, unsigned height)
{
    if (!lcd || !pixels || !DisplayLayout_WindowValid(x,y,width,height) ||
        width * height > PIXEL_BUFFER_BYTES / 2U) return ESP_ERR_INVALID_ARG;
    if (lcd_color_pending) return ESP_ERR_INVALID_STATE;
    unsigned left = X_OFFSET + x, right = left + width - 1U;
    const uint8_t columns[] = { left >> 8, left & 255, right >> 8, right & 255 };
    unsigned max_rows = DisplayLayout_DmaRows(width);
    for (unsigned offset = 0; offset < height;) {
        unsigned rows_count = height - offset;
        if (rows_count > max_rows) rows_count = max_rows;
        unsigned first = y + offset, last = first + rows_count - 1;
        const uint8_t rows[] = { first >> 8, first & 255, last >> 8, last & 255 };
        esp_err_t error = command(0x2a, columns, sizeof(columns));
        if (error == ESP_OK) error = command(0x2b, rows, sizeof(rows));
        if (error != ESP_OK) return error;
        /* Independent window + RAMWR per segment; RGB565 is already MSB
         * first. The command stays single-line; only payload uses QIO. */
        const int packed = (int)((0x32U << 24) | (0x2cU << 8));
        init_command_stage(0x2c);
        lcd_color_pending = true;
        error = esp_lcd_panel_io_tx_color(lcd, packed,
            pixels + offset * width * 2U, width * rows_count * 2U);
        /* Drain even after an enqueue error: earlier chunks may be live.
         * Returning after enqueue alone would let the next row overwrite
         * pixels while DMA is still reading it. */
        esp_err_t drain_error = lcd_wait_idle();
        if (drain_error != ESP_OK) {
            if (lcd_initializing) {
                char name[32];
                snprintf(name, sizeof(name), "%s_DMA_DRAIN", lcd_init_phase);
                init_stage(name);
            }
            return drain_error;
        }
        if (error != ESP_OK) return error;
        offset += rows_count;
    }
    return ESP_OK;
}
static esp_err_t draw_strip(unsigned y, unsigned height)
{
    return draw_region(0, y, WIDTH, height);
}
static esp_err_t lcd_init(void)
{
    lcd_initializing = true;
    lcd_init_phase = "INIT";
    init_stage("XL_RESET_CONFIG");
    esp_err_t error = BoardBus_XlConfigureOutput(BOARD_XL_LCD_RESET, BOARD_XL_LCD_RESET);
    if (error != ESP_OK) return error;
    const spi_bus_config_t bus = {
        .sclk_io_num = 20, .data0_io_num = 3, .data1_io_num = 9,
        .data2_io_num = 10, .data3_io_num = 11, .data4_io_num = -1,
        .data5_io_num = -1, .data6_io_num = -1, .data7_io_num = -1,
        .max_transfer_sz = LCD_MAX_TRANSFER_BYTES, .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    init_stage("SPI_BUS");
    error = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) return error;
    spi_bus_owned = true;
    init_stage("PANEL_IO");
    error = lcd_add_writer();
    if (error != ESP_OK) return error;
    init_stage("PIXEL_ALLOC");
    pixels = heap_caps_malloc(PIXEL_BUFFER_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!pixels) return ESP_ERR_NO_MEM;
    gpio_config_t te = { .pin_bit_mask = 1ULL << TE_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        /* CO5300 Reg35 M=0 / Reg44 N=0: TE is high during Vblank.
         * The scan-window policy therefore starts on the rising edge. */
        .intr_type = GPIO_INTR_POSEDGE };
    init_stage("TE_GPIO");
    error = gpio_config(&te);
    if (error != ESP_OK) return error;
    /* Service flags 0: IRAM_ATTR alone does not promise delivery while flash
     * cache is disabled. A late/missed TE is rejected by the task's window. */
    init_stage("TE_ISR_SERVICE");
    error = gpio_install_isr_service(0);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    init_stage("TE_ISR_HANDLER");
    if ((error = gpio_isr_handler_add(TE_GPIO, te_interrupt, NULL)) != ESP_OK) return error;
    te_handler_installed = true;
    /* Match the verified ESP32_01_Displays_OC5300 sequence, retaining this board's
     * XL9555 reset and pin map. Do not switch handles for register reads in
     * this reference-path test: its status stays explicitly NOT_ATTEMPTED.
     * Transport success alone does not prove that the panel is displaying. */
    vTaskDelay(pdMS_TO_TICKS(20));
    init_stage("RESET_LOW");
    if ((error = BoardBus_XlOutput(BOARD_XL_LCD_RESET, 0)) != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(100));
    init_stage("RESET_HIGH");
    if ((error = BoardBus_XlOutput(BOARD_XL_LCD_RESET, BOARD_XL_LCD_RESET)) != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(150));
    /* The verified OC5300 build sends Sleep Out / Display On without data. */
    if ((error = command(0x11, NULL, 0)) != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(120));
    const uint8_t init[][2] = {
        {0xfe, 0x00}, {0xc4, 0x80}, {0x36, 0x00}, {0x3a, 0x55}, {0x35, 0x00},
        {0x53, 0x20}, {0x51, 0xff}, {0x63, 0xff},
    };
    for (unsigned i = 0; i < sizeof(init) / sizeof(init[0]); ++i)
        if ((error = one(init[i][0], init[i][1])) != ESP_OK) return error;
    const uint8_t te_scanline[] = {0x00, 0x00};
    if ((error = command(0x44, te_scanline, sizeof(te_scanline))) != ESP_OK) return error;
    const uint8_t columns[] = {0x00, 0x16, 0x01, 0xaf};
    const uint8_t rows[] = {0x00, 0x00, 0x01, 0xf5};
    if ((error = command(0x2a, columns, sizeof(columns))) != ESP_OK) return error;
    if ((error = command(0x2b, rows, sizeof(rows))) != ESP_OK) return error;
    if ((error = command(0x11, NULL, 0)) != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(60));
    if ((error = command(0x29, NULL, 0)) != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(100));
    if ((error = one(0x51, 0xff)) != ESP_OK) return error;
    /* OC5300 Init wrapper repeats Display On / FF brightness; Main_config
     * then selects 180. Preserve that tested call order exactly. */
    if ((error = command(0x29, NULL, 0)) != ESP_OK) return error;
    if ((error = one(0x51, 0xff)) != ESP_OK) return error;
    if ((error = one(0x51, 180)) != ESP_OK) return error;
    portENTER_CRITICAL(&lock); status.te_sync_enabled = DISPLAY_TE_SYNCHRONIZE; portEXIT_CRITICAL(&lock);
    lcd_init_phase = "CLEAR";
    init_stage("CLEAR_BEGIN");
    memset(pixels, 0, WIDTH * STRIP_HEIGHT * 2);
    for (unsigned y = 0; y < HEIGHT; y += STRIP_HEIGHT) {
        unsigned h = HEIGHT - y < STRIP_HEIGHT ? HEIGHT - y : STRIP_HEIGHT;
        if ((error = draw_strip(y, h)) != ESP_OK) return error;
    }
    if (!DISPLAY_TE_SYNCHRONIZE) {
        lcd_init_phase = "COLOR_BARS";
        init_stage("COLOR_BARS_BEGIN");
        /* Bottom RGB/white bars provide a static check independent of fonts,
         * incoming telemetry, touch data and TE. RGB565 is sent MSB first. */
        const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
        for (unsigned y = 0; y < STRIP_HEIGHT; ++y) {
            for (unsigned x = 0; x < WIDTH; ++x) {
                uint16_t color = colors[x * 4U / WIDTH];
                unsigned offset = (y * WIDTH + x) * 2U;
                pixels[offset] = color >> 8; pixels[offset + 1] = color & 0xff;
            }
        }
        if ((error = draw_strip(HEIGHT - 44, STRIP_HEIGHT)) != ESP_OK) return error;
    }
    init_stage("READY");
    return ESP_OK;
}
static void lcd_cleanup(void)
{
    if (te_handler_installed) {
        (void)gpio_intr_disable(TE_GPIO);
        (void)gpio_isr_handler_remove(TE_GPIO);
        te_handler_installed = false;
    }
    portENTER_CRITICAL(&lock); status.te_sync_enabled = false; portEXIT_CRITICAL(&lock);
    /* If drain fails, keep the IO and pixel storage alive and stop drawing.
     * An error return from tx_color does not prove that DMA stopped. */
    if (lcd_wait_idle() != ESP_OK) return;
    if (lcd) {
        if (esp_lcd_panel_io_del(lcd) != ESP_OK) return;
        lcd = NULL;
    }
    if (spi_bus_owned && !lcd && spi_bus_free(SPI2_HOST) == ESP_OK) spi_bus_owned = false;
    /* No queued operation retains the buffer after the successful drain. */
    if (pixels) { heap_caps_free(pixels); pixels = NULL; }
}
static esp_err_t touch_init(void)
{
    const gpio_config_t config = { .pin_bit_mask = 1ULL << 14, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE };
    esp_err_t error = gpio_config(&config);
    if (error == ESP_OK) error = BoardBus_XlConfigureOutput(BOARD_XL_TOUCH_RESET, 0);
    if (error != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(10));
    if ((error = BoardBus_XlOutput(BOARD_XL_TOUCH_RESET, BOARD_XL_TOUCH_RESET)) != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(150));
    if ((error = BoardBus_Probe(0x5a)) != ESP_OK) return error;
    error = BoardBus_AddDevice(0x5a, &touch);
    if (error == ESP_OK) {
        const uint8_t normal_mode[] = {0xd1, 0x09};
        error = BoardBus_Write(touch, normal_mode, sizeof(normal_mode));
    }
    return error;
}
static void poll_touch(void)
{
    /* Reading an already acknowledged packet does not produce a fresh touch.
     * INT remains low after a failed transaction, so it also enables retries. */
    if (gpio_get_level(14)) return;
    uint8_t packet[15] = {0}, count = 0;
    uint16_t x[2] = {0}, y[2] = {0};
    const uint8_t address[] = {0xd0, 0x00}, ack[] = {0xd0, 0x00, 0xab};
    esp_err_t error = BoardBus_ReadStopped(touch, address, sizeof(address), packet, sizeof(packet));
    if (error == ESP_OK) {
        portENTER_CRITICAL(&lock);
        snprintf(status.touch_report,sizeof(status.touch_report),"%02X %02X %02X %02X %02X %02X %02X",
            packet[0],packet[1],packet[2],packet[3],packet[4],packet[5],packet[6]);
        portEXIT_CRITICAL(&lock);
    }
    if (error == ESP_OK) error = BoardBus_Write(touch, ack, sizeof(ack));
    if (error == ESP_OK && packet[6] != 0xab) {
        portENTER_CRITICAL(&lock); ++status.touch_empty_reports; portEXIT_CRITICAL(&lock);
        return;
    }
    if (error == ESP_OK && !Cst9217_Decode(packet, sizeof(packet), x, y, &count)) error = ESP_ERR_INVALID_RESPONSE;
    if (error == ESP_OK) {
        for (unsigned i = 0; i < count; ++i)
            if (!DisplayLayout_Touch(&x[i], &y[i])) error = ESP_ERR_INVALID_RESPONSE;
    }
    portENTER_CRITICAL(&lock);
    ++status.touch_reads; status.touch_valid = error == ESP_OK;
    status.touch_count = error == ESP_OK ? count : 0;
    if (error == ESP_OK) {
        memcpy(status.x, x, sizeof(x)); memcpy(status.y, y, sizeof(y));
        status.last_touch_ms = (uint32_t)(esp_timer_get_time() / 1000);
    } else ++status.touch_errors;
    portEXIT_CRITICAL(&lock);
    touch_stage(error == ESP_OK ? "READY" : "READ_ERROR", error);
    FaceLogic_Screen(&face, (uint32_t)(esp_timer_get_time()/1000), error == ESP_OK, count != 0);
}
/* Compact 5x7 column font, digits followed by A..Z. */
static const uint8_t font[][5] = {
    {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},{0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},
    {0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},
};
static uint8_t glyph(char c, unsigned column)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= '0' && c <= '9') return font[c - '0'][column];
    if (c >= 'A' && c <= 'Z') return font[10 + c - 'A'][column];
    if (c == '.') return column == 2 ? 0x60 : 0;
    if (c == ':') return column == 2 ? 0x36 : 0;
    if (c == '-') return 8;
    if (c == '/') return (uint8_t)(0x40 >> column);
    if (c == '%') { const uint8_t p[] = {0x63,0x13,8,0x64,0x63}; return p[column]; }
    if (c == '=') return 0x14;
    return 0;
}
static void prepare_text_row(const char *text, uint16_t color)
{
    memset(pixels, 0, PIXEL_BUFFER_BYTES);
    for (unsigned letter = 0; text[letter] && letter < DISPLAY_TEXT_COLUMNS; ++letter) {
        for (unsigned col = 0; col < 5; ++col) {
            uint8_t bits = glyph(text[letter], col);
            for (unsigned row = 0; row < 7; ++row) if (bits & (1U << row)) {
                for (unsigned dy = 0; dy < 3; ++dy) for (unsigned dx = 0; dx < 3; ++dx) {
                    unsigned at = DisplayLayout_PixelOffset(letter * 18U + col * 3U + dx, row * 3U + dy);
                    pixels[at] = color >> 8; pixels[at + 1] = color & 255;
                }
            }
        }
    }
}
static void set_row(unsigned index, const char *text, uint16_t color)
{
    if (index >= ROW_COUNT) return;
    strlcpy(desired[index].text, text, sizeof(desired[index].text));
    desired[index].color = color; desired[index].valid = true;
    dirty[index] = !displayed[index].valid || displayed[index].color != color ||
        strcmp(displayed[index].text, desired[index].text) != 0;
    if (dirty[index]) refresh_pending = true;
}
static void update_pending_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < ROW_COUNT; ++i) if (dirty[i]) ++count;
    portENTER_CRITICAL(&lock);
    status.pending_strips = count;
    if (!count && refresh_pending) { ++status.frames; refresh_pending = false; }
    portEXIT_CRITICAL(&lock);
}
static uint32_t lcd_transfer_budget_us(void)
{
    return DisplaySync_TransferBudgetUs(STRIP_HEIGHT, DISPLAY_TEXT_WIDTH, LCD_SPI_HZ,
        DisplayLayout_DmaRows(STRIP_HEIGHT));
}
static bool wait_te_window(unsigned y, uint32_t budget, uint32_t *edge_us, uint32_t *scan_start_us)
{
    uint32_t previous;
    portENTER_CRITICAL(&lock); previous = status.te_edges; portEXIT_CRITICAL(&lock);
    /* Drop queued old edges. If an edge races this drain, wait for the next
     * one; never use a cached event to start a new visible transfer. */
    (void)ulTaskNotifyTake(pdTRUE, 0);
    TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(TE_WAIT_MS);
    stage("WAIT_TE", ESP_OK);
    for (;;) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout || ulTaskNotifyTake(pdTRUE, timeout - elapsed) == 0) {
            portENTER_CRITICAL(&lock); ++status.te_timeouts; portEXIT_CRITICAL(&lock);
            stage("TE_TIMEOUT", ESP_ERR_TIMEOUT); return false;
        }
        uint32_t sequence, period, edge;
        uint8_t valid_periods;
        portENTER_CRITICAL(&lock);
        sequence = status.te_edges; period = status.te_period_us;
        edge = (uint32_t)te_last_at_us; valid_periods = te_valid_periods;
        portEXIT_CRITICAL(&lock);
        if (sequence == previous) continue;
        DisplaySync_Decision decision = DisplaySync_Check((uint32_t)esp_timer_get_time(), edge,
            period, valid_periods, y, HEIGHT,
            budget, scan_start_us);
        if (decision != DISPLAY_SYNC_READY) {
            portENTER_CRITICAL(&lock);
            if (decision == DISPLAY_SYNC_PERIOD_INVALID) ++status.te_invalid_periods;
            else ++status.te_late_skips;
            portEXIT_CRITICAL(&lock);
            stage(decision == DISPLAY_SYNC_PERIOD_INVALID ? "TE_PERIOD" : "TE_LATE", ESP_ERR_TIMEOUT);
            return false;
        }
        *edge_us = edge;
        return true;
    }
}
static void draw_dirty_row(void)
{
    unsigned index = DisplaySync_NextDirty(dirty, ROW_COUNT, &next_dirty_row);
    if (index == ROW_COUNT) return;
    /* Every landscape row becomes a native column rectangle starting at
     * scan line 54. Rotate through dirty rows after a late TE as before. */
    /* Font rendering is deliberately before the edge wait, outside the short
     * scan window. This task owns the one internal DMA strip buffer. */
    prepare_text_row(desired[index].text, desired[index].color);
    uint32_t edge_us = 0, scan_start_us = 0;
    if (DISPLAY_TE_SYNCHRONIZE && !wait_te_window(DISPLAY_TEXT_NATIVE_Y, lcd_transfer_budget_us(), &edge_us, &scan_start_us)) return;
    uint32_t began = (uint32_t)esp_timer_get_time();
    if (DISPLAY_TE_SYNCHRONIZE && (uint64_t)(began - edge_us) +
        lcd_transfer_budget_us() + DISPLAY_TE_GUARD_US >= scan_start_us) {
        portENTER_CRITICAL(&lock); ++status.te_late_skips; portEXIT_CRITICAL(&lock);
        stage("TE_LATE", ESP_ERR_TIMEOUT); return;
    }
    esp_err_t error = draw_region(row_y[index], DISPLAY_TEXT_NATIVE_Y,
        STRIP_HEIGHT, DISPLAY_TEXT_WIDTH);
    uint32_t ended = (uint32_t)esp_timer_get_time(), duration = ended - began;
    bool overrun = DISPLAY_TE_SYNCHRONIZE && (uint64_t)(ended - edge_us) + DISPLAY_TE_GUARD_US >= scan_start_us;
    portENTER_CRITICAL(&lock);
    status.last_draw_us = duration;
    if (duration > status.max_draw_us) status.max_draw_us = duration;
    if (error != ESP_OK) { ++status.write_errors; status.configured = false; }
    else if (overrun) ++status.te_overruns;
    else if (DISPLAY_TE_SYNCHRONIZE) ++status.te_syncs;
    portEXIT_CRITICAL(&lock);
    if (error != ESP_OK) {
        lcd_cleanup();
        /* Runtime write failures also need a quiet retry backoff. */
        last_init_attempt_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    if (error == ESP_OK) {
        displayed[index] = desired[index]; dirty[index] = false;
        update_pending_count();
    }
    stage(error != ESP_OK ? "WRITE_ERROR" : overrun ? "TE_OVERRUN" : "OUTPUT_SENT",
        error != ESP_OK ? error : overrun ? ESP_ERR_TIMEOUT : ESP_OK);
}
static bool token(const char *line, const char *key, char *out, size_t size)
{
    size_t key_length = strlen(key);
    for (const char *p = line; p && *p;) {
        if (strncmp(p, key, key_length) == 0 && p[key_length] == '=') {
            p += key_length + 1; size_t length = strcspn(p, " \r\n");
            if (length >= size) length = size - 1;
            memcpy(out, p, length); out[length] = 0; return true;
        }
        p = strchr(p, ' '); if (p) ++p;
    }
    if (size) out[0] = 0;
    return false;
}
static void update_rows(void)
{
    char bat[sizeof(battery)], per[sizeof(peripheral)]; uint32_t bat_at, per_at;
    portENTER_CRITICAL(&lock);
    memcpy(bat, battery, sizeof(bat)); memcpy(per, peripheral, sizeof(per));
    bat_at = battery_at; per_at = peripheral_at;
    portEXIT_CRITICAL(&lock);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    char text[48], value[32];
    set_row(0, DISPLAY_TE_SYNCHRONIZE ? "ROBOT DOG V0.24" : "ROBOT DOG - NO TE", 0x07ff);
    char bat_valid_text[8];
    bool bat_fresh = bat[0] && now - bat_at < 3500;
    bool bat_valid = bat_fresh && token(bat, "valid", bat_valid_text, sizeof(bat_valid_text)) &&
        strcmp(bat_valid_text, "1") == 0;
    if (bat_fresh && token(bat, "set_mA", value, sizeof(value)))
        snprintf(text, sizeof(text), "SET %s MA", value);
    else snprintf(text, sizeof(text), "STM BAT STALE");
    set_row(1, text, 0xffff);
    if (bat_valid && token(bat, "soc", value, sizeof(value))) {
        /* Keep the interval explicit; do not invent a precise SOC. */
        for (char *p = value; *p; ++p) if (*p == '(' || *p == ')' || *p == '[' || *p == ']') *p = ' '; else if (*p == ',') *p = '-';
        snprintf(text, sizeof(text), "BAT %s", value);
    } else snprintf(text, sizeof(text), "BAT NA");
    set_row(2, text, 0xffff);
    char valid[8], age[16];
    bool aht_valid = per[0] && now - per_at < 3500 &&
        token(per, "aht_valid", valid, sizeof(valid)) && strcmp(valid, "1") == 0 &&
        token(per, "aht_age_ms", age, sizeof(age)) && strtoul(age, NULL, 10) + (now - per_at) < 5000;
    if (aht_valid && token(per, "aht_t_cC", value, sizeof(value)) && strcmp(value, "NA"))
        snprintf(text, sizeof(text), "TEMP %.2f C", strtol(value, NULL, 10) / 100.0);
    else snprintf(text, sizeof(text), "TEMP NA");
    set_row(3, text, 0xffff);
    if (aht_valid && token(per, "aht_rh_cPct", value, sizeof(value)) && strcmp(value, "NA"))
        snprintf(text, sizeof(text), "HUM %.2f %%", strtoul(value, NULL, 10) / 100.0);
    else snprintf(text, sizeof(text), "HUM NA");
    set_row(4, text, 0xffff);
    Display_Status d; Display_GetStatus(&d);
    if (d.touch_valid && d.touch_count) snprintf(text, sizeof(text), "TOUCH %u %u", d.x[0], d.y[0]);
    else snprintf(text, sizeof(text), "TOUCH %s", d.touch_present ? "WAIT" : "NA");
    set_row(5, text, 0xffe0);
    snprintf(text, sizeof(text), "XIAOZHI VOICE");
    set_row(6, text, 0xffff);
    snprintf(text, sizeof(text), "TAP FOR FACE %luS", (unsigned long)(now / 1000));
    set_row(7, text, 0x7bef);
    update_pending_count();
}
static void update_face_state(uint32_t now)
{
    char per[sizeof(peripheral)], value[24];
    uint32_t received;
    portENTER_CRITICAL(&lock);
    memcpy(per, peripheral, sizeof(per)); received = peripheral_at;
    portEXIT_CRITICAL(&lock);
    if (per[0] && now - received < 1500U) {
        uint32_t source = 0, presses = 0;
        bool valid = token(per, "t", value, sizeof(value)) && value[0] &&
            strspn(value, "0123456789") == strlen(value);
        if (valid) source = (uint32_t)strtoul(value, NULL, 10);
        valid = valid && token(per, "touch_press", value, sizeof(value)) && value[0] &&
            strspn(value, "0123456789") == strlen(value);
        if (valid) presses = (uint32_t)strtoul(value, NULL, 10);
        valid = valid && token(per, "touch", value, sizeof(value)) &&
            (strcmp(value, "0") == 0 || strcmp(value, "1") == 0);
        if (valid) FaceLogic_Head(&face, now, source, value[0] == '1', presses);
    }
    FaceLogic_Update(&face, now);
    unsigned request;
    int mood;
    portENTER_CRITICAL(&lock);
    request=requested_page; requested_page=0;
    mood=requested_mood; requested_mood=-1;
    portEXIT_CRITICAL(&lock);
    if (mood >= 0) {
        if (mood == FACE_COMFORT) { face.pet_active=true; face.pet_at=now; }
        else face.selected=(unsigned)mood;
    }
    if(request){face.face_page=request==1U;face.screen_down=false;face.screen_long=false;}
    portENTER_CRITICAL(&lock);
    strlcpy(status.ui_mode, face.face_page ? "FACE" : "DEBUG", sizeof(status.ui_mode));
    strlcpy(status.expression, face.face_page ? FaceLogic_Name(FaceLogic_Mood(&face, now)) : "NONE", sizeof(status.expression));
    status.screen_taps = face.taps; status.head_touch_events = face.head_events;
    status.head_touch = face.head_known && face.head_down && now - face.head_at < 1200U;
    status.te_sync_enabled = DISPLAY_TE_SYNCHRONIZE;
    portEXIT_CRITICAL(&lock);
}
static void face_write_error(esp_err_t error)
{
    portENTER_CRITICAL(&lock);
    ++status.write_errors; status.configured = false;
    portEXIT_CRITICAL(&lock);
    lcd_cleanup();
    last_init_attempt_ms = (uint32_t)(esp_timer_get_time()/1000);
    stage("WRITE_ERROR", error);
}
static bool change_page(void)
{
    if (shown_face == face.face_page) return true;
    if (face.face_page && !face_pixels) {
        face_pixels = heap_caps_malloc(FACE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!face_pixels) { face.face_page = false; stage("FACE_MEMORY", ESP_ERR_NO_MEM); return false; }
    }
    memset(pixels, 0, PIXEL_BUFFER_BYTES);
    for (unsigned y=0; y<HEIGHT; y+=STRIP_HEIGHT) {
        unsigned h=HEIGHT-y < STRIP_HEIGHT ? HEIGHT-y : STRIP_HEIGHT;
        esp_err_t error=draw_strip(y,h);
        if (error != ESP_OK) { face_write_error(error); return false; }
    }
    shown_face=face.face_page;
    memset(displayed,0,sizeof(displayed)); memset(dirty,0,sizeof(dirty));
    refresh_pending=false;
    portENTER_CRITICAL(&lock); status.pending_strips=0; portEXIT_CRITICAL(&lock);
    if (!shown_face) update_rows();
    last_face_frame=0;
    face_inflight=false;face_offset=0;
    return true;
}
static void draw_face_frame(uint32_t now)
{
    if (!face_pixels) return;
    if (!face_inflight) {
        if(now-last_face_frame<40U)return;
        last_face_frame=now;
        FaceRender_Draw(face_pixels, FaceLogic_Mood(&face,now), now);
        face_inflight=true;face_offset=0;face_started_us=(uint32_t)esp_timer_get_time();
    }
    const unsigned width=FACE_H, chunk=DisplayLayout_DmaRows(FACE_H);
    uint32_t budget=DisplaySync_TransferBudgetUs(width,chunk,LCD_SPI_HZ,chunk)+450U;
    uint32_t edge=0, deadline=0;
    if (!wait_te_window(FACE_NATIVE_Y+face_offset,budget,&edge,&deadline)) return;
    uint32_t period;
    portENTER_CRITICAL(&lock);period=status.te_period_us;portEXIT_CRITICAL(&lock);
    for (unsigned offset=face_offset; offset<FACE_W;) {
        unsigned rows=FACE_W-offset < chunk ? FACE_W-offset : chunk;
        memcpy(pixels,face_pixels+offset*width*2U,width*rows*2U);
        budget=DisplaySync_TransferBudgetUs(width,rows,LCD_SPI_HZ,chunk)+450U;
        deadline=(uint32_t)((uint64_t)(FACE_NATIVE_Y+offset)*period/HEIGHT);
        if ((uint64_t)((uint32_t)esp_timer_get_time()-edge)+budget+DISPLAY_TE_GUARD_US>=deadline) {
            portENTER_CRITICAL(&lock);++status.te_late_skips;portEXIT_CRITICAL(&lock);
            return; /* Keep the frozen image and resume THIS chunk at the next TE. */
        }
        esp_err_t error=draw_region(FACE_Y,FACE_NATIVE_Y+offset,width,rows);
        if (error!=ESP_OK) { face_inflight=false;face_write_error(error); return; }
        offset+=rows;face_offset=offset;
        if ((uint64_t)((uint32_t)esp_timer_get_time()-edge)+DISPLAY_TE_GUARD_US>=deadline && offset<FACE_W) {
            portENTER_CRITICAL(&lock);++status.te_overruns;portEXIT_CRITICAL(&lock);
            return;
        }
    }
    face_inflight=false;
    uint32_t duration=(uint32_t)esp_timer_get_time()-face_started_us;
    portENTER_CRITICAL(&lock);
    ++status.face_frames; ++status.frames; ++status.te_syncs;
    status.last_draw_us=duration;
    if(duration>status.max_draw_us)status.max_draw_us=duration;
    portEXIT_CRITICAL(&lock);
    stage("OUTPUT_SENT",ESP_OK);
}
static bool lcd_resources_idle(void)
{
    return !lcd && !pixels && !spi_bus_owned && !te_handler_installed && !lcd_color_pending;
}
static esp_err_t display_initialize_once(void)
{
    last_init_attempt_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&lock);
    ++status.init_attempts;
    status.retry_pending = false;
    status.configured = false;
    te_valid_periods = 0;
    te_last_at_us = 0;
    status.te_period_us = 0;
    portEXIT_CRITICAL(&lock);
    stage("INITIALIZING", ESP_OK);
    esp_err_t error = lcd_init();
    lcd_initializing = false;
    if (error != ESP_OK) {
        lcd_cleanup();
        /* Count the retry delay from the failure, not from a long init's start. */
        last_init_attempt_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    if (error == ESP_OK) {
        /* Init clears panel RAM. Even unchanged labels must be sent again. */
        memset(displayed, 0, sizeof(displayed));
        memset(dirty, 0, sizeof(dirty));
        refresh_pending = false;
        next_dirty_row = 0;
        shown_face = false;
    }
    portENTER_CRITICAL(&lock);
    status.configured = error == ESP_OK;
    status.pending_strips = 0;
    status.retry_pending = error != ESP_OK &&
        DisplayRecovery_CanRetry(status.init_attempts, lcd_resources_idle());
    portEXIT_CRITICAL(&lock);
    stage(error == ESP_OK ? (DISPLAY_TE_SYNCHRONIZE ? "WAIT_TE" : "INITIALIZING") : "INIT_ERROR", error);
    return error;
}
static void display_task(void *unused)
{
    (void)unused;
    display_task_handle = xTaskGetCurrentTaskHandle();
    FaceLogic_Init(&face,(uint32_t)(esp_timer_get_time()/1000));
    face.face_page = true;
    stage("INITIALIZING", ESP_OK); touch_stage("INITIALIZING", ESP_OK);
    esp_err_t error = touch_init();
    portENTER_CRITICAL(&lock); status.touch_present = error == ESP_OK; portEXIT_CRITICAL(&lock);
    touch_stage(error == ESP_OK ? "WAITING_DATA" : "NOT_FOUND", error);
    (void)display_initialize_once();
    uint32_t last_update = 0, last_touch = 0;
    bool first_update = true;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (!status.configured) {
            bool idle = lcd_resources_idle();
            unsigned attempts = status.init_attempts;
            portENTER_CRITICAL(&lock);
            status.retry_pending = DisplayRecovery_CanRetry(attempts, idle);
            portEXIT_CRITICAL(&lock);
            BoardBus_Status bus_state;
            BoardBus_GetStatus(&bus_state);
            bool healthy = bus_state.valid == 7 && bus_state.line_sample_valid &&
                bus_state.line_sample_age_ms <= 3000 && bus_state.sda_stable &&
                bus_state.scl_stable && bus_state.sda_level && bus_state.scl_level;
            if (DisplayRecovery_ShouldRetry(now, last_init_attempt_ms, attempts, idle, healthy)) {
                (void)display_initialize_once();
                first_update = true;
                now = (uint32_t)(esp_timer_get_time() / 1000);
            }
        }
        if (touch && now - last_touch >= 40) { last_touch = now; poll_touch(); }
        now = (uint32_t)(esp_timer_get_time()/1000);
        update_face_state(now);
        if (status.configured && !change_page()) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (status.configured && !face.face_page && (first_update || now - last_update >= 1000)) {
            first_update = false; last_update = now; update_rows();
        }
        if (status.configured) {
            if (face.face_page) draw_face_frame(now);
            else draw_dirty_row();
        }
        /* In synchronized mode, absent TE pauses visible writes. The bench
         * diagnostic mode sends dirty strips directly without claiming sync. */
        vTaskDelay(pdMS_TO_TICKS(status.configured ? 2 : 20));
    }
}
esp_err_t Display_Start(void)
{
    if (started) return ESP_OK;
    started = true;
    if (xTaskCreate(display_task, "periph_display", 6144, NULL, 2, NULL) != pdPASS) {
        started = false;
        stage("TASK_ERROR", ESP_ERR_NO_MEM); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
