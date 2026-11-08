#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace robot {
bool Token(std::string_view line, std::string_view key, std::string_view& value);
bool UnsignedToken(std::string_view line, std::string_view key, uint32_t& value);
bool SignedToken(std::string_view line, std::string_view key, int32_t& value);

struct Telemetry {
    bool present = false;
    uint32_t received_ms = 0, uptime_ms = 0, last_id = 0;
    bool sensor_valid = false, servo_allowed = false, teaching = false;
    int32_t temperature_cc = 0;
    uint32_t humidity_cpct = 0, sensor_age_ms = 0;
    uint32_t walk_id = 0, servo_mask = 0;
    std::string walk_state, walk_reason, sensor_state;
    bool Parse(std::string_view line, uint32_t now);
    bool Fresh(uint32_t now) const;
    bool SensorFresh(uint32_t now) const;
    uint32_t EstimatedUptime(uint32_t now) const;
};
}
