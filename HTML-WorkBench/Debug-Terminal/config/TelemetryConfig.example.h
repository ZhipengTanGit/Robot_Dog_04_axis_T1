#ifndef ROBOT_TELEMETRY_CONFIG_H
#define ROBOT_TELEMETRY_CONFIG_H
#define ROBOT_NETWORK_ENABLED 0 /* USB-only by default; set 1 for LAN uploads. */
/* Copy to TelemetryConfig.local.h. Match the Ubuntu service environment token.
 * LAN HTTP only: do not expose this listener or its token to the Internet. */
#define ROBOT_SERVER_URL "http://192.168.0.194:8080"
#define ROBOT_INGEST_TOKEN "REPLACE_WITH_RANDOM_LOCAL_TOKEN"
#endif
