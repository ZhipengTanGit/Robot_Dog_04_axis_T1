#include <cstdint>
#include <string>
#include <mutex>
#include <vector>
#include <cassert>
#include <iostream>
#include <sstream>
#define private public
#include "robot_stm32.h"
#undef private
#include <driver/uart.h>
#include <cJSON.h>

static int64_t clock_us = 10000000;
static RobotStm32* robot_link;
static std::vector<std::string> sent;
static std::string ack = "OK";
int64_t esp_timer_get_time() { return clock_us; }
void vTaskDelay(uint32_t ticks) { clock_us += ticks * 1000; }
int xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, void*) { return 1; }
int uart_param_config(int, const uart_config_t*) { return 0; }
int uart_set_pin(int, int, int, int, int) { return 0; }
int uart_driver_install(int, int, int, int, void*, int) { return 0; }
int uart_driver_delete(int) { return 0; }
int uart_read_bytes(int, void*, unsigned, unsigned) { return 0; }
void BoardBus_RefreshStatus() {}
void Display_UpdateRobot(const char*, const char*) {}
int uart_write_bytes(int, const void* bytes, size_t size) {
    std::string line(static_cast<const char*>(bytes), size);
    sent.push_back(line);
    std::istringstream input(line);
    std::string prefix, command;
    uint32_t id;
    if (input >> prefix >> command >> id && ack != "MISSING") {
        robot_link->Line("PERIPH_ACK cmd=" + command + " id=" + std::to_string(id) +
            " ok=" + (ack == "OK" ? "1" : "0") + " reason=" + ack);
    }
    return static_cast<int>(size);
}
static void Feed(RobotStm32& link, uint32_t uptime=9000, unsigned allowed=1, unsigned valid=1) {
    link.Line("PERIPH t=" + std::to_string(uptime) +
        " aht_t_cC=2312 aht_rh_cPct=5678 aht_valid=" + std::to_string(valid) +
        " aht_age_ms=100 aht_state=READY servo_allowed=" + std::to_string(allowed) +
        " teach=0 last_id=70 walk_state=IDLE walk_id=0 walk_reason=NONE servo_mask=0");
}
int main() {
    RobotStm32 link;
    robot_link = &link;
    assert(link.Start() == 0);
    assert(link.Action("forward").find("STM32_OFFLINE") != std::string::npos && sent.empty());
    assert(link.Action("handshake").find("UNKNOWN_ACTION") != std::string::npos && sent.empty());
    Feed(link);
    assert(link.Action("forward", 0).find("DURATION_RANGE") != std::string::npos && sent.empty());
    auto env = cJSON_Parse(link.EnvironmentJson().c_str());
    assert(cJSON_IsTrue(cJSON_GetObjectItem(env,"ok")));
    assert(cJSON_GetObjectItem(env,"temperature_celsius")->valuedouble == 23.12);
    cJSON_Delete(env);
    Feed(link, 9000, 0);
    assert(link.Action("forward").find("BATTERY_INTERLOCK") != std::string::npos && sent.empty());
    assert(link.Action("stop").find("STOP_ACCEPTED") != std::string::npos && !link.leased_);
    Feed(link);
    ack = "TTL_NOT_READY";
    assert(link.Action("forward").find("TTL_NOT_READY") != std::string::npos && !link.leased_);
    ack = "OK";
    assert(link.Action("forward").find("ACCEPTED_NOT_COMPLETED") != std::string::npos && link.leased_);
    assert(sent.back().find(" 3000 100 0\r\n") != std::string::npos);
    auto count = sent.size();
    assert(link.Action("right").find("BUSY") != std::string::npos && sent.size() == count);
    clock_us += 400000;
    link.MaintainLease(10400);
    assert(sent.back().find("PERIPH KEEP ") == 0 && sent.back().find(" 9400\r\n") != std::string::npos);
    assert(link.Action("stop").find("STOP_ACCEPTED") != std::string::npos && !link.leased_);
    Feed(link, 9400);
    ack = "MISSING";
    assert(link.Action("forward").find("ACK_TIMEOUT_STOP_SENT") != std::string::npos);
    assert(sent.back() == "\r\nPERIPH STOP\r\n" && !link.leased_);
    ack = "OK";
    Feed(link, 10600);
    assert(link.Action("center",1000).find("ACCEPTED_NOT_COMPLETED") != std::string::npos);
    assert(sent.back().find(" 1500 1500 1500 1500\r\n") != std::string::npos);
    clock_us += 1000000;
    link.MaintainLease(static_cast<uint32_t>(clock_us/1000));
    assert(!link.leased_ && sent.back().find("PERIPH STOP ") == 0);
    Feed(link, 11600);
    assert(link.Action("pet").find("ACCEPTED_NOT_COMPLETED") != std::string::npos);
    assert(sent.back().find(" 6000 100 4\r\n") != std::string::npos);
    Feed(link, 500); // STM32 reset cancels the lease and the old ID namespace.
    assert(!link.leased_);
    assert(link.Action("stop").find("STOP_ACCEPTED") != std::string::npos);
    Feed(link, 500, 1, 0);
    env = cJSON_Parse(link.EnvironmentJson().c_str());
    assert(cJSON_IsFalse(cJSON_GetObjectItem(env,"ok")));
    assert(!cJSON_GetObjectItem(env,"temperature_celsius"));
    cJSON_Delete(env);
    clock_us += 3000000;
    assert(link.Action("forward").find("STM32_OFFLINE") != std::string::npos);
    std::cout << "STM32 action ACK/refusal/timeout-stop, lease, reset, sensor availability: PASS\n";
}
