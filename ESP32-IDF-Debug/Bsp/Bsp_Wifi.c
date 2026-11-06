#include "Bsp_Wifi.h"
#include "Bsp_WifiPolicy.h"
#include "Bsp_Camera.h"
#include "Bsp_Display.h"
#include "Bsp_UartBridge.h"
#include "Bsp_UdpTelemetry.h"
#include "Bsp_AudioMonitor.h"
#include "Bsp_I2cDiagnostics.h"
#include "UartBridgePolicy.h"
#include "WifiControlPolicy.h"
#include "WifiSessionAuth.h"
#include "WifiStoredProfile.h"
#include "BuildConfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "lwip/sockets.h"
#include <netinet/tcp.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define EV_IP BIT0
#define EV_DOWN BIT1
#define EV_STOP BIT2
#define WIFI_ATTEMPT_MS 30000
#define WIFI_API_PORT 81
#define WIFI_STREAM_PORT 82
/* Match the PC's bounded JPEG receiver, including 1080p frames. */
#define WIFI_FRAME_MAX_BYTES (512U * 1024U)
#define WIFI_STREAM_HEADER_BYTES 384U
typedef struct { char ssid[33], password[65]; } Credentials;
typedef struct {
    char id[17], action[12], boot[9];
    Credentials credentials;
    char api_token[WIFI_SESSION_TOKEN_LENGTH + 1];
    uint32_t deadline;
} WifiCommand;
static portMUX_TYPE wifi_lock = portMUX_INITIALIZER_UNLOCKED;
static BoardWifi_Status status = { .state = "OFF" };
static BoardWifi_ScanStatus scan_status;
static WifiCommand pending;
static bool pending_valid, available;
static char seen[64][17];
static unsigned seen_next;
static char device_id[24], boot_id[33];
static char *(*make_snapshot)(void);
static Credentials initial;
static EventGroupHandle_t events;
static esp_netif_t *station;
static httpd_handle_t api, stream_api;
/* Only the single stream HTTP worker accesses this send budget. */
static int64_t stream_send_deadline_us;
static bool driver_started;
/* Public images receive this key over USB, then restore the paired NVS blob. */
#if ROBOT_PUBLIC_TEST_BUILD
static char session_api_token[WIFI_SESSION_TOKEN_LENGTH + 1];
#endif
/* The single API worker owns this bounded replay ledger. Even rejected or
 * expired admitted commands are not retried with a freshly extended deadline. */
static char control_seen[256][17];
static unsigned control_next;

static void wipe(void *buffer, size_t length)
{
    volatile unsigned char *p = buffer;
    while (length--) *p++ = 0;
}
static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static bool hex_string(const char *value, size_t length)
{
    if (!value || strnlen(value, length + 1) != length) return false;
    for (size_t i = 0; i < length; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}
static bool credentials_valid(const char *ssid, const char *password)
{
    if (!ssid || !password) return false;
    size_t s = strnlen(ssid, 33), p = strnlen(password, 65);
    if (!s || s > 32 || p > 64 || (p && p < 8)) return false;
    if (p == 64) {
        for (size_t i = 0; i < p; ++i)
            if (!((password[i] >= '0' && password[i] <= '9') ||
                  (password[i] >= 'a' && password[i] <= 'f') ||
                  (password[i] >= 'A' && password[i] <= 'F'))) return false;
    }
    return true;
}
static bool fresh(const WifiCommand *command)
{
    return WifiPolicy_Deadline(millis(), command->deadline, boot_id, command->boot);
}
void BoardWifi_GetStatus(BoardWifi_Status *out)
{
    if (!out) return;
    portENTER_CRITICAL(&wifi_lock); *out = status; portEXIT_CRITICAL(&wifi_lock);
}
void BoardWifi_GetScanStatus(BoardWifi_ScanStatus *out)
{
    if (!out) return;
    portENTER_CRITICAL(&wifi_lock); *out = scan_status; portEXIT_CRITICAL(&wifi_lock);
}
static void request_result(const char *id, const char *result, int error)
{
    portENTER_CRITICAL(&wifi_lock);
    if (!strcmp(id, status.request_id)) {
        snprintf(status.request_status, sizeof(status.request_status), "%s", result);
        status.error = error;
    }
    portEXIT_CRITICAL(&wifi_lock);
}
static void link_state(const char *state, int error)
{
    portENTER_CRITICAL(&wifi_lock);
    snprintf(status.state, sizeof(status.state), "%s", state);
    status.error = error;
    portEXIT_CRITICAL(&wifi_lock);
}
static void revoke_link(void)
{
    portENTER_CRITICAL(&wifi_lock);
    status.ready = status.api_ready = false;
    status.ip_address = 0; status.ip[0] = 0; status.rssi = 0;
    if (strcmp(status.state, "OFF")) snprintf(status.state, sizeof(status.state), "RETRY");
    ++status.generation;
    portEXIT_CRITICAL(&wifi_lock);
}
static void wifi_event(void *unused, esp_event_base_t base, int32_t id, void *data)
{
    (void)unused;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        revoke_link();
        xEventGroupSetBits(events, EV_STOP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        revoke_link();
        const wifi_event_sta_disconnected_t *event = data;
        portENTER_CRITICAL(&wifi_lock);
        status.disconnect_reason = event->reason; ++status.disconnects;
        portEXIT_CRITICAL(&wifi_lock);
        xEventGroupSetBits(events, EV_DOWN);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        revoke_link(); xEventGroupSetBits(events, EV_DOWN);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(events, EV_IP);
    }
}
#if ROBOT_PUBLIC_TEST_BUILD
static esp_err_t load_paired_profile(WifiStoredProfile *profile)
{
    nvs_handle_t handle;size_t length=sizeof(*profile);
    memset(profile,0,sizeof(*profile));
    esp_err_t error=nvs_open("robot_wifi",NVS_READONLY,&handle);
    if(error==ESP_OK){error=nvs_get_blob(handle,"paired_v1",profile,&length);nvs_close(handle);}
    if(error==ESP_OK && (length!=sizeof(*profile) || !WifiStoredProfile_Valid(profile)))error=ESP_ERR_INVALID_RESPONSE;
    if(error!=ESP_OK)wipe(profile,sizeof(*profile));
    return error;
}
#endif
static esp_err_t load_credentials(Credentials *credentials)
{
#if ROBOT_PUBLIC_TEST_BUILD
    WifiStoredProfile paired;
    if(load_paired_profile(&paired)==ESP_OK) {
        memcpy(credentials->ssid,paired.ssid,sizeof(credentials->ssid));
        memcpy(credentials->password,paired.password,sizeof(credentials->password));
        wipe(&paired,sizeof(paired));return ESP_OK;
    }
#endif
    nvs_handle_t handle;
    esp_err_t error = nvs_open("robot_wifi", NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    size_t length = sizeof(*credentials);
    memset(credentials, 0, sizeof(*credentials));
    error = nvs_get_blob(handle, "credentials", credentials, &length);
    nvs_close(handle);
    if (error == ESP_OK && (length != sizeof(*credentials) ||
        !credentials_valid(credentials->ssid, credentials->password))) error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) wipe(credentials, sizeof(*credentials));
    return error;
}
static esp_err_t save_credentials(const Credentials *credentials)
{
#if ROBOT_PUBLIC_TEST_BUILD
    WifiStoredProfile profile={.version=WIFI_STORED_PROFILE_VERSION},old;
    memcpy(profile.ssid,credentials->ssid,sizeof(profile.ssid));
    memcpy(profile.password,credentials->password,sizeof(profile.password));
    portENTER_CRITICAL(&wifi_lock);memcpy(profile.token,session_api_token,sizeof(profile.token));portEXIT_CRITICAL(&wifi_lock);
    if(!WifiStoredProfile_Valid(&profile)){wipe(&profile,sizeof(profile));return ESP_ERR_INVALID_STATE;}
    esp_err_t error=load_paired_profile(&old);
    if(error!=ESP_OK || memcmp(&old,&profile,sizeof(profile))) {
        nvs_handle_t handle;error=nvs_open("robot_wifi",NVS_READWRITE,&handle);
        if(error==ESP_OK){error=nvs_set_blob(handle,"paired_v1",&profile,sizeof(profile));if(error==ESP_OK)error=nvs_commit(handle);nvs_close(handle);}
        if(error==ESP_OK)error=load_paired_profile(&old);
        if(error==ESP_OK && memcmp(&old,&profile,sizeof(profile)))error=ESP_ERR_INVALID_RESPONSE;
    }
    wipe(&profile,sizeof(profile));wipe(&old,sizeof(old));return error;
#else
    nvs_handle_t handle;
    esp_err_t error = nvs_open("robot_wifi", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    /* One atomic blob prevents mixed SSID/password after an interrupted update. */
    error = nvs_set_blob(handle, "credentials", credentials, sizeof(*credentials));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
#endif
}
static bool authorized(httpd_req_t *request)
{
#if ROBOT_PUBLIC_TEST_BUILD
    char received[WIFI_SESSION_HEADER_CAPACITY] = {0};
    char token[sizeof(session_api_token)];
    portENTER_CRITICAL(&wifi_lock);
    memcpy(token, session_api_token, sizeof(token));
    portEXIT_CRITICAL(&wifi_lock);
    size_t length = httpd_req_get_hdr_value_len(request, "Authorization");
    bool read = length < sizeof(received) &&
        httpd_req_get_hdr_value_str(request, "Authorization", received, sizeof(received)) == ESP_OK;
    bool equal = WifiSessionAuth_Authorized(token, (const unsigned char *)received, length, read);
    wipe(received, sizeof(received));
    wipe(token, sizeof(token));
    return equal;
#else
    static const char expected[] = "Bearer " ROBOT_INGEST_TOKEN;
    char received[sizeof(expected)] = {0};
    size_t length = httpd_req_get_hdr_value_len(request, "Authorization");
    bool read = length < sizeof(received) &&
        httpd_req_get_hdr_value_str(request, "Authorization", received, sizeof(received)) == ESP_OK;
    bool equal = WifiPolicy_SecretEqual((const unsigned char *)expected, sizeof(expected)-1,
                                        (const unsigned char *)received, length, read);
    wipe(received, sizeof(received));
    return equal && sizeof(ROBOT_INGEST_TOKEN) > 1;
#endif
}
static esp_err_t reject(httpd_req_t *request, const char *code, const char *message)
{
    httpd_resp_set_status(request, code);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, message, HTTPD_RESP_USE_STRLEN);
}
static bool api_allowed(httpd_req_t *request, uint32_t *generation)
{
    if (!authorized(request)) {
        (void)reject(request, "401 Unauthorized", "{\"error\":\"Unauthorized\"}"); return false;
    }
    BoardWifi_Status current; BoardWifi_GetStatus(&current);
    if (!current.ready || !current.api_ready) {
        (void)reject(request, "503 Service Unavailable", "{\"error\":\"WiFi unavailable\"}"); return false;
    }
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Device-Id", device_id);
    httpd_resp_set_hdr(request, "X-Boot-Id", boot_id);
    *generation = current.generation;
    return true;
}
static bool same_connection(uint32_t generation)
{
    BoardWifi_Status current; BoardWifi_GetStatus(&current);
    return current.ready && current.api_ready && current.generation == generation;
}
static esp_err_t status_handler(httpd_req_t *request)
{
    uint32_t generation;
    if (!api_allowed(request, &generation)) return ESP_OK;
    char *body = make_snapshot ? make_snapshot() : NULL;
    if (!body) return reject(request, "503 Service Unavailable", "{\"error\":\"Snapshot unavailable\"}");
    if (!same_connection(generation)) {
        cJSON_free(body);
        return reject(request, "503 Service Unavailable", "{\"error\":\"WiFi changed\"}");
    }
    char generation_text[16];
    snprintf(generation_text, sizeof(generation_text), "%lu", (unsigned long)generation);
    httpd_resp_set_hdr(request, "X-Wifi-Generation", generation_text);
    httpd_resp_set_type(request, "application/json");
    esp_err_t error = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return error;
}
static bool execute_control(const char *text, uint32_t deadline)
{
    size_t length = strlen(text);
    if (!length || length > 287) return false;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)text[i] < 32 || (unsigned char)text[i] > 126) return false;
    /* Bounded service calibration, under the same authenticated boot/deadline
     * and replay checks as other controls. No arbitrary register writes. */
    if (!strcmp(text, "ESP CAM_COLOR READ"))
        return WifiControl_Deadline(millis(), deadline) && Camera_ReadColor() == ESP_OK;
    if(!strcmp(text,"ESP CAM_LENC BEGIN") || !strcmp(text,"ESP CAM_LENC END"))
        return WifiControl_Deadline(millis(),deadline) && Camera_CalibrationMode(!strcmp(text,"ESP CAM_LENC BEGIN"))==ESP_OK;
    if(!strcmp(text,"ESP FACE SHOW") || !strcmp(text,"ESP FACE DEBUG"))
        return WifiControl_Deadline(millis(),deadline) && Display_RequestFace(!strcmp(text,"ESP FACE SHOW"))==ESP_OK;
    if (!strcmp(text,"ESP CAM_LENC SET 0") || !strcmp(text,"ESP CAM_LENC SET 1"))
        return WifiControl_Deadline(millis(),deadline) && Camera_SetLensCorrection(text[17]=='1')==ESP_OK;
    if(!strcmp(text,"ESP CAM_LENC QSET 0") || !strcmp(text,"ESP CAM_LENC QSET 1") ||
       !strcmp(text,"ESP CAM_LENC QSAVE 0") || !strcmp(text,"ESP CAM_LENC QSAVE 1"))
        return WifiControl_Deadline(millis(),deadline) && Camera_SetLensAutoQ(text[length-1]=='1',length==20U)==ESP_OK;
    if (length==19U && (!strncmp(text,"ESP CAM_LENC EDGE ",18) || !strncmp(text,"ESP CAM_LENC SAVE ",18)) &&
        text[18]>='0' && text[18]<='3')
        return WifiControl_Deadline(millis(),deadline) && Camera_SetLensEdge((unsigned)(text[18]-'0'),text[13]=='S')==ESP_OK;
    if(length==70U && (!strncmp(text,"ESP CAM_LENC FULL ",18) || !strncmp(text,"ESP CAM_LENC KEEP ",18)) && hex_string(text+18,52)) {
        uint8_t table[26];
        for(unsigned i=0;i<26;++i){char pair[3]={text[18+i*2],text[19+i*2],0};table[i]=(uint8_t)strtoul(pair,NULL,16);}
        return WifiControl_Deadline(millis(),deadline) && Camera_SetLensFullTable(table,text[13]=='K')==ESP_OK;
    }
    if(length==91U && (!strncmp(text,"ESP CAM_LENC GREEN ",19) || !strncmp(text,"ESP CAM_LENC GSAVE ",19)) && hex_string(text+19,72)) {
        uint8_t table[36];
        for(unsigned i=0;i<36;++i) {
            const char *hex="0123456789abcdef";
            table[i]=(uint8_t)(((strchr(hex,text[19+i*2])-hex)<<4)|(strchr(hex,text[20+i*2])-hex));
        }
        return WifiControl_Deadline(millis(),deadline) && Camera_SetLensGreenTable(table,text[13]=='G' && text[14]=='S')==ESP_OK;
    }
    if(length==69U && (!strncmp(text,"ESP CAM_LENC TABLE ",19) || !strncmp(text,"ESP CAM_LENC STORE ",19)) && hex_string(text+19,50)) {
        uint8_t table[25];
        for(unsigned i=0;i<25;++i) {
            char pair[3]={text[19+i*2],text[20+i*2],0};
            table[i]=(uint8_t)strtoul(pair,NULL,16);
        }
        return WifiControl_Deadline(millis(),deadline) && Camera_SetLensTable(table,text[13]=='S')==ESP_OK;
    }
    if (!strncmp(text, "ESP CAM_COLOR SET ", 18)) {
        unsigned mode, red, green, blue;
        int consumed = 0;
        if (sscanf(text, "ESP CAM_COLOR SET %1u %4u %4u %4u%n",
                   &mode, &red, &green, &blue, &consumed) != 4 ||
            (size_t)consumed != length || mode > 3 ||
            red < 256 || red > 4095 || green < 256 || green > 4095 || blue < 256 || blue > 4095)
            return false;
        char canonical[48];
        snprintf(canonical, sizeof(canonical), "ESP CAM_COLOR SET %u %u %u %u", mode, red, green, blue);
        if (strcmp(text, canonical)) return false;
        return WifiControl_Deadline(millis(), deadline) &&
            Camera_SetColor((uint8_t)mode, (uint16_t)red, (uint16_t)green, (uint16_t)blue) == ESP_OK;
    }
    if (WifiControl_PeripheralLine(text)) {
        char command[80];
        if (length + 1 >= sizeof(command)) return false;
        memcpy(command, text, length); command[length++] = '\n';
        uint32_t now = millis();
        if (!WifiControl_Deadline(now, deadline)) return false;
        return UartBridge_QueueCommand(command, length,
            xTaskGetTickCount() + pdMS_TO_TICKS(deadline - now));
    }
    if (strncmp(text, "ESP ", 4)) return false;
    char command[289], request_id[17];
    uint8_t ignored[292];
    UartBridgeLocalState parser;
    UartBridgeLocal_Reset(&parser);
    UartBridgeLocalAction action;
    UartBridgeLocalCharge charge;
    UartBridgeLocalWifi wifi;
    SoundCommand sound;
    uint32_t ttl;
    memcpy(command, text, length); command[length++] = '\n';
    size_t passthrough = UartBridgeLocal_FilterWithSound(&parser, (const uint8_t *)command, length,
        ignored, &action, request_id, &ttl, &charge, &wifi, &sound);
    bool accepted = false;
    if (!passthrough && WifiControl_Deadline(millis(), deadline)) {
        switch (action) {
        case UART_BRIDGE_LOCAL_SOUND: accepted = UdpTelemetry_QueueSound(&sound); break;
        case UART_BRIDGE_LOCAL_STATUS: accepted = true; break;
        case UART_BRIDGE_LOCAL_SCAN: accepted = I2cDiagnostics_RequestScan(request_id) == ESP_OK; break;
        case UART_BRIDGE_LOCAL_CAMERA_CHECK: accepted = Camera_RequestCheck(request_id) == ESP_OK; break;
        case UART_BRIDGE_LOCAL_AUDIO_START:
            accepted = AudioMonitor_SetPullLease(request_id, ttl) == ESP_OK; break;
        case UART_BRIDGE_LOCAL_AUDIO_STOP: AudioMonitor_StopPull(request_id); accepted = true; break;
        case UART_BRIDGE_LOCAL_CHARGE_CURRENT:
            accepted = UdpTelemetry_QueueSerialCurrent(request_id, charge.ma, charge.deadline_ms, charge.boot_prefix); break;
        case UART_BRIDGE_LOCAL_WIFI_CONNECT:
        case UART_BRIDGE_LOCAL_WIFI_RECONNECT:
        case UART_BRIDGE_LOCAL_WIFI_DISCONNECT:
        case UART_BRIDGE_LOCAL_WIFI_SCAN:
            if (wifi.api_token[0]) break; /* Pairing keys are accepted on UART only. */
            accepted = UdpTelemetry_QueueWifi(request_id,
                action == UART_BRIDGE_LOCAL_WIFI_CONNECT ? "connect" :
                action == UART_BRIDGE_LOCAL_WIFI_RECONNECT ? "reconnect" :
                action == UART_BRIDGE_LOCAL_WIFI_SCAN ? "scan" : "disconnect",
                wifi.ssid, wifi.password, wifi.deadline_ms, wifi.boot_prefix); break;
        default: break;
        }
    }
    wipe(command, sizeof(command)); wipe(ignored, sizeof(ignored));
    wipe(&wifi, sizeof(wifi)); UartBridgeLocal_Reset(&parser);
    return accepted;
}
static esp_err_t control_handler(httpd_req_t *request)
{
    uint32_t generation;
    if (!api_allowed(request, &generation)) return ESP_OK;
    char body[640];
    if (request->content_len < 1 || request->content_len >= sizeof(body))
        return reject(request, "400 Bad Request", "{\"error\":\"Invalid control length\"}");
    size_t used = 0;
    int64_t started = esp_timer_get_time();
    while (used < request->content_len) {
        int count = httpd_req_recv(request, body + used, request->content_len - used);
        if (count <= 0 || esp_timer_get_time() - started > 1000000) {
            wipe(body, sizeof(body));
            return reject(request, "408 Request Timeout", "{\"error\":\"Control receive timed out\"}");
        }
        used += (size_t)count;
    }
    body[used] = 0;
    cJSON *json = memchr(body, 0, used) || strstr(body, "\\u0000") ? NULL :
        cJSON_ParseWithLengthOpts(body, used + 1, NULL, true);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    cJSON *boot = cJSON_GetObjectItemCaseSensitive(json, "boot_id");
    cJSON *deadline = cJSON_GetObjectItemCaseSensitive(json, "deadline_ms");
    cJSON *line = cJSON_GetObjectItemCaseSensitive(json, "line");
    bool valid = cJSON_IsObject(json) && cJSON_GetArraySize(json) == 4 &&
        cJSON_IsString(id) && hex_string(id->valuestring, 16) &&
        cJSON_IsString(boot) && !strcmp(boot->valuestring, boot_id) &&
        cJSON_IsNumber(deadline) && isfinite(deadline->valuedouble) &&
        deadline->valuedouble >= 0 && deadline->valuedouble <= UINT32_MAX &&
        floor(deadline->valuedouble) == deadline->valuedouble && cJSON_IsString(line);
    /* Four exact keys implies no duplicate field can hide another required key. */
    if (!valid || !WifiControl_Deadline(millis(), (uint32_t)deadline->valuedouble) || !same_connection(generation)) {
        cJSON_Delete(json); wipe(body, sizeof(body));
        return reject(request, "409 Conflict", "{\"error\":\"Invalid, expired or wrong-boot control\"}");
    }
    for (unsigned i = 0; i < 256; ++i) if (!strcmp(control_seen[i], id->valuestring)) {
        cJSON_Delete(json); wipe(body, sizeof(body));
        return reject(request, "409 Conflict", "{\"error\":\"Duplicate command; not replayed\"}");
    }
    snprintf(control_seen[control_next], sizeof(control_seen[0]), "%s", id->valuestring);
    control_next = (control_next + 1) % 256;
    bool accepted = execute_control(line->valuestring, (uint32_t)deadline->valuedouble);
    char response[80];
    snprintf(response, sizeof(response), "{\"id\":\"%s\",\"accepted\":%s}", id->valuestring, accepted ? "true" : "false");
    cJSON_Delete(json); wipe(body, sizeof(body));
    return reject(request, accepted ? "200 OK" : "409 Conflict", response);
}
static esp_err_t audio_handler(httpd_req_t *request)
{
    uint32_t generation;
    if (!api_allowed(request, &generation)) return ESP_OK;
    char query[40], session[17];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        strncmp(query, "session=", 8) || strlen(query) != 24 ||
        httpd_query_key_value(query, "session", session, sizeof(session)) != ESP_OK || !hex_string(session, 16))
        return reject(request, "400 Bad Request", "{\"error\":\"Invalid audio session\"}");
    char *body = AudioMonitor_TakePullPacket(session);
    if (!body) return reject(request, "204 No Content", "");
    if (!same_connection(generation)) {
        cJSON_free(body);
        return reject(request, "503 Service Unavailable", "{\"error\":\"WiFi changed\"}");
    }
    httpd_resp_set_type(request, "application/json");
    esp_err_t error = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return error;
}
static esp_err_t capture_handler(httpd_req_t *request)
{
    uint32_t generation;
    if (!api_allowed(request, &generation)) return ESP_OK;
    Camera_Status camera; Camera_GetStatus(&camera);
    if (!camera.ready) return reject(request, "503 Service Unavailable", "{\"error\":\"Camera not ready\"}");
    Camera_Frame frame = {0};
    int64_t capture_started_us = esp_timer_get_time();
    if (Camera_AcquireFrame(&frame, 150) != ESP_OK)
        return reject(request, "503 Service Unavailable", "{\"error\":\"No fresh frame\"}");
    int64_t capture_time_us = esp_timer_get_time() - capture_started_us;
    if (!same_connection(generation)) {
        Camera_ReleaseFrame(&frame, false);
        return reject(request, "503 Service Unavailable", "{\"error\":\"WiFi unavailable\"}");
    }
    char sequence[16], timestamp[16], generation_text[16], capture_time[24];
    snprintf(sequence, sizeof(sequence), "%lu", (unsigned long)frame.sequence);
    snprintf(timestamp, sizeof(timestamp), "%lu", (unsigned long)frame.timestamp_ms);
    snprintf(generation_text, sizeof(generation_text), "%lu", (unsigned long)generation);
    snprintf(capture_time, sizeof(capture_time), "%lld", (long long)capture_time_us);
    httpd_resp_set_type(request, "image/jpeg");
    httpd_resp_set_hdr(request, "X-Frame-Seq", sequence);
    httpd_resp_set_hdr(request, "X-Frame-Timestamp-Ms", timestamp);
    httpd_resp_set_hdr(request, "X-Wifi-Generation", generation_text);
    httpd_resp_set_hdr(request, "X-Capture-Time-Us", capture_time);
    esp_err_t error = httpd_resp_send(request, (const char *)frame.data, frame.length);
    Camera_ReleaseFrame(&frame, error == ESP_OK);
    return error;
}
static int stream_send(httpd_handle_t server, int socket, const char *data, size_t length, int flags)
{
    (void)server;
    if (stream_send_deadline_us) {
        int64_t left_us = stream_send_deadline_us - esp_timer_get_time();
        if (left_us <= 0) return HTTPD_SOCK_ERR_TIMEOUT;
        /* A per-send timeout alone lets a slowly reading peer stretch a frame
         * indefinitely via partial sends. Bound the whole frame instead. */
        int64_t left_ms = (left_us + 999) / 1000;
        struct timeval timeout = { .tv_sec = left_ms / 1000, .tv_usec = (left_ms % 1000) * 1000 };
        if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
            return HTTPD_SOCK_ERR_FAIL;
    }
    return send(socket, data, length, flags);
}
static esp_err_t stream_handler(httpd_req_t *request)
{
    uint32_t generation;
    if (!api_allowed(request, &generation)) return ESP_OK;
    esp_err_t error = httpd_sess_set_send_override(request->handle, httpd_req_to_sockfd(request), stream_send);
    if (error != ESP_OK) return error;
    /* One reusable bounded copy frees both camera FBs while TCP waits for ACKs.
     * It also merges MIME headers and JPEG into a single HTTP chunk. */
    char *packet = heap_caps_malloc(WIFI_FRAME_MAX_BYTES + WIFI_STREAM_HEADER_BYTES,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!packet) return reject(request, "503 Service Unavailable", "{\"error\":\"Stream buffer unavailable\"}");
    char generation_text[16];
    snprintf(generation_text, sizeof(generation_text), "%lu", (unsigned long)generation);
    httpd_resp_set_hdr(request, "X-Wifi-Generation", generation_text);
    httpd_resp_set_type(request, "multipart/x-mixed-replace; boundary=robotframe");
    bool started = false;
    error = ESP_OK;
    while (same_connection(generation)) {
        Camera_Status camera; Camera_GetStatus(&camera);
        if (!camera.ready) break;
        Camera_Frame frame = {0};
        int64_t capture_started_us = esp_timer_get_time();
        error = Camera_AcquireFrame(&frame, 150);
        int64_t capture_time_us = esp_timer_get_time() - capture_started_us;
        if (error != ESP_OK) break;
        if (!same_connection(generation) || frame.length > WIFI_FRAME_MAX_BYTES) {
            Camera_ReleaseFrame(&frame, false);
            error = ESP_ERR_INVALID_STATE;
            break;
        }
        /* Exactly one pending frame. Slow clients still drop older camera
         * frames instead of accumulating an application frame queue. */
        int length = snprintf(packet, WIFI_STREAM_HEADER_BYTES,
            "\r\n--robotframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n"
            "X-Device-Id: %s\r\nX-Boot-Id: %s\r\nX-Wifi-Generation: %lu\r\n"
            "X-Frame-Seq: %lu\r\nX-Frame-Timestamp-Ms: %lu\r\nX-Capture-Time-Us: %lld\r\n\r\n",
            (unsigned)frame.length, device_id, boot_id, (unsigned long)generation,
            (unsigned long)frame.sequence, (unsigned long)frame.timestamp_ms, (long long)capture_time_us);
        if (length < 0 || (size_t)length >= WIFI_STREAM_HEADER_BYTES) {
            Camera_ReleaseFrame(&frame, false);
            error = ESP_ERR_INVALID_SIZE;
            break;
        }
        size_t packet_length = (size_t)length + frame.length;
        memcpy(packet + length, frame.data, frame.length);
        /* Same worker releases the camera lease BEFORE any network write. */
        Camera_ReleaseFrame(&frame, true);
        Camera_GetStatus(&camera);
        if (!same_connection(generation) || !camera.ready) {
            Camera_RecordSendFailure(); error = ESP_ERR_INVALID_STATE; break;
        }
        started = true;
        stream_send_deadline_us = esp_timer_get_time() + 1000000LL;
        error = httpd_resp_send_chunk(request, packet, packet_length);
        if (error != ESP_OK) Camera_RecordSendFailure();
        if (error != ESP_OK) break;
    }
    stream_send_deadline_us = esp_timer_get_time() + 1000000LL;
    if (!started) error = reject(request, "503 Service Unavailable", "{\"error\":\"Camera stream unavailable\"}");
    else if (error == ESP_OK) {
        static const char ending[] = "\r\n--robotframe--\r\n";
        error = httpd_resp_send_chunk(request, ending, sizeof(ending) - 1);
        if (error == ESP_OK) error = httpd_resp_send_chunk(request, NULL, 0);
    }
    stream_send_deadline_us = 0;
    heap_caps_free(packet);
    return error;
}
static void stop_api(void)
{
    portENTER_CRITICAL(&wifi_lock); status.api_ready = false; portEXIT_CRITICAL(&wifi_lock);
    /* Revoke before waiting: the separate stream worker exits after its
     * bounded capture/socket operation and always gives back its frame. */
    if (stream_api) { (void)httpd_stop(stream_api); stream_api = NULL; }
    if (api) { (void)httpd_stop(api); api = NULL; }
}
static esp_err_t api_open_socket(httpd_handle_t server, int socket)
{
    (void)server;
    /* Headers and JPEG bytes are separate writes. Do not hold the latter
     * behind a delayed ACK for the small header packet. */
    const int enabled = 1;
    return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0 ? ESP_OK : ESP_FAIL;
}
static esp_err_t start_api(void)
{
    if (api && stream_api) return ESP_OK;
    if (api || stream_api) stop_api();
#if ROBOT_PUBLIC_TEST_BUILD
    if (!WifiSessionAuth_TokenValid(session_api_token)) return ESP_ERR_INVALID_STATE;
#else
    if (sizeof(ROBOT_INGEST_TOKEN) <= 1) return ESP_ERR_INVALID_STATE;
#endif
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WIFI_API_PORT; config.ctrl_port = 32769;
    config.stack_size = 8192; config.max_open_sockets = 3;
    config.max_uri_handlers = 4; config.lru_purge_enable = true;
    config.recv_wait_timeout = 1; config.send_wait_timeout = 1;
    config.open_fn = api_open_socket;
    esp_err_t error = httpd_start(&api, &config);
    if (error != ESP_OK) return error;
    const httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    const httpd_uri_t capture_uri = { .uri = "/capture.jpg", .method = HTTP_GET, .handler = capture_handler };
    const httpd_uri_t control_uri = { .uri = "/api/control", .method = HTTP_POST, .handler = control_handler };
    const httpd_uri_t audio_uri = { .uri = "/api/audio", .method = HTTP_GET, .handler = audio_handler };
    error = httpd_register_uri_handler(api, &status_uri);
    if (error == ESP_OK) error = httpd_register_uri_handler(api, &capture_uri);
    if (error == ESP_OK) error = httpd_register_uri_handler(api, &control_uri);
    if (error == ESP_OK) error = httpd_register_uri_handler(api, &audio_uri);
    if (error == ESP_OK) {
        /* A long-lived stream must not occupy the status server's worker. */
        config.server_port = WIFI_STREAM_PORT; config.ctrl_port = 32770;
        config.max_open_sockets = 1; config.max_uri_handlers = 1;
        config.lru_purge_enable = false;
        error = httpd_start(&stream_api, &config);
    }
    if (error == ESP_OK) {
        const httpd_uri_t stream_uri = { .uri = "/stream.mjpg", .method = HTTP_GET, .handler = stream_handler };
        error = httpd_register_uri_handler(stream_api, &stream_uri);
    }
    if (error != ESP_OK) stop_api();
    return error;
}
static esp_err_t stop_station(void)
{
    revoke_link(); stop_api();
    if (!driver_started) return ESP_OK;
    xEventGroupClearBits(events, EV_STOP);
    esp_err_t error = esp_wifi_stop();
    if (error != ESP_OK) return error;
    /* The event-loop STOP barrier drains old disconnect/IP events before the
     * next credentials are installed. Late old GOT_IP must never save them. */
    EventBits_t bits = xEventGroupWaitBits(events, EV_STOP, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    if (!(bits & EV_STOP)) return ESP_ERR_TIMEOUT;
    driver_started = false;
    xEventGroupClearBits(events, EV_IP | EV_DOWN);
    return ESP_OK;
}
static esp_err_t initialize_wifi(void)
{
    esp_err_t error = nvs_flash_init(); /* Never erase unrelated NVS data. */
    if (error != ESP_OK) return error;
    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!station) station = esp_netif_create_default_wifi_sta();
    if (!station) return ESP_ERR_NO_MEM;
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT(); config.nvs_enable = 0;
    error = esp_wifi_init(&config);
    if (error == ESP_OK) error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (error == ESP_OK) error = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (error == ESP_OK) error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK) error = esp_wifi_set_mode(WIFI_MODE_STA);
    return error;
}
static esp_err_t connect_station(const Credentials *credentials)
{
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, credentials->ssid, strlen(credentials->ssid));
    memcpy(config.sta.password, credentials->password, strlen(credentials->password));
    config.sta.threshold.authmode = credentials->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &config);
    wipe(&config, sizeof(config));
    if (error == ESP_OK) error = esp_wifi_start();
    if (error != ESP_OK) return error;
    driver_started = true;
    error = esp_wifi_set_ps(WIFI_PS_NONE);
    if (error == ESP_OK) error = esp_wifi_connect();
    return error;
}
static bool take_command(WifiCommand *command)
{
    portENTER_CRITICAL(&wifi_lock);
    bool take = pending_valid;
    if (take) { *command = pending; wipe(&pending, sizeof(pending)); pending_valid = false; }
    portEXIT_CRITICAL(&wifi_lock);
    return take;
}
static esp_err_t scan_networks(const WifiCommand *command)
{
    BoardWifi_ScanStatus *result = calloc(1, sizeof(*result));
    wifi_ap_record_t *records = calloc(BOARD_WIFI_SCAN_MAX, sizeof(*records));
    if (!result || !records) { free(result); free(records); return ESP_ERR_NO_MEM; }
    snprintf(result->request_id, sizeof(result->request_id), "%s", command->id);
    result->running = true;
    portENTER_CRITICAL(&wifi_lock); scan_status = *result; portEXIT_CRITICAL(&wifi_lock);
    bool temporary = !driver_started;
    esp_err_t error = ESP_OK;
    if (temporary) {
        error = esp_wifi_start();
        if (error == ESP_OK) driver_started = true;
    }
    const wifi_scan_config_t config = {
        .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 30, .max = 100 },
    };
    if (error == ESP_OK) error = esp_wifi_scan_start(&config, true);
    uint16_t count = BOARD_WIFI_SCAN_MAX;
    if (error == ESP_OK) error = esp_wifi_scan_get_ap_records(&count, records);
    else (void)esp_wifi_clear_ap_list();
    if (error == ESP_OK) {
        /* IDF returns strongest first. Merge repeated BSSIDs for one SSID. */
        for (unsigned i = 0; i < count; ++i) {
            size_t length = strnlen((const char *)records[i].ssid, 32);
            if (!length) continue;
            bool duplicate = false;
            for (unsigned j = 0; j < result->count; ++j)
                if (strlen(result->networks[j].ssid) == length && !memcmp(result->networks[j].ssid, records[i].ssid, length)) duplicate = true;
            if (duplicate) continue;
            BoardWifi_AccessPoint *ap = &result->networks[result->count++];
            memcpy(ap->ssid, records[i].ssid, length); ap->ssid[length] = 0;
            ap->rssi = records[i].rssi; ap->channel = records[i].primary;
            ap->secure = records[i].authmode != WIFI_AUTH_OPEN;
        }
    }
    if (temporary && driver_started) {
        esp_err_t stop_error = stop_station();
        if (error == ESP_OK) error = stop_error;
    }
    if (!fresh(command)) error = ESP_ERR_TIMEOUT;
    result->running = false; result->completed = true; result->error = error;
    result->completed_ms = millis();
    if (error != ESP_OK) result->count = 0;
    portENTER_CRITICAL(&wifi_lock); scan_status = *result; portEXIT_CRITICAL(&wifi_lock);
    free(records); free(result); return error;
}
static void announce_wifi(uint32_t now)
{
    static int sock=-1;
    static uint32_t last;
    BoardWifi_Status current;BoardWifi_GetStatus(&current);
    if(!current.ready || !current.api_ready){if(sock>=0){close(sock);sock=-1;}return;}
    if((uint32_t)(now-last)<2000U)return;
    last=now;
    char key[65]={0};
#if ROBOT_PUBLIC_TEST_BUILD
    portENTER_CRITICAL(&wifi_lock);memcpy(key,session_api_token,sizeof(key));portEXIT_CRITICAL(&wifi_lock);
#else
    snprintf(key,sizeof(key),"%s",ROBOT_INGEST_TOKEN);
#endif
    if(!WifiSessionAuth_TokenValid(key)){wipe(key,sizeof(key));return;}
    char packet[256];unsigned char digest[32];
    int length=snprintf(packet,sizeof(packet),"RDW1 %s %s %s %lu",device_id,boot_id,current.ip,(unsigned long)now);
    if(length<=0 || (size_t)length>sizeof(packet)-66U){wipe(key,sizeof(key));return;}
    const mbedtls_md_info_t *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int result=md ? mbedtls_md_hmac(md,(const unsigned char *)key,64,(const unsigned char *)packet,(size_t)length,digest) : -1;
    wipe(key,sizeof(key));
    if(result!=0)return;
    packet[length++]=' ';
    for(unsigned i=0;i<32;++i)snprintf(packet+length+i*2,3,"%02x",digest[i]);
    length+=64;wipe(digest,sizeof(digest));
    if(sock<0){sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(sock>=0){int yes=1;(void)setsockopt(sock,SOL_SOCKET,SO_BROADCAST,&yes,sizeof(yes));}}
    if(sock>=0) {
        struct sockaddr_in destination={.sin_family=AF_INET,.sin_port=htons(32321),.sin_addr={.s_addr=htonl(INADDR_BROADCAST)}};
        if(sendto(sock,packet,(size_t)length,0,(struct sockaddr *)&destination,sizeof(destination))<0){close(sock);sock=-1;}
    }
}
static void wifi_task(void *unused)
{
    (void)unused;
    esp_err_t error = initialize_wifi();
    if (error != ESP_OK) {
        wipe(&initial, sizeof(initial)); link_state("ERROR", error);
        portENTER_CRITICAL(&wifi_lock);
        available = false; wipe(&pending, sizeof(pending)); pending_valid = false;
        if (status.request_id[0]) snprintf(status.request_status, sizeof(status.request_status), "FAILED");
        portEXIT_CRITICAL(&wifi_lock);
        vTaskDelete(NULL); return;
    }
    Credentials target = {0};
    bool paired_ready=false;
#if ROBOT_PUBLIC_TEST_BUILD
    WifiStoredProfile paired;
    paired_ready=load_paired_profile(&paired)==ESP_OK;
    bool saved=load_credentials(&target)==ESP_OK;
    if(paired_ready) {
        portENTER_CRITICAL(&wifi_lock);memcpy(session_api_token,paired.token,sizeof(session_api_token));portEXIT_CRITICAL(&wifi_lock);
    } else wipe(&target,sizeof(target));
    wipe(&paired,sizeof(paired));
#else
    bool saved=load_credentials(&target)==ESP_OK;
    if(!saved)target=initial;
    paired_ready=credentials_valid(target.ssid,target.password);
#endif
    wipe(&initial, sizeof(initial));
    portENTER_CRITICAL(&wifi_lock); status.saved = saved; status.autoconnect=paired_ready; portEXIT_CRITICAL(&wifi_lock);
    bool want_connection = false, persist_pending = false;
    uint32_t attempt_until = 0, next_retry = 0, last_rssi = 0;
    unsigned retries = 0;
    bool auto_enabled=paired_ready;
    uint32_t auto_retry_at=paired_ready ? millis()+3000U : 0;
    char active_id[17] = {0};
    /* Let camera/display boot allocations settle, then connect independently
     * of USB. A legacy password without its paired API key needs one USB pair. */
    if(paired_ready)link_state("RETRY",ESP_OK);
    wipe(&target,sizeof(target));
    for (;;) {
        WifiCommand command = {0};
        if (take_command(&command)) {
            if (!fresh(&command)) request_result(command.id, "EXPIRED", ESP_ERR_TIMEOUT);
            else if (!strcmp(command.action, "scan")) {
                /* Scan never replaces credentials, pairing keys or the live API. */
                error = scan_networks(&command);
                request_result(command.id, error == ESP_OK ? "SUCCEEDED" : "FAILED", error);
            } else {
                want_connection = false; persist_pending = false; retries = 0; next_retry = 0;
                auto_enabled=false;auto_retry_at=0;
                error = stop_station();
                if (error == ESP_OK && !fresh(&command)) error = ESP_ERR_TIMEOUT;
                /* Stop/revoke both HTTP workers before changing their key.
                 * The key and credentials belong to the same fresh USB request. */
#if ROBOT_PUBLIC_TEST_BUILD
                if (error == ESP_OK && strcmp(command.action, "disconnect")) {
                    if (WifiSessionAuth_TokenValid(command.api_token)) {
                        portENTER_CRITICAL(&wifi_lock);
                        wipe(session_api_token, sizeof(session_api_token));
                        memcpy(session_api_token, command.api_token, sizeof(session_api_token));
                        portEXIT_CRITICAL(&wifi_lock);
                    }
                    if (!WifiSessionAuth_TokenValid(session_api_token)) error = ESP_ERR_INVALID_STATE;
                }
#endif
                if (error == ESP_OK && !strcmp(command.action, "disconnect")) {
#if ROBOT_PUBLIC_TEST_BUILD
                    portENTER_CRITICAL(&wifi_lock);
                    wipe(session_api_token, sizeof(session_api_token));
                    portEXIT_CRITICAL(&wifi_lock);
#endif
                    wipe(&target, sizeof(target)); active_id[0] = 0;
                    portENTER_CRITICAL(&wifi_lock); status.ssid[0] = 0; portEXIT_CRITICAL(&wifi_lock);
                    link_state("OFF", ESP_OK); request_result(command.id, "SUCCEEDED", ESP_OK);
                } else if (error == ESP_OK) {
                    wipe(&target, sizeof(target));
                    if (!strcmp(command.action, "reconnect")) error = load_credentials(&target);
                    else target = command.credentials;
                    if (error == ESP_OK && !fresh(&command)) error = ESP_ERR_TIMEOUT;
                    if (error == ESP_OK) {
                        snprintf(active_id, sizeof(active_id), "%s", command.id);
                        portENTER_CRITICAL(&wifi_lock);
                        snprintf(status.ssid, sizeof(status.ssid), "%s", target.ssid);
                        status.disconnect_reason = 0; status.save_error = 0;
                        portEXIT_CRITICAL(&wifi_lock);
                        error = connect_station(&target);
                        want_connection = error == ESP_OK; persist_pending = true;
                        attempt_until = millis() + WIFI_ATTEMPT_MS;
                        if (error == ESP_OK) {
                            link_state("CONNECTING", ESP_OK); request_result(command.id, "CONNECTING", ESP_OK);
                        }
                    }
                }
                if (error != ESP_OK) {
                    (void)stop_station(); wipe(&target, sizeof(target));
                    link_state("ERROR", error);
                    request_result(command.id, error == ESP_ERR_TIMEOUT ? "EXPIRED" : "FAILED", error);
                }
            }
            wipe(&command, sizeof(command));
        }
        EventBits_t bits = xEventGroupWaitBits(events, EV_IP | EV_DOWN, pdTRUE, pdFALSE, pdMS_TO_TICKS(25));
        uint32_t now = millis();
        if ((bits & EV_IP) && want_connection) {
            BoardWifi_Status before; BoardWifi_GetStatus(&before);
            wifi_ap_record_t ap;
            esp_netif_ip_info_t info;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
                strnlen((const char *)ap.ssid, sizeof(ap.ssid)) == strlen(target.ssid) &&
                !memcmp(ap.ssid, target.ssid, strlen(target.ssid)) &&
                esp_netif_get_ip_info(station, &info) == ESP_OK && info.ip.addr) {
                bool did_persist = persist_pending;
                int save_error = did_persist ? save_credentials(&target) : ESP_OK;
                persist_pending = false; wipe(target.password, sizeof(target.password));
                error = start_api();
                portENTER_CRITICAL(&wifi_lock);
                /* A disconnect can run on the event-loop task while NVS or
                 * HTTP setup is in progress. It must win over this GOT_IP. */
                bool unchanged = status.generation == before.generation;
                if (did_persist) {
                    status.save_error = save_error;
                    if (save_error == ESP_OK) {status.saved = true;status.autoconnect=true;}
                }
                if (unchanged) {
                    status.ready = true; status.api_ready = error == ESP_OK;
                    status.ip_address = info.ip.addr;
                    snprintf(status.ip, sizeof(status.ip), IPSTR, IP2STR(&info.ip));
                    status.rssi = ap.rssi;
                    status.disconnect_reason = 0;
                    ++status.generation; ++status.got_ip_events;
                    snprintf(status.state, sizeof(status.state), "%s", error == ESP_OK ? "READY" : "ERROR");
                    status.error = error;
                }
                portEXIT_CRITICAL(&wifi_lock);
                if (!unchanged) { stop_api(); continue; }
                request_result(active_id, error == ESP_OK ? "SUCCEEDED" : "FAILED", error);
                if(save_error==ESP_OK)auto_enabled=true;
                auto_retry_at=0;
                retries = 0; next_retry = 0; attempt_until = 0;
                bits &= ~EV_DOWN;
            }
        }
        if ((bits & EV_DOWN) && want_connection) {
            stop_api();
            if (!attempt_until) attempt_until = now + WIFI_ATTEMPT_MS;
            if (++retries <= 3) {
                next_retry = now + (500U << retries); link_state("RETRY", 0);
            } else attempt_until = now;
        }
        if (want_connection && attempt_until && (int32_t)(now - attempt_until) >= 0) {
            want_connection = false; (void)stop_station(); wipe(target.password, sizeof(target.password));
            auto_retry_at=auto_enabled ? now+15000U : 0;
            link_state(auto_enabled ? "RETRY" : "ERROR", ESP_ERR_TIMEOUT); request_result(active_id, "FAILED", ESP_ERR_TIMEOUT);
        } else if (want_connection && next_retry && (int32_t)(now - next_retry) >= 0) {
            next_retry = 0; error = esp_wifi_connect();
            if (error != ESP_OK) { attempt_until = now; link_state("ERROR", error); }
            else link_state("CONNECTING", ESP_OK);
        }
        if(!want_connection && auto_enabled && auto_retry_at && (int32_t)(now-auto_retry_at)>=0) {
            auto_retry_at=0;
            WifiCommand scan={.deadline=millis()+5000U};
            snprintf(scan.id,sizeof(scan.id),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
            memcpy(scan.boot,boot_id,8);scan.boot[8]=0;
            link_state("SCANNING",ESP_OK);
            (void)scan_networks(&scan);
            wipe(&target,sizeof(target));
#if ROBOT_PUBLIC_TEST_BUILD
            WifiStoredProfile retry_profile;
            error=load_paired_profile(&retry_profile);
            if(error==ESP_OK){memcpy(target.ssid,retry_profile.ssid,sizeof(target.ssid));memcpy(target.password,retry_profile.password,sizeof(target.password));}
            wipe(&retry_profile,sizeof(retry_profile));
#else
            error=load_credentials(&target);
#endif
            bool needs_persist=false;
#if !ROBOT_PUBLIC_TEST_BUILD
            if(error!=ESP_OK && credentials_valid(ROBOT_WIFI_SSID,ROBOT_WIFI_PASSWORD)) {
                snprintf(target.ssid,sizeof(target.ssid),"%s",ROBOT_WIFI_SSID);
                snprintf(target.password,sizeof(target.password),"%s",ROBOT_WIFI_PASSWORD);
                error=ESP_OK;needs_persist=true;
            }
#endif
            if(error==ESP_OK) {
                portENTER_CRITICAL(&wifi_lock);snprintf(status.ssid,sizeof(status.ssid),"%s",target.ssid);portEXIT_CRITICAL(&wifi_lock);
                error=connect_station(&target);
            }
            wipe(target.password,sizeof(target.password));
            want_connection=error==ESP_OK;persist_pending=needs_persist;
            if(want_connection){attempt_until=millis()+WIFI_ATTEMPT_MS;retries=0;next_retry=0;link_state("CONNECTING",ESP_OK);}
            else{(void)stop_station();auto_retry_at=millis()+15000U;link_state("RETRY",error);}
            now=millis();
        }
        BoardWifi_Status current; BoardWifi_GetStatus(&current);
        announce_wifi(now);
        if (current.ready && (uint32_t)(now - last_rssi) >= 1000U) {
            last_rssi = now;
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                portENTER_CRITICAL(&wifi_lock); status.rssi = ap.rssi; portEXIT_CRITICAL(&wifi_lock);
            }
        }
    }
}
esp_err_t BoardWifi_Start(const char *device, const char *boot, char *(*snapshot)(void),
                          const char *legacy_ssid, const char *legacy_password)
{
    if (!device || !boot || !snapshot || !hex_string(boot, 32)) return ESP_ERR_INVALID_ARG;
    if (events) return ESP_ERR_INVALID_STATE;
    snprintf(device_id, sizeof(device_id), "%s", device);
    snprintf(boot_id, sizeof(boot_id), "%s", boot);
    make_snapshot = snapshot;
    if (credentials_valid(legacy_ssid, legacy_password)) {
        snprintf(initial.ssid, sizeof(initial.ssid), "%s", legacy_ssid);
        snprintf(initial.password, sizeof(initial.password), "%s", legacy_password);
    }
    events = xEventGroupCreate();
    if (!events) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&wifi_lock); available = true; portEXIT_CRITICAL(&wifi_lock);
    if (xTaskCreate(wifi_task, "wifi_manager", 6144, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&wifi_lock); available = false; portEXIT_CRITICAL(&wifi_lock);
        wipe(&initial, sizeof(initial)); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
bool BoardWifi_Queue(const char *id, const char *action, const char *ssid,
                     const char *password, uint32_t deadline_ms, const char *boot_prefix)
{
    return BoardWifi_QueuePaired(id, action, ssid, password, deadline_ms, boot_prefix, NULL);
}
bool BoardWifi_QueuePaired(const char *id, const char *action, const char *ssid,
                           const char *password, uint32_t deadline_ms,
                           const char *boot_prefix, const char *api_token)
{
    if (!hex_string(id, 16) || !hex_string(boot_prefix, 8) || !action) return false;
    bool connect = !strcmp(action, "connect");
    if (!connect && strcmp(action, "reconnect") && strcmp(action, "disconnect") && strcmp(action, "scan")) return false;
    if (connect && !credentials_valid(ssid, password)) return false;
    bool supplied = api_token && api_token[0];
    if (supplied && ((!strcmp(action, "disconnect") || !strcmp(action, "scan")) || !WifiSessionAuth_TokenValid(api_token))) return false;
#if !ROBOT_PUBLIC_TEST_BUILD
    /* Private builds keep their compiled trust root; USB cannot rotate it. */
    if (supplied) return false;
#endif
    WifiCommand command = { .deadline = deadline_ms };
    memcpy(command.id, id, 17); memcpy(command.boot, boot_prefix, 9);
    snprintf(command.action, sizeof(command.action), "%s", action);
    if (!fresh(&command)) return false;
    if (supplied) memcpy(command.api_token, api_token, sizeof(command.api_token));
    if (connect) {
        snprintf(command.credentials.ssid, sizeof(command.credentials.ssid), "%s", ssid);
        snprintf(command.credentials.password, sizeof(command.credentials.password), "%s", password);
    }
    portENTER_CRITICAL(&wifi_lock);
    bool accepted = available && !pending_valid;
    bool duplicate = false;
    for (unsigned i = 0; i < 64; ++i) if (!strcmp(seen[i], id)) { duplicate = true; break; }
    if (accepted && !duplicate) {
        pending = command; pending_valid = true;
        memcpy(seen[seen_next], id, 17); seen_next = (seen_next + 1) % 64;
        memcpy(status.request_id, id, 17);
        snprintf(status.request_status, sizeof(status.request_status), "PENDING");
        status.error = 0;
    }
    portEXIT_CRITICAL(&wifi_lock);
    wipe(&command, sizeof(command));
    return duplicate || accepted;
}
