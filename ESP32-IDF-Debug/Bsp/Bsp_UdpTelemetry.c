#include "Bsp_UdpTelemetry.h"
#include "BuildConfig.h"
#if ROBOT_PUBLIC_TEST_BUILD
#include "Bsp_CameraSerial.h"
#endif
#include "TelemetryLine.h"
#include "Bsp_Camera.h"
#include "Bsp_Peripherals.h"
#include "Bsp_UartBridge.h"
#include "ChargeCommand.h"
#include "Bsp_AudioMonitor.h"
#include "Bsp_Audio.h"
#include "Bsp_I2cDiagnostics.h"
#include "Bsp_Wifi.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifndef ROBOT_NETWORK_ENABLED
#define ROBOT_NETWORK_ENABLED 1
#endif

#define LOG_LINES 20U
typedef enum { NET_OFF, NET_INIT, NET_CONNECTING, NET_RETRY, NET_READY, NET_ERROR } NetState;
typedef struct {
    NetState state;
    uint32_t ip, generation, sent, send_errors, oversize, retries;
    uint32_t disconnects, got_ip_events, frames_sent, frame_errors;
    TickType_t next_connect;
    int error, http_status;
    uint16_t reason, last_disconnect_reason;
    uint8_t connected, associated, started, connect_pending, upload_ready;
} NetworkState;
typedef struct {
    char raw[TELEMETRY_LINE_MAX + 1U], peripheral[TELEMETRY_LINE_MAX + 1U];
    char periph_ack[128];
    uint32_t sequence, uart_lines;
    TickType_t last_bat, last_peripheral, last_ack;
    unsigned next, count;
    uint8_t has_bat, has_peripheral;
    /* Kept last: USB snapshots omit the repeated log ring allocation/copy. */
    char logs[LOG_LINES][TELEMETRY_LINE_MAX + 1U];
} TelemetryStore;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static NetworkState network;
static TelemetryStore telemetry;
static TelemetryLine line = { .discarding = 1U };
static uint8_t startup_called;
static char device_id[24], boot_id[33];
static uint32_t payload_sequence;
static bool identity_ready;
static bool serial_status_pending;
static TickType_t serial_status_at;
typedef struct {
    char id[33], status[12], detail[40];
    unsigned ma;
    TickType_t deadline;
    bool active, acknowledged;
} CommandState;
static CommandState charge_command;
typedef struct { char id[17], status[24]; esp_err_t error; uint32_t expires_ms; } SoundReply;
static SoundReply sound_reply[2];
static SemaphoreHandle_t sound_dispatch;
static void sound_ack(const char *line)
{
    char id[17], reason[24]; unsigned ok; int used=0;
    if (sscanf(line,"PERIPH_ACK cmd=BEEP id=%16[0-9a-f] ok=%1u reason=%23[A-Z_]%n",id,&ok,reason,&used)!=3 ||
        ok>1 || strlen(id)!=16) return;
    for (const char *tail=line+used; *tail; ++tail) if (*tail!='\r' && *tail!='\n') return;
    uint32_t now_ms=(uint32_t)(esp_timer_get_time()/1000);
    portENTER_CRITICAL(&state_lock);
    if (!strcmp(sound_reply[1].id,id) && (int32_t)(now_ms-sound_reply[1].expires_ms)<0 &&
        (!strcmp(sound_reply[1].status,"QUEUED") || !strcmp(sound_reply[1].status,"STARTED"))) {
        const char *status=!ok ? "FAILED" : !strcmp(reason,"STARTED") ? "STARTED" :
            !strcmp(reason,"DONE") ? "COMPLETED" : !strcmp(reason,"STOPPED") ? "STOPPED" : "FAILED";
        snprintf(sound_reply[1].status,sizeof(sound_reply[1].status),"%s",status);
        sound_reply[1].error=!strcmp(status,"FAILED") ? ESP_FAIL : ESP_OK;
    }
    portEXIT_CRITICAL(&state_lock);
}
/* More than the maximum new commands possible within a 15-second lease at
 * one response/second. Keeps delayed duplicate responses from being replayed. */
static char command_ids[32][33];
static unsigned next_command_id;
static bool command_seen_or_mark(const char *id)
{
    portENTER_CRITICAL(&state_lock);
    for (unsigned i=0;i<32;++i) if (!strcmp(command_ids[i],id)) {
        portEXIT_CRITICAL(&state_lock); return true;
    }
    snprintf(command_ids[next_command_id],sizeof(command_ids[0]),"%s",id);
    next_command_id=(next_command_id+1)%32;
    portEXIT_CRITICAL(&state_lock);
    return false;
}
typedef struct { char body[768]; size_t used; bool overflow; } UploadResponse;
static const char *const names[] = { "OFF", "INIT", "CONNECTING", "RETRY", "READY", "ERROR" };

static NetworkState state_copy(void)
{
    BoardWifi_Status wifi;
    NetworkState copy;
    portENTER_CRITICAL(&state_lock);
    /* Order concurrent snapshots. BoardWifi never calls back with wifi_lock
     * held, so the only nested lock order is state_lock -> wifi_lock. */
    BoardWifi_GetStatus(&wifi);
    if (!wifi.ready || network.generation != wifi.generation) network.upload_ready = 0;
    network.connected = wifi.ready;
    network.ip = wifi.ip_address; network.generation = wifi.generation;
    network.disconnects = wifi.disconnects; network.got_ip_events = wifi.got_ip_events;
    network.reason = (uint16_t)wifi.disconnect_reason; network.error = wifi.error;
    network.state = !strcmp(wifi.state, "READY") ? NET_READY :
                    !strcmp(wifi.state, "CONNECTING") ? NET_CONNECTING :
                    !strcmp(wifi.state, "RETRY") ? NET_RETRY :
                    !strcmp(wifi.state, "ERROR") ? NET_ERROR : NET_OFF;
    copy = network; portEXIT_CRITICAL(&state_lock);
    return copy;
}
static void queue_line(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    portENTER_CRITICAL(&state_lock);
    char *stored = telemetry.logs[telemetry.next];
    for (size_t i = 0; i < length; ++i) { stored[i] = data[i] ? (char)data[i] : '?'; }
    stored[length] = '\0';
    if (length >= 6U && memcmp(data, "BAT t=", 6) == 0) {
        memcpy(telemetry.raw, stored, length + 1U);
        telemetry.last_bat = xTaskGetTickCount(); telemetry.has_bat = 1U;
    } else if (length >= 9U && memcmp(data, "PERIPH t=", 9) == 0) {
        memcpy(telemetry.peripheral, stored, length + 1U);
        telemetry.last_peripheral = xTaskGetTickCount(); telemetry.has_peripheral = 1U;
    } else if (length >= 11U && memcmp(data, "PERIPH_ACK ", 11) == 0 && length < sizeof(telemetry.periph_ack)) {
        memcpy(telemetry.periph_ack, stored, length + 1U);
        telemetry.last_ack = xTaskGetTickCount();
    }
    telemetry.next = (telemetry.next + 1U) % LOG_LINES;
    if (telemetry.count < LOG_LINES) { ++telemetry.count; }
    ++telemetry.sequence; ++telemetry.uart_lines;
    portEXIT_CRITICAL(&state_lock);
    /* Single UART producer owns this ring slot until the next callback.
     * This setter only copies state, so the local display works without Wi-Fi. */
    if (length >= 6U && memcmp(data, "BAT t=", 6) == 0) {
        Peripherals_UpdateRobot(stored, NULL);
    } else if (length >= 9U && memcmp(data, "PERIPH t=", 9) == 0) {
        Peripherals_UpdateRobot(NULL, stored);
    }
    sound_ack(stored);
    ChargeCommandAck ack;
    if (ChargeCommand_ParseAck(stored,&ack)) {
        portENTER_CRITICAL(&state_lock);
        if (charge_command.active && !strcmp(ack.id,charge_command.id) && ack.target_ma==charge_command.ma) {
            snprintf(charge_command.status,sizeof(charge_command.status),"%s",ack.status);
            snprintf(charge_command.detail,sizeof(charge_command.detail),"%s",ack.reason);
            charge_command.acknowledged=true;
        }
        portEXIT_CRITICAL(&state_lock);
    }
}
void UdpTelemetry_Mirror(const uint8_t *data, size_t length)
{
    uint32_t before = line.oversized_lines;
    TelemetryLine_Feed(&line, data, length, queue_line, NULL);
    if (line.oversized_lines != before) {
        portENTER_CRITICAL(&state_lock);
        network.oversize += line.oversized_lines - before;
        portEXIT_CRITICAL(&state_lock);
    }
}
void UdpTelemetry_InvalidateStream(void)
{
    TelemetryLine_Reset(&line,1U);
    portENTER_CRITICAL(&state_lock);
    telemetry.has_bat=0U;
    telemetry.has_peripheral=0U;
    telemetry.periph_ack[0]=0;
    portEXIT_CRITICAL(&state_lock);
}
size_t UdpTelemetry_FormatStatus(char *buffer, size_t size)
{
    NetworkState state = state_copy();
    esp_ip4_addr_t ip = { .addr = state.ip };
    int n = snprintf(buffer, size,
        "[NET] wifi=%s ip=" IPSTR " server=%s upload=%u tx=%lu err=%lu http=%d frame=%lu frame_err=%lu reason=%u\r\n",
        names[state.state], IP2STR(&ip), ROBOT_SERVER_URL, state.upload_ready,
        (unsigned long)state.sent, (unsigned long)state.send_errors, state.http_status,
        (unsigned long)state.frames_sent, (unsigned long)state.frame_errors, state.reason);
    return n < 0 || !size ? 0U : (size_t)n < size ? (size_t)n : size - 1U;
}

static uint32_t next_sequence(void)
{
    portENTER_CRITICAL(&state_lock);
    uint32_t sequence=++payload_sequence;
    portEXIT_CRITICAL(&state_lock);
    return sequence;
}
static char *make_payload(uint32_t sequence, bool serial_snapshot)
{
    /* Heap snapshot keeps the 20-line ring off the uploader task stack. */
    size_t snapshot_size=serial_snapshot ? offsetof(TelemetryStore,logs) : sizeof(TelemetryStore);
    TelemetryStore *copy = malloc(snapshot_size);
    if (!copy) { return NULL; }
    NetworkState state = state_copy();
    portENTER_CRITICAL(&state_lock);
    memcpy(copy,&telemetry,snapshot_size);
    portEXIT_CRITICAL(&state_lock);
    TickType_t now = xTaskGetTickCount();
    uint32_t age = (uint32_t)((TickType_t)(now - copy->last_bat) * portTICK_PERIOD_MS);
    uint32_t pa = (uint32_t)((TickType_t)(now - copy->last_peripheral) * portTICK_PERIOD_MS);
    BoardWifi_Status wifi; BoardWifi_GetStatus(&wifi);
    esp_ip4_addr_t ip = { .addr = wifi.ip_address };
    char ip_text[16]; snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&ip));
    cJSON *json = cJSON_CreateObject();
    if (!json) { free(copy); return NULL; }
    bool ok = true;
#define STR(o,k,v) do { if (!cJSON_AddStringToObject(o,k,v)) ok = false; } while(0)
#define NUM(o,k,v) do { if (!cJSON_AddNumberToObject(o,k,(double)(v))) ok = false; } while(0)
#define BOOL(o,k,v) do { if (!cJSON_AddBoolToObject(o,k,v)) ok = false; } while(0)
    STR(json,"device_id",device_id); STR(json,"boot_id",boot_id); NUM(json,"seq",sequence);
#if ROBOT_PUBLIC_TEST_BUILD
    STR(json,"firmware_build",ROBOT_FIRMWARE_BUILD_ID);
    STR(json,"idf_version",esp_get_idf_version());
    BOOL(json,"public_test_build",true);
    NUM(json,"wifi_pairing_proto",1);
    NUM(json,"camera_snapshot_proto",CameraSerial_Available() ? 1 : 0);
#endif
    STR(json,"ip",ip_text); STR(json,"ssid",wifi.ssid); STR(json,"wifi_state",wifi.state);
    NUM(json,"rssi",wifi.rssi); NUM(json,"uptime_ms",(uint32_t)(esp_timer_get_time()/1000));
    cJSON *wifi_json = cJSON_AddObjectToObject(json, "wifi");
    if (wifi_json) {
        STR(wifi_json,"state",wifi.state); STR(wifi_json,"ssid",wifi.ssid); STR(wifi_json,"ip",wifi.ip);
        STR(wifi_json,"request_id",wifi.request_id); STR(wifi_json,"request_status",wifi.request_status);
        NUM(wifi_json,"rssi",wifi.rssi); NUM(wifi_json,"error",wifi.error);
        NUM(wifi_json,"save_error",wifi.save_error); NUM(wifi_json,"disconnect_reason",wifi.disconnect_reason);
        NUM(wifi_json,"api_port",81); BOOL(wifi_json,"saved",wifi.saved); BOOL(wifi_json,"api_ready",wifi.api_ready);
        NUM(wifi_json,"stream_port",82); BOOL(wifi_json,"stream_ready",wifi.api_ready);
        NUM(wifi_json,"scan_proto",1);
        BOOL(wifi_json,"autoconnect",wifi.autoconnect);
        BoardWifi_ScanStatus *scan = malloc(sizeof(*scan));
        cJSON *scan_json = cJSON_AddObjectToObject(wifi_json, "scan");
        if (scan && scan_json) {
            BoardWifi_GetScanStatus(scan);
            STR(scan_json,"request_id",scan->request_id); BOOL(scan_json,"running",scan->running);
            BOOL(scan_json,"completed",scan->completed); NUM(scan_json,"error",scan->error);
            NUM(scan_json,"completed_ms",scan->completed_ms);
            cJSON *networks = cJSON_AddArrayToObject(scan_json,"networks");
            if (!networks) ok = false;
            else for (unsigned i=0; i<scan->count; ++i) {
                const BoardWifi_AccessPoint *ap=&scan->networks[i];
                char hex[65]; unsigned length=(unsigned)strlen(ap->ssid);
                for (unsigned j=0;j<length;++j) snprintf(hex+j*2,3,"%02x",(unsigned char)ap->ssid[j]);
                hex[length*2]=0;
                cJSON *item=cJSON_CreateObject();
                if (!item || !cJSON_AddItemToArray(networks,item)) { cJSON_Delete(item); ok=false; break; }
                /* Raw SSID bytes need not be UTF-8; hex keeps telemetry valid. */
                STR(item,"ssid_hex",hex); NUM(item,"rssi",ap->rssi);
                NUM(item,"channel",ap->channel); BOOL(item,"secure",ap->secure);
            }
        } else ok = false;
        free(scan);
    } else { ok = false; }
    if (copy->has_bat) { NUM(json,"uart_age_ms",age); }
    else if (!cJSON_AddNullToObject(json,"uart_age_ms")) { ok = false; }
    BOOL(json,"uart_valid",copy->has_bat && age <= 3000U);
    STR(json,"raw",copy->raw); STR(json,"peripheral_raw",copy->peripheral);
    NUM(json,"control_proto",1);
    NUM(json,"servo_speed_proto",1);
    SoundReply sounds[2];
    uint32_t sound_now=(uint32_t)(esp_timer_get_time()/1000);
    portENTER_CRITICAL(&state_lock);
    for (unsigned i=0;i<2;++i) {
        if (sound_reply[i].id[0] && (!strcmp(sound_reply[i].status,"QUEUED") || !strcmp(sound_reply[i].status,"STARTED")) &&
            (int32_t)(sound_now-sound_reply[i].expires_ms)>=0) {
            strcpy(sound_reply[i].status,"TIMED_OUT"); sound_reply[i].error=ESP_ERR_TIMEOUT;
        }
        sounds[i]=sound_reply[i];
    }
    portEXIT_CRITICAL(&state_lock);
    Audio_Status tone; Audio_GetStatus(&tone);
    if (sounds[0].id[0] && !strcmp(sounds[0].id,tone.tone_request_id) &&
        (!strcmp(sounds[0].status,"QUEUED") || !strcmp(sounds[0].status,"STARTED"))) {
        const char *state=!strcmp(tone.tone_state,"COMPLETED") ? "COMPLETED" :
            !strcmp(tone.tone_state,"STOPPED") ? "STOPPED" :
            (!strcmp(tone.tone_state,"ERROR")||!strcmp(tone.tone_state,"SHUTDOWN_ERROR")) ? "FAILED" : "STARTED";
        snprintf(sounds[0].status,sizeof(sounds[0].status),"%s",state); sounds[0].error=tone.tone_error;
        portENTER_CRITICAL(&state_lock);
        if (!strcmp(sound_reply[0].id,sounds[0].id) &&
            (!strcmp(sound_reply[0].status,"QUEUED") || !strcmp(sound_reply[0].status,"STARTED"))) sound_reply[0]=sounds[0];
        portEXIT_CRITICAL(&state_lock);
    }
    cJSON *sound_json=cJSON_AddObjectToObject(json,"sound_test");
    if (sound_json) {
        NUM(sound_json,"proto",1);
        for (unsigned i=0;i<2;++i) {
            cJSON *item=cJSON_AddObjectToObject(sound_json,i==0?"speaker":"buzzer");
            if (item) { STR(item,"id",sounds[i].id); STR(item,"status",sounds[i].status); NUM(item,"error",sounds[i].error); }
            else ok=false;
        }
    } else ok=false;
    STR(json,"periph_ack",copy->periph_ack);
    if (copy->periph_ack[0]) { NUM(json,"periph_ack_age_ms",(uint32_t)((TickType_t)(now-copy->last_ack)*portTICK_PERIOD_MS)); }
    else if (!cJSON_AddNullToObject(json,"periph_ack_age_ms")) { ok=false; }
    if (copy->has_peripheral) { NUM(json,"peripheral_age_ms",pa); }
    else if (!cJSON_AddNullToObject(json,"peripheral_age_ms")) { ok = false; }
    NUM(json,"log_seq",copy->sequence); NUM(json,"uart_lines",copy->uart_lines);
    NUM(json,"disconnect_reason",state.reason); NUM(json,"last_disconnect_reason",state.last_disconnect_reason);
    NUM(json,"wifi_disconnects",wifi.disconnects); NUM(json,"wifi_got_ip",wifi.got_ip_events);
    NUM(json,"generation",wifi.generation); NUM(json,"reset_reason",esp_reset_reason());
    NUM(json,"free_heap",esp_get_free_heap_size()); NUM(json,"min_free_heap",esp_get_minimum_free_heap_size());
    NUM(json,"internal_heap",heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    NUM(json,"min_internal_heap",heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    NUM(json,"upload_sent",state.sent); NUM(json,"upload_errors",state.send_errors);
    NUM(json,"upload_http_status",state.http_status); NUM(json,"frames_sent",state.frames_sent);
    NUM(json,"frame_errors",state.frame_errors); NUM(json,"uart_oversize",state.oversize);
    cJSON *bridge=cJSON_AddObjectToObject(json,"uart_bridge");
    if (bridge) {
        NUM(bridge,"pc_baud",UART_BRIDGE_PC_BAUD); NUM(bridge,"stm_baud",UART_BRIDGE_STM_BAUD);
        NUM(bridge,"pc_rx_bytes",g_uart_bridge_stats[0].rx_bytes);
        NUM(bridge,"pc_tx_errors",g_uart_bridge_stats[0].write_errors);
        NUM(bridge,"pc_fifo_overflows",g_uart_bridge_stats[0].fifo_overflows);
        NUM(bridge,"pc_frame_errors",g_uart_bridge_stats[0].frame_errors);
        NUM(bridge,"stm_rx_bytes",g_uart_bridge_stats[1].rx_bytes);
        NUM(bridge,"stm_tx_errors",g_uart_bridge_stats[1].write_errors);
        NUM(bridge,"stm_fifo_overflows",g_uart_bridge_stats[1].fifo_overflows);
        NUM(bridge,"stm_frame_errors",g_uart_bridge_stats[1].frame_errors);
        NUM(bridge,"stm_resyncs",g_uart_bridge_stats[1].resyncs);
    } else { ok=false; }
    CommandState command;
    portENTER_CRITICAL(&state_lock);
    if (charge_command.active && !charge_command.acknowledged &&
        (int32_t)(now-charge_command.deadline)>=0) {
        snprintf(charge_command.status,sizeof(charge_command.status),"FAILED");
        snprintf(charge_command.detail,sizeof(charge_command.detail),"NO_CONFIRMATION");
        charge_command.acknowledged=true;
    }
    command=charge_command;
    portEXIT_CRITICAL(&state_lock);
    if (command.active && command.acknowledged) {
        cJSON *ack=cJSON_AddObjectToObject(json,"command_ack");
        if (ack) {
            STR(ack,"id",command.id); STR(ack,"status",command.status);
            NUM(ack,"ma",command.ma); STR(ack,"detail",command.detail);
        } else { ok=false; }
    }
    cJSON *logs = cJSON_AddArrayToObject(json,"logs");
    if (!logs) { ok = false; }
    unsigned first = (copy->next + LOG_LINES - copy->count) % LOG_LINES;
    for (unsigned i = 0; !serial_snapshot && logs && i < copy->count; ++i) {
        cJSON *item = cJSON_CreateString(copy->logs[(first+i)%LOG_LINES]);
        if (!item || !cJSON_AddItemToArray(logs,item)) { cJSON_Delete(item); ok = false; }
    }
    free(copy);
    Camera_Status c; Camera_GetStatus(&c);
    cJSON *cj = cJSON_AddObjectToObject(json,"camera");
    if (cj) {
        /* Camera SDK logs are captured in memory rather than interleaved with
         * the UART protocol. Keep this buffer off the bridge task stack. */
        char *diagnostics = malloc(CAMERA_DIAGNOSTICS_CAPACITY);
        if (diagnostics) {
            Camera_GetDiagnostics(diagnostics, CAMERA_DIAGNOSTICS_CAPACITY);
            STR(cj,"diagnostics",diagnostics);
            free(diagnostics);
        } else { ok = false; }
        BOOL(cj,"ready",c.ready); STR(cj,"state",c.state); NUM(cj,"error",c.error);
        NUM(cj,"sensor_pid",c.sensor_pid); NUM(cj,"psram_bytes",c.psram_bytes);
        NUM(cj,"width",c.width); NUM(cj,"height",c.height); NUM(cj,"frames",c.frames);
        NUM(cj,"capture_failures",c.capture_failures); NUM(cj,"send_failures",c.send_failures);
        NUM(cj,"last_frame_ms",c.last_frame_ms); BOOL(cj,"diag_complete",c.diag_complete);
        NUM(cj,"xl_output0",c.xl_output0); NUM(cj,"xl_config0",c.xl_config0);
        NUM(cj,"xl_input0",c.xl_input0); NUM(cj,"xl_valid",c.xl_valid); NUM(cj,"xl_error",c.xl_error);
        NUM(cj,"sensor_probe_error",c.sensor_probe_error); NUM(cj,"id_restart_error",c.id_restart_error);
        NUM(cj,"id_stop_error",c.id_stop_error); NUM(cj,"id_restart",c.id_restart); NUM(cj,"id_stop",c.id_stop);
        NUM(cj,"sda_level",c.sda_level); NUM(cj,"scl_level",c.scl_level);
        BOOL(cj,"http_ready",wifi.api_ready && c.ready); NUM(cj,"http_port",81);
    } else { ok = false; }
    Camera_CheckStatus check; Camera_GetCheckStatus(&check);
    cJSON *cc=cJSON_AddObjectToObject(json,"camera_check");
    if (cc) {
        STR(cc,"id",check.id); STR(cc,"state",check.state); STR(cc,"stage",check.stage);
        BOOL(cc,"running",check.running); BOOL(cc,"completed",check.completed); BOOL(cc,"usable",check.usable);
        BOOL(cc,"expander_ok",check.expander_ok); BOOL(cc,"sccb_ack",check.sccb_ack);
        BOOL(cc,"id_ok",check.id_ok); BOOL(cc,"init_ok",check.init_ok); BOOL(cc,"frame_ok",check.frame_ok);
        NUM(cc,"sequence",check.sequence); NUM(cc,"started_ms",check.started_ms); NUM(cc,"completed_ms",check.completed_ms);
        NUM(cc,"frame_bytes",check.frame_bytes); NUM(cc,"frame_timestamp_ms",check.frame_timestamp_ms);
        NUM(cc,"sensor_pid",check.sensor_pid); NUM(cc,"width",check.width); NUM(cc,"height",check.height);
        NUM(cc,"xl_output0",check.xl_output0); NUM(cc,"xl_config0",check.xl_config0);
        NUM(cc,"xl_input0",check.xl_input0); NUM(cc,"xl_valid",check.xl_valid);
        NUM(cc,"error",check.error); NUM(cc,"expander_error",check.expander_error);
        NUM(cc,"sccb_error",check.sccb_error); NUM(cc,"id_error",check.id_error);
        NUM(cc,"init_error",check.init_error); NUM(cc,"frame_error",check.frame_error);
    } else { ok=false; }
    cJSON *peripherals = Peripherals_CreateJson();
    if (!peripherals || !cJSON_AddItemToObject(json,"esp_peripherals",peripherals)) {
        cJSON_Delete(peripherals); ok = false;
    }
    AudioMonitor_Status stream; AudioMonitor_GetStatus(&stream);
    cJSON *audio_stream=cJSON_AddObjectToObject(json,"audio_stream");
    if (audio_stream) {
        BOOL(audio_stream,"started",stream.started); BOOL(audio_stream,"enabled",stream.enabled);
        BOOL(audio_stream,"codec_ready",stream.codec_ready); STR(audio_stream,"state",stream.state);
        STR(audio_stream,"transport",stream.transport);
        NUM(audio_stream,"lease_remaining_ms",stream.lease_remaining_ms);
        NUM(audio_stream,"packets",stream.packets); NUM(audio_stream,"samples",stream.samples);
        NUM(audio_stream,"failed_packets",stream.failed_packets);
        NUM(audio_stream,"buffered_samples",stream.buffered_samples);
        NUM(audio_stream,"dropped_samples",stream.dropped_samples);
        NUM(audio_stream,"newest_age_ms",stream.newest_age_ms);
        NUM(audio_stream,"http_status",stream.http_status); NUM(audio_stream,"error",stream.error);
    } else { ok=false; }
    I2cDiagnostics_Status scan; I2cDiagnostics_GetStatus(&scan);
    cJSON *sj=cJSON_AddObjectToObject(json,"i2c_scan");
    if (sj) {
        STR(sj,"id",scan.id); STR(sj,"state",scan.state);
        BOOL(sj,"running",scan.running); BOOL(sj,"completed",scan.completed);
        NUM(sj,"sequence",scan.sequence); NUM(sj,"started_ms",scan.started_ms);
        NUM(sj,"completed_ms",scan.completed_ms); NUM(sj,"count",scan.count);
        NUM(sj,"scanned",scan.scanned); NUM(sj,"nack_count",scan.nack_count);
        NUM(sj,"timeout_count",scan.timeout_count); NUM(sj,"error_count",scan.error_count);
        NUM(sj,"sda",scan.sda); NUM(sj,"scl",scan.scl); NUM(sj,"error",scan.error);
        NUM(sj,"last_probe_error",scan.last_probe_error); NUM(sj,"last_address",scan.last_address);
        BOOL(sj,"codec_ack",scan.codec_ack); BOOL(sj,"codec_alternate_ack",scan.codec_alternate_ack);
        STR(sj,"codec_test_method","REGISTER_READ"); NUM(sj,"codec_test_hz",I2C_DIAGNOSTICS_CODEC_HZ);
        NUM(sj,"codec_test_register",I2C_DIAGNOSTICS_CODEC_REGISTER); NUM(sj,"probe_hz",100000);
        BOOL(sj,"codec_read_attempted",scan.codec_read_attempted); BOOL(sj,"codec_alternate_read_attempted",scan.codec_alternate_read_attempted);
        NUM(sj,"codec_read_error",scan.codec_read_error); NUM(sj,"codec_alternate_read_error",scan.codec_alternate_read_error);
        NUM(sj,"codec_reg00",scan.codec_reg00); NUM(sj,"codec_alternate_reg00",scan.codec_alternate_reg00);
        NUM(sj,"codec_unconfirmed_count",scan.codec_unconfirmed_count);
        BOOL(sj,"audio_restart_required",scan.audio_restart_required);
        cJSON *addresses=cJSON_AddArrayToObject(sj,"addresses");
        if (!addresses) ok=false;
        for (unsigned i=0; addresses && i<scan.count && i<sizeof(scan.addresses); ++i) {
            cJSON *item=cJSON_CreateNumber(scan.addresses[i]);
            if (!item || !cJSON_AddItemToArray(addresses,item)) { cJSON_Delete(item); ok=false; }
        }
    } else { ok=false; }
#undef STR
#undef NUM
#undef BOOL
    char *body = ok ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    return body;
}
void UdpTelemetry_RequestSerialStatus(void)
{
    portENTER_CRITICAL(&state_lock); serial_status_pending=true; portEXIT_CRITICAL(&state_lock);
}
char *UdpTelemetry_TakeSerialStatus(void)
{
    TickType_t now=xTaskGetTickCount();
    portENTER_CRITICAL(&state_lock);
    bool take=serial_status_pending && identity_ready &&
        (serial_status_at==0 || (TickType_t)(now-serial_status_at)>=pdMS_TO_TICKS(1000));
    if (take) { serial_status_pending=false; serial_status_at=now; }
    portEXIT_CRITICAL(&state_lock);
    return take ? make_payload(next_sequence(),true) : NULL;
}
static char *api_snapshot(void) { return make_payload(next_sequence(), false); }
bool UdpTelemetry_QueueWifi(const char *id, const char *action, const char *ssid,
                            const char *password, uint32_t deadline_ms, const char *boot_prefix)
{
    return UdpTelemetry_QueueWifiPaired(id, action, ssid, password, deadline_ms, boot_prefix, NULL);
}
bool UdpTelemetry_QueueWifiPaired(const char *id, const char *action, const char *ssid,
                                  const char *password, uint32_t deadline_ms,
                                  const char *boot_prefix, const char *api_token)
{
    portENTER_CRITICAL(&state_lock); bool ready = identity_ready; portEXIT_CRITICAL(&state_lock);
    return ready && BoardWifi_QueuePaired(id, action, ssid, password, deadline_ms, boot_prefix, api_token);
}
bool UdpTelemetry_QueueSound(const SoundCommand *c)
{
    if (!c || !ChargeCommand_IdValid(c->id) || !sound_dispatch || xSemaphoreTake(sound_dispatch,pdMS_TO_TICKS(50))!=pdTRUE) return false;
    uint32_t now_ms=(uint32_t)(esp_timer_get_time()/1000);
    int32_t remaining=(int32_t)(c->deadline_ms-now_ms);
    portENTER_CRITICAL(&state_lock);
    bool same_boot=identity_ready && !strncmp(boot_id,c->boot_prefix,8);
    portEXIT_CRITICAL(&state_lock);
    if (!same_boot || remaining<=0 || remaining>5000 || command_seen_or_mark(c->id)) { xSemaphoreGive(sound_dispatch); return false; }
    unsigned target=(c->action==SOUND_BUZZER || c->action==SOUND_BUZZER_STOP)?1:0;
    portENTER_CRITICAL(&state_lock);
    snprintf(sound_reply[target].id,17,"%s",c->id);
    strcpy(sound_reply[target].status,"QUEUED"); sound_reply[target].error=ESP_OK;
    sound_reply[target].expires_ms=c->deadline_ms+4000U;
    portEXIT_CRITICAL(&state_lock);
    esp_err_t error=ESP_ERR_INVALID_ARG;
    if (c->action==SOUND_SPEAKER) error=Audio_PlayToneWithId(c->hz,c->duration_ms,c->volume_percent,c->id);
    else if (c->action==SOUND_SPEAKER_STOP) error=Audio_StopToneWithId(c->id);
    else {
        char line[64];
        portENTER_CRITICAL(&state_lock);
        const char *cap=strstr(telemetry.peripheral," buzzer_proto=1");
        bool identified=telemetry.has_peripheral && (TickType_t)(xTaskGetTickCount()-telemetry.last_peripheral)<pdMS_TO_TICKS(3000) &&
            cap && (cap[15]==' ' || cap[15]=='\r' || cap[15]=='\n' || cap[15]==0);
        portEXIT_CRITICAL(&state_lock);
        unsigned duration=(unsigned)(c->action==SOUND_BUZZER?c->duration_ms:0);
        int length=identified ? snprintf(line,sizeof(line),"PERIPH BEEP %u %s\n",duration,c->id) :
            snprintf(line,sizeof(line),"PERIPH BEEP %u\n",duration);
        error=UartBridge_QueueCommand(line,(size_t)length,xTaskGetTickCount()+pdMS_TO_TICKS((uint32_t)remaining))?ESP_OK:ESP_ERR_TIMEOUT;
        if (!identified && error==ESP_OK) {
            portENTER_CRITICAL(&state_lock);
            if (!strcmp(sound_reply[target].id,c->id)) strcpy(sound_reply[target].status,"UNCONFIRMED");
            portEXIT_CRITICAL(&state_lock);
        }
    }
    if (error!=ESP_OK) {
        portENTER_CRITICAL(&state_lock);
        if (!strcmp(sound_reply[target].id,c->id)) { strcpy(sound_reply[target].status,"FAILED"); sound_reply[target].error=error; }
        portEXIT_CRITICAL(&state_lock);
    }
    UdpTelemetry_RequestSerialStatus();
    xSemaphoreGive(sound_dispatch);
    return error==ESP_OK;
}

bool UdpTelemetry_QueueSerialCurrent(const char *id, unsigned ma, uint32_t deadline_ms,
                                     const char *boot_prefix)
{
    if (!ChargeCommand_IdValid(id) || !ChargeCommand_Allowed(ma) || !boot_prefix || strlen(boot_prefix)!=8) return false;
    portENTER_CRITICAL(&state_lock);
    bool same_boot=identity_ready && !strncmp(boot_id,boot_prefix,8);
    portEXIT_CRITICAL(&state_lock);
    TickType_t basis=xTaskGetTickCount();
    uint32_t now_ms=(uint32_t)(esp_timer_get_time()/1000);
    int32_t remaining=(int32_t)(deadline_ms-now_ms);
    if (!same_boot || remaining<=0 || remaining>15000 || command_seen_or_mark(id)) return false;
    char command_line[80];
    size_t length=ChargeCommand_Format(command_line,sizeof(command_line),ma,id);
    TickType_t deadline=basis+pdMS_TO_TICKS((uint32_t)remaining);
    portENTER_CRITICAL(&state_lock);
    memset(&charge_command,0,sizeof(charge_command));
    snprintf(charge_command.id,sizeof(charge_command.id),"%s",id);
    charge_command.ma=ma; charge_command.deadline=deadline; charge_command.active=true;
    portEXIT_CRITICAL(&state_lock);
    bool queued=length && UartBridge_QueueCommand(command_line,length,deadline);
    if (!queued) {
        portENTER_CRITICAL(&state_lock);
        snprintf(charge_command.status,sizeof(charge_command.status),"FAILED");
        snprintf(charge_command.detail,sizeof(charge_command.detail),"UART_BUSY");
        charge_command.acknowledged=true;
        portEXIT_CRITICAL(&state_lock);
    }
    UdpTelemetry_RequestSerialStatus();
    return queued;
}

/* No HTTP listener is created on ESP32. Each task owns its outbound client.
 * Redirects are disabled so the LAN ingest token cannot follow another host. */
static esp_err_t response_event(esp_http_client_event_t *event)
{
    UploadResponse *response=event->user_data;
    if (event->event_id==HTTP_EVENT_ON_DATA && response && event->data_len>0) {
        size_t count=(size_t)event->data_len;
        if (response->used+count>=sizeof(response->body)) { response->overflow=true; }
        else if (!response->overflow) {
            memcpy(response->body+response->used,event->data,count);
            response->used+=count; response->body[response->used]='\0';
        }
    }
    return ESP_OK;
}
static esp_http_client_handle_t make_client(const char *path, const char *type, UploadResponse *response)
{
    char url[160];
    if (snprintf(url,sizeof(url),"%s%s",ROBOT_SERVER_URL,path) >= sizeof(url)) { return NULL; }
    esp_http_client_config_t config = { .url=url, .method=HTTP_METHOD_POST,
        .timeout_ms=1800, .disable_auto_redirect=true, .keep_alive_enable=true,
        .buffer_size=512, .buffer_size_tx=1024,
        .event_handler=response_event, .user_data=response };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { return NULL; }
    if (esp_http_client_set_header(client,"Authorization","Bearer " ROBOT_INGEST_TOKEN) != ESP_OK ||
        esp_http_client_set_header(client,"Content-Type",type) != ESP_OK ||
        esp_http_client_set_header(client,"X-Device-Id",device_id) != ESP_OK ||
        esp_http_client_set_header(client,"X-Boot-Id",boot_id) != ESP_OK) {
        esp_http_client_cleanup(client); return NULL;
    }
    return client;
}
static bool post_body(esp_http_client_handle_t client, const void *data, size_t length,
                      const char *content_type, int *status, UploadResponse *response)
{
    *status = 0;
    if (response) { memset(response,0,sizeof(*response)); }
    if (!client || !length || length > 512U*1024U) { return false; }
    /* IDF clears Content-Type when clearing post_data after a request.
     * Restore it on EVERY reuse, or the next request becomes form-urlencoded. */
    esp_err_t err = esp_http_client_set_header(client,"Content-Type",content_type);
    if (err == ESP_OK) { err = esp_http_client_set_post_field(client,data,(int)length); }
    if (err == ESP_OK) { err = esp_http_client_perform(client); }
    if (err == ESP_OK) { *status = esp_http_client_get_status_code(client); }
    bool success = err == ESP_OK && *status >= 200 && *status < 300;
    /* Never leave a pointer to a returned framebuffer / freed JSON cached. */
    esp_http_client_set_post_field(client,NULL,0);
    if (!success) { esp_http_client_close(client); }
    return success;
}

static void accept_command(const UploadResponse *response, TickType_t request_started)
{
    if (response->overflow || !response->used) return;
    cJSON *json=cJSON_ParseWithLength(response->body,response->used);
    if (!json) return;
    const cJSON *audio=cJSON_GetObjectItemCaseSensitive(json,"audio_monitor");
    const cJSON *enabled=cJSON_GetObjectItemCaseSensitive(audio,"enabled");
    const cJSON *session=cJSON_GetObjectItemCaseSensitive(audio,"session_id");
    const cJSON *audio_ttl=cJSON_GetObjectItemCaseSensitive(audio,"expires_in_ms");
    uint32_t elapsed=(uint32_t)((TickType_t)(xTaskGetTickCount()-request_started)*portTICK_PERIOD_MS);
    const cJSON *camera_check=cJSON_GetObjectItemCaseSensitive(json,"camera_check");
    const cJSON *camera_id=cJSON_GetObjectItemCaseSensitive(camera_check,"id");
    const cJSON *camera_ttl=cJSON_GetObjectItemCaseSensitive(camera_check,"expires_in_ms");
    if (cJSON_IsString(camera_id) && cJSON_IsNumber(camera_ttl) &&
        isfinite(camera_ttl->valuedouble) && camera_ttl->valuedouble>elapsed &&
        camera_ttl->valuedouble<=30000 && camera_ttl->valuedouble==(unsigned)camera_ttl->valuedouble) {
        (void)Camera_RequestCheck(camera_id->valuestring);
    }
    const cJSON *scan=cJSON_GetObjectItemCaseSensitive(json,"i2c_scan");
    const cJSON *scan_id=cJSON_GetObjectItemCaseSensitive(scan,"id");
    const cJSON *scan_ttl=cJSON_GetObjectItemCaseSensitive(scan,"expires_in_ms");
    if (cJSON_IsString(scan_id) && cJSON_IsNumber(scan_ttl) &&
        isfinite(scan_ttl->valuedouble) && scan_ttl->valuedouble>elapsed &&
        scan_ttl->valuedouble<=15000 && scan_ttl->valuedouble==(unsigned)scan_ttl->valuedouble) {
        (void)I2cDiagnostics_RequestScan(scan_id->valuestring);
    }
    if (cJSON_IsTrue(enabled) && cJSON_IsString(session) &&
        ChargeCommand_IdValid(session->valuestring) && cJSON_IsNumber(audio_ttl) &&
        isfinite(audio_ttl->valuedouble) && audio_ttl->valuedouble>elapsed &&
        audio_ttl->valuedouble<=15000 && audio_ttl->valuedouble==(unsigned)audio_ttl->valuedouble) {
        (void)AudioMonitor_SetLease(session->valuestring,(unsigned)audio_ttl->valuedouble-elapsed);
    } else {
        AudioMonitor_Stop();
    }
    const cJSON *command=cJSON_GetObjectItemCaseSensitive(json,"command");
    const cJSON *id=cJSON_GetObjectItemCaseSensitive(command,"id");
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(command,"type");
    const cJSON *ma=cJSON_GetObjectItemCaseSensitive(command,"ma");
    const cJSON *ttl=cJSON_GetObjectItemCaseSensitive(command,"expires_in_ms");
    if (!cJSON_IsString(id) || !ChargeCommand_IdValid(id->valuestring) ||
        !cJSON_IsString(type) || strcmp(type->valuestring,"charge_current") ||
        !cJSON_IsNumber(ma) || !isfinite(ma->valuedouble) || ma->valuedouble<0 ||
        ma->valuedouble>4350 || ma->valuedouble!=(unsigned)ma->valuedouble ||
        !ChargeCommand_Allowed((unsigned)ma->valuedouble) ||
        !cJSON_IsNumber(ttl) || !isfinite(ttl->valuedouble) || ttl->valuedouble<=elapsed ||
        ttl->valuedouble>15000 || ttl->valuedouble!=(unsigned)ttl->valuedouble) {
        cJSON_Delete(json); return;
    }
    if (command_seen_or_mark(id->valuestring)) {
        cJSON_Delete(json); return;
    }
    char command_line[80];
    unsigned target=(unsigned)ma->valuedouble;
    size_t length=ChargeCommand_Format(command_line,sizeof(command_line),target,id->valuestring);
    TickType_t deadline=xTaskGetTickCount()+pdMS_TO_TICKS((unsigned)ttl->valuedouble-elapsed);
    portENTER_CRITICAL(&state_lock);
    memset(&charge_command,0,sizeof(charge_command));
    snprintf(charge_command.id,sizeof(charge_command.id),"%s",id->valuestring);
    charge_command.ma=target; charge_command.deadline=deadline; charge_command.active=true;
    portEXIT_CRITICAL(&state_lock);
    if (!length || !UartBridge_QueueCommand(command_line,length,deadline)) {
        portENTER_CRITICAL(&state_lock);
        snprintf(charge_command.status,sizeof(charge_command.status),"FAILED");
        snprintf(charge_command.detail,sizeof(charge_command.detail),"UART_BUSY");
        charge_command.acknowledged=true;
        portEXIT_CRITICAL(&state_lock);
    }
    cJSON_Delete(json);
}

static void frame_task(void *arg)
{
    (void)arg;
    esp_http_client_handle_t client = NULL;
    for (;;) {
        NetworkState state = state_copy();
        Camera_Status camera; Camera_GetStatus(&camera);
        if (state.connected && state.upload_ready && camera.ready) {
            if (!client) { client = make_client("/api/frame","image/jpeg",NULL); }
            Camera_Frame frame = {0};
            if (client && Camera_AcquireFrame(&frame,250) == ESP_OK) {
                int status;
                bool sent = post_body(client,frame.data,frame.length,"image/jpeg",&status,NULL);
                Camera_ReleaseFrame(&frame,sent);
                portENTER_CRITICAL(&state_lock);
                if (sent) { ++network.frames_sent; } else { ++network.frame_errors; }
                portEXIT_CRITICAL(&state_lock);
            }
        } else if (client) { esp_http_client_cleanup(client); client = NULL; }
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}
static void upload_task(void *argument)
{
    (void)argument;
    esp_http_client_handle_t client = NULL;
    TickType_t next_upload = 0;
    uint32_t failures = 0;
    UploadResponse response;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        NetworkState state = state_copy();
        if (state.connected && (int32_t)(now-next_upload) >= 0) {
            if (!client) { client=make_client("/api/telemetry","application/json",&response); }
            char *body=make_payload(next_sequence(),false);
            int status=0;
            TickType_t requested_at=xTaskGetTickCount();
            bool sent=body && post_body(client,body,strlen(body),"application/json",&status,&response);
            cJSON_free(body);
            portENTER_CRITICAL(&state_lock);
            if (sent) { ++network.sent; } else { ++network.send_errors; }
            network.http_status=status;
            network.upload_ready=sent && network.connected && network.generation==state.generation;
            portEXIT_CRITICAL(&state_lock);
            NetworkState after=state_copy();
            if (sent && after.connected && after.generation==state.generation) {
                accept_command(&response,requested_at);
            }
            failures=sent ? 0 : failures < 3U ? failures+1U : 3U;
            next_upload=xTaskGetTickCount()+pdMS_TO_TICKS(1000U << failures);
        }
        if (!state.connected && client) { esp_http_client_cleanup(client); client=NULL; }
        vTaskDelay(pdMS_TO_TICKS(50U));
    }
}
esp_err_t UdpTelemetry_Start(void)
{
    if (startup_called) { return ESP_ERR_INVALID_STATE; }
    startup_called=1;
    sound_dispatch=xSemaphoreCreateMutex();
    if (!sound_dispatch) return ESP_ERR_NO_MEM;
    uint8_t mac[6], random[16];
    esp_err_t err=esp_read_mac(mac,ESP_MAC_WIFI_STA);
    if (err != ESP_OK) { return err; }
    snprintf(device_id,sizeof(device_id),"dog-%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    esp_fill_random(random,sizeof(random));
    for (unsigned i=0;i<sizeof(random);++i) { snprintf(boot_id+i*2,3,"%02x",random[i]); }
    portENTER_CRITICAL(&state_lock); identity_ready=true; portEXIT_CRITICAL(&state_lock);
    (void)AudioMonitor_Start(device_id,boot_id);
    err = BoardWifi_Start(device_id, boot_id, api_snapshot,
                          ROBOT_NETWORK_ENABLED ? ROBOT_WIFI_SSID : NULL,
                          ROBOT_NETWORK_ENABLED ? ROBOT_WIFI_PASSWORD : NULL);
    if (err != ESP_OK) return err;
    if (!ROBOT_NETWORK_ENABLED) {
        portENTER_CRITICAL(&state_lock); network.state=NET_OFF; portEXIT_CRITICAL(&state_lock);
        return ESP_OK;
    }
    portENTER_CRITICAL(&state_lock); network.state=NET_INIT; portEXIT_CRITICAL(&state_lock);
    if (xTaskCreate(upload_task,"ubuntu_upload",8192,NULL,5,NULL) != pdPASS) {
        portENTER_CRITICAL(&state_lock); network.state=NET_ERROR; network.error=ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&state_lock); return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(frame_task,"camera_upload",6144,NULL,3,NULL) != pdPASS) {
        portENTER_CRITICAL(&state_lock); ++network.frame_errors; portEXIT_CRITICAL(&state_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
