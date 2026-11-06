#include "Bsp_UartBridge.h"
#include "Bsp_UdpTelemetry.h"
#include "Bsp_Camera.h"
#include "UartBridgePolicy.h"
#include "Bsp_I2cDiagnostics.h"
#include "Bsp_AudioMonitor.h"
#if defined(ROBOT_PUBLIC_TEST_BUILD) && ROBOT_PUBLIC_TEST_BUILD
#include "Bsp_CameraSerial.h"
#endif

#include <stddef.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define BRIDGE_RX_BUFFER_SIZE 4096
#define BRIDGE_TX_BUFFER_SIZE 2048
#define BRIDGE_EVENT_QUEUE_SIZE 32
#define BRIDGE_CHUNK_SIZE 256

volatile UartBridge_DirectionStats g_uart_bridge_stats[2];

typedef struct {
    uart_port_t source;
    uart_port_t destination;
    QueueHandle_t events;
    volatile UartBridge_DirectionStats *stats;
} BridgeDirection;

static BridgeDirection directions[2] = {
    { .source = UART_NUM_0, .destination = UART_NUM_1, .stats = &g_uart_bridge_stats[0] },
    { .source = UART_NUM_1, .destination = UART_NUM_0, .stats = &g_uart_bridge_stats[1] }
};
static TaskHandle_t bridge_tasks[2];
typedef struct { char line[80]; size_t length; TickType_t deadline; } BridgeCommand;
static QueueHandle_t control_commands;
static QueueHandle_t stop_commands;

bool UartBridge_QueueCommand(const char *line, size_t length, TickType_t deadline)
{
    if (!control_commands || !line || !length || length >= sizeof(((BridgeCommand *)0)->line) ||
        line[length-1]!='\n' || (int32_t)(deadline-xTaskGetTickCount())<=0) return false;
    BridgeCommand command={.length=length,.deadline=deadline};
    memcpy(command.line,line,length);
    /* STOP has its own overwrite slot, even when the normal queue is full.
     * The STM32 command-id barrier rejects any older motion left behind it. */
    if (length >= 11 && !memcmp(line,"PERIPH STOP",11) && (line[11]==' ' || line[11]=='\r' || line[11]=='\n')) {
        /* Network motion/STOP share one HTTP producer. Remove its unsent
         * motion before replacing an older STOP (even an out-of-order id). */
        (void)xQueueReset(control_commands);
        return stop_commands && xQueueOverwrite(stop_commands,&command)==pdTRUE;
    }
    return xQueueSend(control_commands,&command,0)==pdTRUE;
}
/* False means a partial/failed write needs a NUL+LF recovery before any more
 * bytes go to STM32. A dropped deadline never sends even the command prefix. */
static bool send_control_command(void)
{
    BridgeCommand command;
    volatile UartBridge_DirectionStats *stats = directions[0].stats;
    if ((!stop_commands || xQueueReceive(stop_commands,&command,0)!=pdTRUE) &&
        (!control_commands || xQueueReceive(control_commands,&command,0)!=pdTRUE)) return true;
    uint32_t reserve = UartBridgeCommand_ReserveTicks(command.length, UART_BRIDGE_STM_BAUD, portTICK_PERIOD_MS);
    TickType_t now = xTaskGetTickCount();
    if (!UartBridgeCommand_CanSend(command.deadline, now, reserve)) {
        ++stats->command_expired; return true;
    }
    TickType_t wait = command.deadline - now - reserve;
    /* Bound the drain pause so the PC RX ring can keep accepting bytes. */
    if (wait > pdMS_TO_TICKS(200)) wait = pdMS_TO_TICKS(200);
    esp_err_t error = uart_wait_tx_done(UART_NUM_1, wait);
    if (error != ESP_OK) { ++stats->command_tx_errors; return true; }
    if (!UartBridgeCommand_CanSend(command.deadline, xTaskGetTickCount(), reserve)) {
        ++stats->command_expired; return true;
    }
    /* Only this task writes UART1. Its drained TX ring holds the whole command,
     * so uart_write_bytes cannot wait behind previous PC traffic. */
    int written = uart_write_bytes(UART_NUM_1,command.line,command.length);
    if (written != (int)command.length) {
        ++stats->write_errors; ++stats->command_tx_errors;
        stats->unforwarded_bytes += written > 0 ? command.length - (size_t)written : command.length;
        return false;
    }
    stats->tx_bytes += (uint32_t)written;
    return true;
}

/* Only the STM32 -> PC task writes network status, and only between complete
 * STM32 lines. Network/event tasks never print to UART0 concurrently. */
static void report_network(TickType_t *last_report)
{
    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - *last_report) < pdMS_TO_TICKS(5000U)) { return; }
    char status[256];
    size_t length = UdpTelemetry_FormatStatus(status, sizeof(status));
    if (length != 0U) { (void)uart_write_bytes(UART_NUM_0, status, length); }
    *last_report = now;
}

static void write_serial_json(const char *prefix, char *json)
{
    if (!json) return;
    static const char end[] = "\r\n";
    size_t length = strlen(json);
    size_t prefix_length = strlen(prefix);
    bool ok = uart_write_bytes(UART_NUM_0, prefix, prefix_length) == (int)prefix_length;
    if (ok) ok = uart_write_bytes(UART_NUM_0, json, length) == (int)length;
    /* Always terminate even if the body write failed, so later STM lines do
     * not get appended to the response prefix. */
    if (uart_write_bytes(UART_NUM_0, end, sizeof(end)-1) != (int)(sizeof(end)-1)) ok = false;
    if (!ok) ++directions[1].stats->write_errors;
    cJSON_free(json);
}
static void report_serial_data(void)
{
    /* Only the STM->PC writer calls this at log-line boundaries. Audio is not
     * tied to the five-second NET report; TakeSerialPacket enforces its own
     * 200 ms cadence and returns NULL when stopped/stale/not enough samples. */
    write_serial_json("ESP_AUDIO ", AudioMonitor_TakeSerialPacket());
    write_serial_json("ESP_STATUS ", UdpTelemetry_TakeSerialStatus());
#if defined(ROBOT_PUBLIC_TEST_BUILD) && ROBOT_PUBLIC_TEST_BUILD
    if (!CameraSerial_WriteNext()) ++directions[1].stats->write_errors;
#endif
}

static bool count_events(BridgeDirection *direction)
{
    uart_event_t event;
    bool lost = false;
    /* BUFFER_FULL is backpressure, not evidence that bytes were lost. */
    for (unsigned i = 0; i < BRIDGE_EVENT_QUEUE_SIZE; ++i) {
        if (xQueueReceive(direction->events, &event, 0) != pdTRUE) {
            break;
        }
        switch (event.type) {
        case UART_FIFO_OVF: ++direction->stats->fifo_overflows; lost = true; break;
        case UART_BUFFER_FULL: ++direction->stats->buffer_full_events; break;
        case UART_FRAME_ERR: ++direction->stats->frame_errors; lost = true; break;
        case UART_PARITY_ERR: ++direction->stats->parity_errors; lost = true; break;
        case UART_BREAK: ++direction->stats->break_events; break;
        default: break;
        }
    }
    return lost;
}

static bool poison_stm_partial(BridgeDirection *direction)
{
    /* STM's parser rejects NUL and discards through LF. LF alone could execute
     * a damaged prefix that happens to be a valid shorter command. */
    const uint8_t poison[] = {0, '\n'};
    int written = uart_write_bytes(direction->destination, poison, sizeof(poison));
    if (written > 0) direction->stats->tx_bytes += (uint32_t)written;
    if (written == (int)sizeof(poison)) return true;
    ++direction->stats->write_errors;
    return false;
}

static void resync_input(BridgeDirection *direction, UartBridgeFrameState *framing, UartBridgeLocalState *local,
                         bool *poison_pending)
{
    /* A loss event does not locate the gap inside the RX ring. Discard the
     * uncertain buffered region, then wait for a fresh stream delimiter. */
    (void)uart_flush_input(direction->source);
    (void)xQueueReset(direction->events);
    UartBridgeFrame_Invalidate(framing);
    UartBridgeLocal_Reset(local);
    ++direction->stats->resyncs;
    if (direction->source == UART_NUM_0) {
        *poison_pending = true;
    } else {
        UdpTelemetry_InvalidateStream();
        static const char loss[] = "\r\n[BRIDGE] STM RX loss; partial line discarded\r\n";
        if (uart_write_bytes(direction->destination, loss, sizeof(loss)-1) != (int)(sizeof(loss)-1))
            ++direction->stats->write_errors;
    }
}

static void bridge_task(void *argument)
{
    BridgeDirection *direction = argument;
    uint8_t data[BRIDGE_CHUNK_SIZE];
    UartBridgeFrameState framing;
    UartBridgeLocalState local;
    UartBridgeLocal_Reset(&local);
    UartBridgeFrame_Init(&framing, direction->source == UART_NUM_0);
    bool poison_pending = false;
    TickType_t last_report = xTaskGetTickCount() - pdMS_TO_TICKS(5000U);
    TickType_t last_input = xTaskGetTickCount();
    /* Start together only after both tasks and the startup banner succeed. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
        if (count_events(direction)) resync_input(direction, &framing, &local, &poison_pending);
        if (poison_pending) {
            if (!poison_stm_partial(direction)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            poison_pending = false;
        }
        TickType_t read_wait = pdMS_TO_TICKS(20);
#if defined(ROBOT_PUBLIC_TEST_BUILD) && ROBOT_PUBLIC_TEST_BUILD
        if (direction->source == UART_NUM_1 && CameraSerial_IsBusy()) read_wait = pdMS_TO_TICKS(1);
#endif
        int length = uart_read_bytes(direction->source, data, sizeof(data), read_wait);
        if (length < 0) {
            ++direction->stats->read_errors;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        direction->stats->rx_bytes += (uint32_t)length;
        if (length > 0) last_input = xTaskGetTickCount();
        /* Catch a loss delivered while uart_read_bytes was waiting. */
        if (count_events(direction)) {
            direction->stats->unforwarded_bytes += (uint32_t)length;
            resync_input(direction, &framing, &local, &poison_pending);
            continue;
        }
        if (length == 0) {
            if (direction->source == UART_NUM_0) {
                bool pending = uxQueueMessagesWaiting(control_commands) || uxQueueMessagesWaiting(stop_commands);
                if (pending && !framing.at_boundary && !framing.discarding &&
                    (TickType_t)(xTaskGetTickCount()-last_input) >= pdMS_TO_TICKS(250)) {
                    /* A USB unplug can strand a half command forever. Poison
                     * that prefix; still discard its possible late USB tail. */
                    UartBridgeFrame_Invalidate(&framing);
                    UartBridgeLocal_Reset(&local);
                    poison_pending = !poison_stm_partial(direction);
                }
                if (!poison_pending && (framing.at_boundary || framing.discarding))
                    poison_pending = !send_control_command();
            } else if (framing.at_boundary && !framing.discarding) {
                report_serial_data(); report_network(&last_report);
            }
        }
        int offset = 0;
        while (offset < length) {
            if (count_events(direction)) {
                direction->stats->unforwarded_bytes += (uint32_t)(length - offset);
                resync_input(direction, &framing, &local, &poison_pending);
                break;
            }
            if (poison_pending) {
                if (!poison_stm_partial(direction)) {
                    direction->stats->unforwarded_bytes += (uint32_t)(length - offset);
                    resync_input(direction, &framing, &local, &poison_pending);
                    break;
                }
                poison_pending = false;
            }
            size_t discarded = UartBridgeFrame_Discard(&framing, data + offset, (size_t)(length-offset));
            if (discarded && direction->source == UART_NUM_1)
                UdpTelemetry_Mirror(data + offset, discarded); /* Includes the resynchronizing LF. */
            direction->stats->unforwarded_bytes += (uint32_t)discarded;
            offset += (int)discarded;
            if (offset == length) break;
            int segment = (int)UartBridgeFrame_Segment(&framing, data + offset, (size_t)(length-offset));
            uint8_t filtered[BRIDGE_CHUNK_SIZE+4];
            const uint8_t *forward = data+offset;
            size_t forward_length = (size_t)segment;
            if (direction->source == UART_NUM_0) {
                UartBridgeLocalAction action;
                char request_id[17]; uint32_t ttl_ms;
                UartBridgeLocalCharge charge;
                UartBridgeLocalWifi wifi;
                SoundCommand sound;
                forward_length = UartBridgeLocal_FilterWithSound(&local, forward, forward_length,
                                                        filtered, &action, request_id, &ttl_ms, &charge, &wifi, &sound);
                forward = filtered;
                if (action == UART_BRIDGE_LOCAL_SOUND) {
                    (void)UdpTelemetry_QueueSound(&sound);
                    UdpTelemetry_RequestSerialStatus();
                } else if (action == UART_BRIDGE_LOCAL_SCAN) {
                    (void)I2cDiagnostics_RequestScan(request_id);
                    UdpTelemetry_RequestSerialStatus();
                } else if (action == UART_BRIDGE_LOCAL_CAMERA_CHECK) {
                    (void)Camera_RequestCheck(request_id);
                    UdpTelemetry_RequestSerialStatus();
#if defined(ROBOT_PUBLIC_TEST_BUILD) && ROBOT_PUBLIC_TEST_BUILD
                } else if (action == UART_BRIDGE_LOCAL_CAMERA_SNAPSHOT) {
                    (void)CameraSerial_Request(request_id);
#endif
                } else if (action == UART_BRIDGE_LOCAL_STATUS) UdpTelemetry_RequestSerialStatus();
                else if (action == UART_BRIDGE_LOCAL_AUDIO_START) {
                    (void)AudioMonitor_SetSerialLease(request_id, ttl_ms);
                    UdpTelemetry_RequestSerialStatus();
                } else if (action == UART_BRIDGE_LOCAL_AUDIO_STOP) {
                    AudioMonitor_StopSerial(request_id);
                    UdpTelemetry_RequestSerialStatus();
                } else if (action == UART_BRIDGE_LOCAL_CHARGE_CURRENT) {
                    (void)UdpTelemetry_QueueSerialCurrent(request_id,charge.ma,charge.deadline_ms,charge.boot_prefix);
                    UdpTelemetry_RequestSerialStatus();
                } else if (action == UART_BRIDGE_LOCAL_WIFI_CONNECT ||
                           action == UART_BRIDGE_LOCAL_WIFI_RECONNECT ||
                           action == UART_BRIDGE_LOCAL_WIFI_DISCONNECT || action == UART_BRIDGE_LOCAL_WIFI_SCAN) {
                    const char *operation=action==UART_BRIDGE_LOCAL_WIFI_CONNECT ? "connect" :
                        action==UART_BRIDGE_LOCAL_WIFI_RECONNECT ? "reconnect" :
                        action==UART_BRIDGE_LOCAL_WIFI_SCAN ? "scan" : "disconnect";
                    (void)UdpTelemetry_QueueWifiPaired(request_id,operation,wifi.ssid,wifi.password,
                                                wifi.deadline_ms,wifi.boot_prefix,wifi.api_token);
                    UdpTelemetry_RequestSerialStatus();
                }
                UartBridgeLocal_ClearSecret(&wifi,sizeof(wifi));
                /* Input may contain a hex-encoded password. It is never echoed. */
                UartBridgeLocal_ClearSecret(data+offset,(size_t)segment);
            } else UdpTelemetry_Mirror(forward, forward_length);
            /* Exact byte count: never use printf/strlen or add a local echo. */
            int written = forward_length ? uart_write_bytes(direction->destination, forward, forward_length) : 0;
            if (written != (int)forward_length) {
                ++direction->stats->write_errors;
                if (written > 0) direction->stats->tx_bytes += (uint32_t)written;
                direction->stats->unforwarded_bytes += (uint32_t)(forward_length - (written > 0 ? written : 0));
                resync_input(direction, &framing, &local, &poison_pending);
                break;
            }
            direction->stats->tx_bytes += (uint32_t)written;
            UartBridgeFrame_Forwarded(&framing, forward, forward_length);
            offset += segment;
            if (framing.at_boundary) {
                if (direction->source == UART_NUM_1) { report_serial_data(); report_network(&last_report); }
                else poison_pending = !send_control_command();
            }
        }
    }
}

static esp_err_t configure_uart(uart_port_t port, int baud, int tx, int rx, QueueHandle_t *events)
{
    const uart_config_t config = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };
    esp_err_t error = uart_param_config(port, &config);
    if (error != ESP_OK) { return error; }
    error = uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (error != ESP_OK) { return error; }
    return uart_driver_install(port, BRIDGE_RX_BUFFER_SIZE, BRIDGE_TX_BUFFER_SIZE,
                               BRIDGE_EVENT_QUEUE_SIZE, events, 0);
}

esp_err_t UartBridge_Start(void)
{
    if (!control_commands) {
        control_commands=xQueueCreate(1,sizeof(BridgeCommand));
        if (!control_commands) return ESP_ERR_NO_MEM;
    }
    if (!stop_commands) {
        stop_commands=xQueueCreate(1,sizeof(BridgeCommand));
        if (!stop_commands) return ESP_ERR_NO_MEM;
    }
    if (uart_is_driver_installed(UART_NUM_0) || uart_is_driver_installed(UART_NUM_1)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = configure_uart(UART_NUM_0, UART_BRIDGE_PC_BAUD, UART_BRIDGE_PC_TX_GPIO,
                                    UART_BRIDGE_PC_RX_GPIO, &directions[0].events);
    if (error != ESP_OK) { return error; }
    error = configure_uart(UART_NUM_1, UART_BRIDGE_STM_BAUD, UART_BRIDGE_STM_TX_GPIO,
                           UART_BRIDGE_STM_RX_GPIO, &directions[1].events);
    if (error != ESP_OK) {
        uart_driver_delete(UART_NUM_0);
        return error;
    }
    /* Boot messages and this one startup banner precede transparent forwarding.
     * Suppress ESP_LOG output, including the normal "Returned from app_main".
     */
    esp_log_level_set("*", ESP_LOG_NONE);
    if (xTaskCreate(bridge_task, "pc_to_stm", 4096, &directions[0], 8, &bridge_tasks[0]) != pdPASS) {
        error = ESP_ERR_NO_MEM;
        goto failed;
    }
    if (xTaskCreate(bridge_task, "stm_to_pc", 6144, &directions[1], 8, &bridge_tasks[1]) != pdPASS) {
        error = ESP_ERR_NO_MEM;
        goto failed;
    }
    static const char banner[] =
        "\r\n[BRIDGE READY] CH340 UART0=921600 8N1 <-> STM32 UART3=115200 8N1\r\n"
        "[BRIDGE] ESP TX=GPIO4 -> STM PC11; ESP RX=GPIO5 <- STM PC10\r\n";
    if (uart_write_bytes(UART_NUM_0, banner, sizeof(banner) - 1U) != (int)(sizeof(banner) - 1U)) {
        error = ESP_FAIL;
        goto failed;
    }
    error = uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000));
    if (error != ESP_OK) { goto failed; }
#if defined(ROBOT_PUBLIC_TEST_BUILD) && ROBOT_PUBLIC_TEST_BUILD
    (void)CameraSerial_Start();
#endif
    xTaskNotifyGive(bridge_tasks[0]);
    xTaskNotifyGive(bridge_tasks[1]);
    return ESP_OK;

failed:
    for (unsigned i = 0; i < 2; ++i) {
        if (bridge_tasks[i] != NULL) {
            vTaskDelete(bridge_tasks[i]);
            bridge_tasks[i] = NULL;
        }
    }
    uart_driver_delete(UART_NUM_1);
    uart_driver_delete(UART_NUM_0);
    return error;
}
