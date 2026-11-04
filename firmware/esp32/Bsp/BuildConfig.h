#ifndef ROBOT_BUILD_CONFIG_H
#define ROBOT_BUILD_CONFIG_H

/* Keep private bench configuration as the default. Public firmware opts in
 * explicitly and must not include either local header, even for dead code. */
#ifndef ROBOT_PUBLIC_TEST_BUILD
#define ROBOT_PUBLIC_TEST_BUILD 0
#endif

#if ROBOT_PUBLIC_TEST_BUILD
#define ROBOT_NETWORK_ENABLED 0
#define ROBOT_WIFI_SSID ""
#define ROBOT_WIFI_PASSWORD ""
#define ROBOT_UDP_TARGET_IP "0.0.0.0"
#define ROBOT_UDP_TARGET_PORT 9000
#define ROBOT_UDP_LOCAL_PORT 9001
#define ROBOT_SERVER_URL ""
#define ROBOT_INGEST_TOKEN ""
#define ROBOT_FIRMWARE_BUILD_ID "public-usb-jpeg-small-20261102-r1"
#else
#include "NetworkConfig.local.h"
#include "TelemetryConfig.local.h"
#endif

#endif
