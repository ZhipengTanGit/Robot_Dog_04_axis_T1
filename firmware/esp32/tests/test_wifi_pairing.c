#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "WifiSessionAuth.h"
#include "UartBridgePolicy.h"

static const char token[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void test_auth(void)
{
    unsigned char header[WIFI_SESSION_HEADER_CAPACITY] = "Bearer ";
    memcpy(header + 7, token, sizeof(token));
    assert(WifiSessionAuth_TokenValid(token));
    assert(!WifiSessionAuth_TokenValid(NULL));
    assert(!WifiSessionAuth_TokenValid(""));
    assert(!WifiSessionAuth_TokenValid("short"));
    assert(!WifiSessionAuth_Authorized("", header, 71, true));
    assert(WifiSessionAuth_Authorized(token, header, 71, true));
    assert(!WifiSessionAuth_Authorized(token, header, 70, true));
    assert(!WifiSessionAuth_Authorized(token, header, 72, true));
    assert(!WifiSessionAuth_Authorized(token, header, 71, false));
    for (unsigned i = 0; i < 71; ++i) {
        header[i] ^= 1;
        assert(!WifiSessionAuth_Authorized(token, header, 71, true));
        header[i] ^= 1;
    }
    char malformed[66];
    memcpy(malformed, token, sizeof(token)); malformed[0] = 'A';
    assert(!WifiSessionAuth_TokenValid(malformed));
    malformed[0] = '0'; malformed[64] = '0'; malformed[65] = 0;
    assert(!WifiSessionAuth_TokenValid(malformed));
}

static void parse_every_split(const char *wire, UartBridgeLocalAction wanted,
                              size_t ssid_length, size_t password_length)
{
    const size_t length = strlen(wire);
    for (size_t split = 0; split < length; ++split) {
        UartBridgeLocalState state;
        UartBridgeLocalWifi wifi;
        UartBridgeLocalAction action;
        uint8_t out[400]; char id[17]; uint32_t ttl;
        UartBridgeLocal_Reset(&state);
        assert(!UartBridgeLocal_Filter(&state, (const uint8_t *)wire, split,
                                       out, &action, id, &ttl, NULL, &wifi));
        assert(action == UART_BRIDGE_LOCAL_NONE);
        assert(!wifi.password[0] && !wifi.api_token[0]);
        assert(!UartBridgeLocal_Filter(&state, (const uint8_t *)wire + split, length - split,
                                       out, &action, id, &ttl, NULL, &wifi));
        assert(action == wanted);
        if (wanted != UART_BRIDGE_LOCAL_REJECTED) {
            assert(wifi.deadline_ms == UINT32_MAX);
            assert(!strcmp(wifi.boot_prefix, "89abcdef"));
            assert(!strcmp(wifi.api_token, token));
            assert(strlen(wifi.ssid) == ssid_length && strlen(wifi.password) == password_length);
        } else assert(!wifi.api_token[0] && !wifi.password[0]);
        for (size_t i = 0; i < sizeof(state.line); ++i) assert(!state.line[i]);
        UartBridgeLocal_ClearSecret(&wifi, sizeof(wifi));
        for (size_t i = 0; i < sizeof(wifi); ++i) assert(!((unsigned char *)&wifi)[i]);
    }
}

int main(void)
{
    test_auth();
    char wire[400];
    snprintf(wire, sizeof(wire), "ESP WIFI_RECONNECT 0123456789abcdef 4294967295 89abcdef %s\n", token);
    parse_every_split(wire, UART_BRIDGE_LOCAL_WIFI_RECONNECT, 0, 0);
    snprintf(wire, sizeof(wire), "ESP WIFI_CONNECT 0123456789abcdef 4294967295 89abcdef 41 - %s\n", token);
    parse_every_split(wire, UART_BRIDGE_LOCAL_WIFI_CONNECT, 1, 0);
    strcpy(wire, "ESP WIFI_CONNECT 0123456789abcdef 4294967295 89abcdef ");
    for (unsigned i = 0; i < 32; ++i) strcat(wire, "41");
    strcat(wire, " ");
    for (unsigned i = 0; i < 64; ++i) strcat(wire, "61");
    strcat(wire, " "); strcat(wire, token); strcat(wire, "\n");
    assert(strlen(wire) > 288); /* Regression: previous capture buffer overflowed. */
    parse_every_split(wire, UART_BRIDGE_LOCAL_WIFI_CONNECT, 32, 64);
    snprintf(wire, sizeof(wire), "ESP WIFI_DISCONNECT 0123456789abcdef 4294967295 89abcdef %s\n", token);
    parse_every_split(wire, UART_BRIDGE_LOCAL_REJECTED, 0, 0);
    snprintf(wire, sizeof(wire), "ESP WIFI_RECONNECT 0123456789abcdef 4294967295 89abcdef %s extra\n", token);
    parse_every_split(wire, UART_BRIDGE_LOCAL_REJECTED, 0, 0);
    snprintf(wire, sizeof(wire), "ESP WIFI_RECONNECT 0123456789abcdef 4294967295 89abcdef %s\n", token);
    wire[strlen(wire) - 2] = 'G';
    parse_every_split(wire, UART_BRIDGE_LOCAL_REJECTED, 0, 0);
    puts("WiFi USB pairing: authorization, framing, longest credentials and secret erasure passed");
    return 0;
}
