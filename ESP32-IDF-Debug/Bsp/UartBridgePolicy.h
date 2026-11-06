#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "SoundCommandLogic.h"

/* PC commands terminate on CR or LF; STM log records terminate on LF. */
typedef struct {
    bool pc_input;
    bool at_boundary;
    bool discarding;
} UartBridgeFrameState;

void UartBridgeFrame_Init(UartBridgeFrameState *state, bool pc_input);
void UartBridgeFrame_Invalidate(UartBridgeFrameState *state);
size_t UartBridgeFrame_Discard(UartBridgeFrameState *state, const uint8_t *data, size_t length);
size_t UartBridgeFrame_Segment(const UartBridgeFrameState *state, const uint8_t *data, size_t length);
void UartBridgeFrame_Forwarded(UartBridgeFrameState *state, const uint8_t *data, size_t length);

/* 8N1 line time plus at least 20 ms scheduling margin, rounded UP to ticks. */
uint32_t UartBridgeCommand_ReserveTicks(size_t length, uint32_t baud, uint32_t tick_ms);
bool UartBridgeCommand_CanSend(uint32_t deadline, uint32_t now, uint32_t reserve_ticks);

typedef enum {
    UART_BRIDGE_LOCAL_NONE, UART_BRIDGE_LOCAL_STATUS,
    UART_BRIDGE_LOCAL_SCAN, UART_BRIDGE_LOCAL_AUDIO_START,
    UART_BRIDGE_LOCAL_AUDIO_STOP, UART_BRIDGE_LOCAL_CHARGE_CURRENT,
    UART_BRIDGE_LOCAL_CAMERA_CHECK, UART_BRIDGE_LOCAL_SOUND,
    UART_BRIDGE_LOCAL_CAMERA_SNAPSHOT,
    UART_BRIDGE_LOCAL_WIFI_CONNECT, UART_BRIDGE_LOCAL_WIFI_RECONNECT,
    UART_BRIDGE_LOCAL_WIFI_DISCONNECT,
    UART_BRIDGE_LOCAL_WIFI_SCAN,
    UART_BRIDGE_LOCAL_REJECTED
} UartBridgeLocalAction;
typedef struct {
    uint32_t ma;
    uint32_t deadline_ms;
    char boot_prefix[9];
} UartBridgeLocalCharge;
typedef struct {
    uint32_t deadline_ms;
    char boot_prefix[9];
    char ssid[33];
    char password[65];
    char api_token[65];
} UartBridgeLocalWifi;
/* Erase transient credential copies even under optimizing builds. */
void UartBridgeLocal_ClearSecret(void *data, size_t length);
typedef struct {
    /* Max SSID/password hex plus the optional 64-character USB pairing key. */
    char line[352];
    size_t length;
    unsigned mode;
} UartBridgeLocalState;
void UartBridgeLocal_Reset(UartBridgeLocalState *state);
/* Input is one CR/LF-delimited segment (or a fragment without a delimiter),
 * as returned by UartBridgeFrame_Segment. out must hold length+4 bytes.
 * Only a possible ESP prefix is buffered; unrelated bytes pass unchanged.
 * Incomplete/malformed ESP commands never fall through to STM32. */
size_t UartBridgeLocal_Filter(UartBridgeLocalState *state, const uint8_t *data, size_t length,
                             uint8_t *out, UartBridgeLocalAction *action, char request_id[17], uint32_t *ttl_ms,
                             UartBridgeLocalCharge *charge, UartBridgeLocalWifi *wifi);

size_t UartBridgeLocal_FilterWithSound(UartBridgeLocalState *state, const uint8_t *data, size_t length,
    uint8_t *out, UartBridgeLocalAction *action, char request_id[17], uint32_t *ttl_ms,
    UartBridgeLocalCharge *charge, UartBridgeLocalWifi *wifi, SoundCommand *sound);
