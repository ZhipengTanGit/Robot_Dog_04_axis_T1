#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "Bsp_WifiPolicy.h"

#define WIFI_SESSION_TOKEN_LENGTH 64U
#define WIFI_SESSION_HEADER_CAPACITY (7U + WIFI_SESSION_TOKEN_LENGTH + 1U)

/* USB provisioning carries a full 256-bit lower-case hex key. No key is
 * accepted from HTTP, advertised in telemetry, or persisted in device NVS. */
static inline bool WifiSessionAuth_TokenValid(const char *token)
{
    if (!token) return false;
    for (size_t i = 0; i < WIFI_SESSION_TOKEN_LENGTH; ++i) {
        char c = token[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return token[WIFI_SESSION_TOKEN_LENGTH] == 0;
}

/* received points to WIFI_SESSION_HEADER_CAPACITY initialized bytes. */
static inline bool WifiSessionAuth_Authorized(const char *token,
                                              const unsigned char *received,
                                              size_t length, bool header_read)
{
    unsigned char expected[WIFI_SESSION_HEADER_CAPACITY] = "Bearer ";
    bool ready = WifiSessionAuth_TokenValid(token);
    if (ready) memcpy(expected + 7, token, WIFI_SESSION_TOKEN_LENGTH);
    bool equal = WifiPolicy_SecretEqual(expected, sizeof(expected) - 1,
                                        received, length, header_read);
    volatile unsigned char *erase = expected;
    for (size_t i = 0; i < sizeof(expected); ++i) erase[i] = 0;
    return ready && equal;
}
