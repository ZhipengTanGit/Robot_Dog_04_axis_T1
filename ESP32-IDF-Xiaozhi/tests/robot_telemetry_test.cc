#include "robot_telemetry.h"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
    const std::string base = "PERIPH t=9000 aht_t_cC=-125 aht_rh_cPct=5432 aht_valid=1 aht_age_ms=100 aht_state=READY servo_allowed=1 teach=0 last_id=77 walk_state=IDLE walk_id=0 walk_reason=NONE servo_mask=0";
    robot::Telemetry t;
    assert(!t.Fresh(0) && !t.SensorFresh(0));
    assert(t.Parse(base, 10000));
    assert(t.temperature_cc == -125 && t.humidity_cpct == 5432 && t.last_id == 77);
    assert(t.Fresh(12499) && !t.Fresh(12500));
    assert(t.EstimatedUptime(11000) == 10000);
    assert(t.SensorFresh(10000));
    auto altered = [&](const std::string& from, const std::string& to) {
        auto s = base; s.replace(s.find(from), from.size(), to); return s;
    };
    assert(t.Parse(altered("aht_age_ms=100", "aht_age_ms=4999"), 10000));
    assert(t.SensorFresh(10000) && !t.SensorFresh(10001));
    assert(t.Parse(altered("aht_age_ms=100", "aht_age_ms=4294967295"), 10000));
    assert(!t.SensorFresh(10001)); // age addition must not overflow to fresh.
    for (auto value : {"NA", "2500junk", "2147483648", "-4001", "8501"}) {
        assert(t.Parse(altered("aht_t_cC=-125", std::string("aht_t_cC=") + value), 10000));
        assert(!t.SensorFresh(10000));
    }
    for (auto value : {"NA", "-1", "10001", "4294967296"}) {
        assert(t.Parse(altered("aht_rh_cPct=5432", std::string("aht_rh_cPct=") + value), 10000));
        assert(!t.SensorFresh(10000));
    }
    assert(t.Parse(altered("aht_valid=1", "aht_valid=0"), 10000));
    assert(!t.SensorFresh(10000));
    assert(!t.Parse(altered("servo_allowed=1", "servo_allowed=2"), 10000));
    assert(!t.Parse(altered("last_id=77", "last_id=77x"), 10000));
    assert(!t.Parse("noise " + base, 10000));
    assert(!t.Parse("PERIPH_ACK t=0 ok=1", 10000));
    assert(!t.Parse("PERIPH t=1", 10000));
    uint32_t v;
    assert(robot::UnsignedToken("last_id=42 id=17", "id", v) && v == 17);
    assert(!robot::UnsignedToken("last_id=42", "id", v));
    assert(!robot::UnsignedToken("id=0x20", "id", v));
    assert(!robot::UnsignedToken("id=+1", "id", v));
    const uint32_t near_wrap = std::numeric_limits<uint32_t>::max() - 99;
    assert(t.Parse(base, near_wrap));
    assert(t.Fresh(100) && t.SensorFresh(100));
    assert(t.EstimatedUptime(100) == 9200);
    std::cout << "Telemetry validation, invalid/stale sensor data, strict tokens and timer wrap: PASS\n";
}
