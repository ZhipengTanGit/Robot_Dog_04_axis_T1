#include "robot_telemetry.h"
#include <limits>
#include <type_traits>

namespace robot {
bool Token(std::string_view line, std::string_view key, std::string_view& value) {
    while (!line.empty()) {
        auto end = line.find_first_of(" \r\n");
        auto word = line.substr(0, end);
        if (word.size() > key.size() && word.substr(0, key.size()) == key && word[key.size()] == '=') {
            value = word.substr(key.size() + 1);
            return true;
        }
        if (end == std::string_view::npos) break;
        line.remove_prefix(end + 1);
    }
    return false;
}
template<typename T>
static bool Number(std::string_view line, std::string_view key, T& value) {
    std::string_view s;
    if (!Token(line, key, s) || s.empty()) return false;
    bool negative = s.front() == '-';
    if (negative) {
        if (!std::is_signed_v<T>) return false;
        s.remove_prefix(1);
    }
    if (s.empty()) return false;
    const uint64_t limit = uint64_t(std::numeric_limits<T>::max()) + (negative ? 1U : 0U);
    uint64_t parsed = 0;
    for (char c : s) {
        if (c < '0' || c > '9' || parsed > (limit - unsigned(c - '0')) / 10) return false;
        parsed = parsed * 10 + unsigned(c - '0');
    }
    value = negative ? static_cast<T>(-static_cast<int64_t>(parsed)) : static_cast<T>(parsed);
    return true;
}
bool UnsignedToken(std::string_view l, std::string_view k, uint32_t& v) { return Number(l, k, v); }
bool SignedToken(std::string_view l, std::string_view k, int32_t& v) { return Number(l, k, v); }
bool Telemetry::Parse(std::string_view line, uint32_t now) {
    Telemetry next;
    uint32_t valid, allowed, teach;
    if (line.substr(0, 7) != "PERIPH " || !UnsignedToken(line, "t", next.uptime_ms) ||
        !UnsignedToken(line, "last_id", next.last_id) ||
        !UnsignedToken(line, "servo_allowed", allowed) || allowed > 1 ||
        !UnsignedToken(line, "teach", teach) || teach > 1 ||
        !UnsignedToken(line, "aht_valid", valid) || valid > 1) return false;
    next.sensor_valid = valid == 1 && SignedToken(line, "aht_t_cC", next.temperature_cc) &&
        UnsignedToken(line, "aht_rh_cPct", next.humidity_cpct) &&
        UnsignedToken(line, "aht_age_ms", next.sensor_age_ms) &&
        next.temperature_cc >= -4000 && next.temperature_cc <= 8500 && next.humidity_cpct <= 10000;
    std::string_view text;
    if (Token(line, "walk_state", text)) next.walk_state = text;
    if (Token(line, "walk_reason", text)) next.walk_reason = text;
    if (Token(line, "aht_state", text)) next.sensor_state = text;
    UnsignedToken(line, "walk_id", next.walk_id);
    UnsignedToken(line, "servo_mask", next.servo_mask);
    next.servo_allowed = allowed == 1;
    next.teaching = teach == 1;
    next.present = true;
    next.received_ms = now;
    *this = next;
    return true;
}
bool Telemetry::Fresh(uint32_t now) const { return present && uint32_t(now - received_ms) < 2500; }
bool Telemetry::SensorFresh(uint32_t now) const {
    return Fresh(now) && sensor_valid && uint64_t(sensor_age_ms) + uint32_t(now - received_ms) < 5000;
}
uint32_t Telemetry::EstimatedUptime(uint32_t now) const { return uptime_ms + uint32_t(now - received_ms); }
}
