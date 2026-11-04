#include "UartBridgePolicy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void parse_fragmented(const char *command, UartBridgeLocalAction expected)
{
    UartBridgeLocalState state;
    UartBridgeLocal_Reset(&state);
    UartBridgeLocalAction action = UART_BRIDGE_LOCAL_NONE;
    char id[17]; uint32_t ttl; UartBridgeLocalCharge charge; UartBridgeLocalWifi wifi; SoundCommand sound;
    for (size_t i = 0; i < strlen(command); ++i) {
        uint8_t output[5];
        size_t count = UartBridgeLocal_FilterWithSound(&state, (const uint8_t *)command + i, 1,
            output, &action, id, &ttl, &charge, &wifi, &sound);
        assert(count == 0);
        if (i + 1 < strlen(command)) assert(action == UART_BRIDGE_LOCAL_NONE);
    }
    assert(action == expected);
    if (expected == UART_BRIDGE_LOCAL_CAMERA_SNAPSHOT) assert(!strcmp(id, "0123456789abcdef"));
}
int main(void)
{
    parse_fragmented("ESP CAMERA_SNAPSHOT 0123456789abcdef\n", UART_BRIDGE_LOCAL_CAMERA_SNAPSHOT);
    parse_fragmented("ESP CAMERA_SNAPSHOT 0123456789abcdef\r", UART_BRIDGE_LOCAL_CAMERA_SNAPSHOT);
    parse_fragmented("ESP CAMERA_SNAPSHOT 0123456789abcdeF\n", UART_BRIDGE_LOCAL_REJECTED);
    parse_fragmented("ESP CAMERA_SNAPSHOT 0123456789abcde\n", UART_BRIDGE_LOCAL_REJECTED);
    parse_fragmented("ESP CAMERA_SNAPSHOT 0123456789abcdef extra\n", UART_BRIDGE_LOCAL_REJECTED);
    puts("camera snapshot command fragmentation/rejection passed");
    return 0;
}
