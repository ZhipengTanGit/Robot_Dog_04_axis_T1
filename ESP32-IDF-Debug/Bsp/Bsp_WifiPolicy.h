#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline bool WifiPolicy_Deadline(uint32_t now, uint32_t deadline,
                                       const char *expected_boot, const char *boot_prefix)
{
    int32_t left = (int32_t)(deadline - now);
    return !memcmp(expected_boot, boot_prefix, 8) && left > 0 && left <= 15000;
}
/* Always compares the entire expected token, even when length/read differ.
 * received must contain at least expected_length initialized bytes. */
static inline bool WifiPolicy_SecretEqual(const unsigned char *expected, size_t expected_length,
                                          const unsigned char *received, size_t received_length,
                                          bool header_read)
{
    volatile size_t difference = (received_length ^ expected_length) | (size_t)!header_read;
    for (size_t i = 0; i < expected_length; ++i) difference |= received[i] ^ expected[i];
    return difference == 0;
}
