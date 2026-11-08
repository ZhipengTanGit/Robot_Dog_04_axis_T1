#pragma once
#include "robot_telemetry.h"
#include <esp_err.h>
#include <mutex>

// UART1 owns only the STM32 link; UART0 remains the Xiaozhi console.
class RobotStm32 {
public:
    esp_err_t Start();
    std::string EnvironmentJson();
    std::string StatusJson();
    std::string Action(const std::string& name, unsigned duration_ms = 3000);
private:
    std::mutex state_mutex_, command_mutex_, tx_mutex_;
    robot::Telemetry telemetry_;
    uint32_t next_id_ = 0, pending_id_ = 0;
    bool ack_seen_ = false, ack_ok_ = false;
    std::string ack_reason_;
    bool leased_ = false, walking_ = false;
    uint32_t lease_deadline_ = 0, action_id_ = 0, lease_started_ = 0;
    bool started_ = false;
    void Task();
    void Line(const std::string& line);
    bool Write(const std::string& line);
    void MaintainLease(uint32_t now);
    uint32_t NextIdLocked();
};
